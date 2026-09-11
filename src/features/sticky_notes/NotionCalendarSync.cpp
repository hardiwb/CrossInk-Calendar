#include "NotionCalendarSync.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#ifdef SIMULATOR

namespace calendar_app {
NotionSyncResult syncFromNotion(const std::string&, const std::string&) {
  return {false, 0, 0, 0, "Notion sync is unavailable in the simulator"};
}
}  // namespace calendar_app

#else

#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "StickyNoteProtocol.h"
#include "StickyNotesStore.h"

namespace calendar_app {
namespace {
constexpr char LOG_TAG[] = "NSYNC";
constexpr char API_ROOT[] = "https://api.notion.com/v1";
constexpr char API_VERSION[] = "2026-03-11";
constexpr char RESPONSE_PATH[] = "/.crosspoint/notion-response.tmp";
constexpr size_t MAX_RESPONSE_BYTES = 256 * 1024;
constexpr size_t PAGE_SIZE = 25;
constexpr size_t MAX_PAGES = 400;

struct Schema {
  std::string dataSourceId;
  std::string titleName;
  std::string titleId;
  std::string dateName;
  std::string dateId;
};

struct HttpSink {
  std::string* memory = nullptr;
  FsFile* file = nullptr;
  size_t received = 0;
  size_t maximum = 0;
  bool ok = true;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
  auto* sink = static_cast<HttpSink*>(event->user_data);
  if (!sink) return ESP_FAIL;
  if (sink->received + static_cast<size_t>(event->data_len) > sink->maximum) {
    sink->ok = false;
    return ESP_FAIL;
  }

  const size_t length = static_cast<size_t>(event->data_len);
  if (sink->memory) {
    sink->memory->append(static_cast<const char*>(event->data), length);
  } else if (!sink->file || sink->file->write(static_cast<const uint8_t*>(event->data), length) != length) {
    sink->ok = false;
    return ESP_FAIL;
  }
  sink->received += length;
  return ESP_OK;
}

class NotionHttp {
 public:
  explicit NotionHttp(const std::string& token) {
    esp_http_client_config_t config = {};
    config.url = API_ROOT;
    config.event_handler = onHttpEvent;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.timeout_ms = 20000;
    config.user_data = &sink_;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = true;
    client_ = esp_http_client_init(&config);
    if (!client_) return;
    const std::string authorization = "Bearer " + token;
    headersOk_ = esp_http_client_set_header(client_, "Authorization", authorization.c_str()) == ESP_OK &&
                 esp_http_client_set_header(client_, "Notion-Version", API_VERSION) == ESP_OK &&
                 esp_http_client_set_header(client_, "Content-Type", "application/json") == ESP_OK &&
                 esp_http_client_set_header(client_, "Accept", "application/json") == ESP_OK &&
                 esp_http_client_set_header(client_, "User-Agent", "CrossInk-Notion-Calendar") == ESP_OK;
  }

  ~NotionHttp() {
    if (client_) esp_http_client_cleanup(client_);
  }

  NotionHttp(const NotionHttp&) = delete;
  NotionHttp& operator=(const NotionHttp&) = delete;

  bool valid() const { return client_ && headersOk_; }
  const std::string& lastError() const { return lastError_; }

  int request(const char* method, const std::string& url, const std::string& payload, std::string* memory,
              FsFile* file, const size_t maximum, bool& complete) {
    complete = false;
    lastError_.clear();
    if (!valid()) {
      lastError_ = "Could not initialize the secure Notion client";
      return -1;
    }
    sink_ = {memory, file, 0, maximum, true};
    if (memory) memory->clear();
    if (esp_http_client_set_url(client_, url.c_str()) != ESP_OK) {
      lastError_ = "Could not prepare the Notion URL";
      return -1;
    }

    const bool isPost = strcmp(method, "POST") == 0;
    if (esp_http_client_set_method(client_, isPost ? HTTP_METHOD_POST : HTTP_METHOD_GET) != ESP_OK) {
      lastError_ = "Could not prepare the Notion request";
      return -1;
    }
    // A GET has no request body. Passing nullptr to set_post_field() also asks
    // ESP-IDF to remove Content-Type, which can fail and is unnecessary here.
    if (isPost &&
        esp_http_client_set_post_field(client_, payload.data(), static_cast<int>(payload.size())) != ESP_OK) {
      lastError_ = "Could not prepare the Notion request body";
      return -1;
    }
    const esp_err_t requestError = esp_http_client_perform(client_);
    if (requestError != ESP_OK) {
      int tlsError = 0;
      int tlsFlags = 0;
      const esp_err_t tlsResult = esp_http_client_get_and_clear_last_tls_error(client_, &tlsError, &tlsFlags);
      LOG_ERR(LOG_TAG, "Notion HTTP request failed: %s", esp_err_to_name(requestError));
      if (tlsResult != ESP_OK || tlsError != 0 || tlsFlags != 0) {
        const int tlsCode = tlsError < 0 ? -tlsError : tlsError;
        LOG_ERR(LOG_TAG, "Notion TLS error: err=%s mbedtls=0x%x flags=0x%x", esp_err_to_name(tlsResult), tlsCode,
                tlsFlags);
      }
      lastError_ = std::string("Notion connection failed (") + esp_err_to_name(requestError) + ")";
      return -1;
    }
    if (!sink_.ok) {
      lastError_ = "Notion response was too large or could not be saved";
      return -1;
    }
    complete = true;
    return esp_http_client_get_status_code(client_);
  }

 private:
  esp_http_client_handle_t client_ = nullptr;
  HttpSink sink_;
  bool headersOk_ = false;
  std::string lastError_;
};

std::string apiUrl(const char* resource, const std::string& id, const char* suffix = "") {
  return std::string(API_ROOT) + "/" + resource + "/" + id + suffix;
}

bool requestJson(NotionHttp& http, const std::string& url, const char* method, const std::string& payload,
                 JsonDocument& doc, std::string& error) {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) {
    error = "Could not prepare calendar storage";
    return false;
  }
  if (Storage.exists(RESPONSE_PATH)) Storage.remove(RESPONSE_PATH);
  FsFile response = Storage.open(RESPONSE_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!response) {
    error = "Could not open the Notion response file";
    return false;
  }
  bool complete = false;
  const int status = http.request(method, url, payload, nullptr, &response, 64 * 1024, complete);
  const bool writeOk = complete && response.sync();
  response.close();
  if (status < 0 || !writeOk) {
    Storage.remove(RESPONSE_PATH);
    error = http.lastError().empty() ? "Could not reach Notion" : http.lastError();
    return false;
  }

  FsFile input;
  if (!Storage.openFileForRead(LOG_TAG, RESPONSE_PATH, input)) {
    Storage.remove(RESPONSE_PATH);
    error = "Could not read the Notion response";
    return false;
  }
  if (status != 200) {
    JsonDocument errorDoc;
    deserializeJson(errorDoc, input);
    input.close();
    Storage.remove(RESPONSE_PATH);
    const char* notionMessage = errorDoc["message"] | "";
    error = notionMessage[0] ? notionMessage : "Notion rejected the request";
    return false;
  }
  const DeserializationError parseError = deserializeJson(doc, input);
  input.close();
  Storage.remove(RESPONSE_PATH);
  if (parseError) {
    error = "Notion returned invalid JSON";
    return false;
  }
  return true;
}

bool discoverSchema(NotionHttp& http, const std::string& databaseId, Schema& schema, std::string& error) {
  JsonDocument database;
  if (!requestJson(http, apiUrl("databases", databaseId), "GET", "", database, error)) return false;
  JsonArrayConst sources = database["data_sources"].as<JsonArrayConst>();
  if (sources.size() == 0) {
    error = "The Notion database has no data source";
    return false;
  }
  if (sources.size() > 1) {
    error = "The Notion database has multiple data sources; use a database with one agenda source";
    return false;
  }
  schema.dataSourceId = sources[0]["id"] | "";
  if (schema.dataSourceId.empty()) {
    error = "Notion did not return a data source ID";
    return false;
  }

  JsonDocument source;
  if (!requestJson(http, apiUrl("data_sources", schema.dataSourceId), "GET", "", source, error)) return false;
  for (JsonPairConst property : source["properties"].as<JsonObjectConst>()) {
    const char* type = property.value()["type"] | "";
    if (schema.titleName.empty() && strcmp(type, "title") == 0) {
      schema.titleName = property.key().c_str();
      schema.titleId = property.value()["id"] | "";
    } else if (schema.dateName.empty() && strcmp(type, "date") == 0) {
      schema.dateName = property.key().c_str();
      schema.dateId = property.value()["id"] | "";
    }
  }
  if (schema.titleName.empty() || schema.dateName.empty()) {
    error = "The agenda needs a title property and a date property";
    return false;
  }
  return true;
}

std::string urlEncode(const std::string& value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(HEX_DIGITS[c >> 4]);
      encoded.push_back(HEX_DIGITS[c & 0x0F]);
    }
  }
  return encoded;
}

bool queryPage(NotionHttp& http, const Schema& schema, const std::string& cursor, JsonDocument& output,
               std::string& error) {
  JsonDocument request;
  JsonArray filters = request["filter"]["and"].to<JsonArray>();
  JsonObject after = filters.add<JsonObject>();
  after["property"] = schema.dateName;
  after["date"]["on_or_after"] = "2024-01-01";
  JsonObject before = filters.add<JsonObject>();
  before["property"] = schema.dateName;
  before["date"]["before"] = "2100-01-01";
  JsonObject sort = request["sorts"].add<JsonObject>();
  sort["property"] = schema.dateName;
  sort["direction"] = "ascending";
  request["page_size"] = PAGE_SIZE;
  if (!cursor.empty()) request["start_cursor"] = cursor;
  std::string payload;
  serializeJson(request, payload);

  const std::string url = apiUrl("data_sources", schema.dataSourceId, "/query") +
                          "?filter_properties%5B%5D=" + urlEncode(schema.titleId) +
                          "&filter_properties%5B%5D=" + urlEncode(schema.dateId);
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) {
    error = "Could not prepare calendar storage";
    return false;
  }
  if (Storage.exists(RESPONSE_PATH)) Storage.remove(RESPONSE_PATH);
  FsFile response = Storage.open(RESPONSE_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!response) {
    error = "Could not open the Notion response file";
    return false;
  }
  bool complete = false;
  const int status = http.request("POST", url, payload, nullptr, &response, MAX_RESPONSE_BYTES, complete);
  const bool writeOk = complete && response.sync();
  response.close();

  if (status != 200 || !writeOk) {
    Storage.remove(RESPONSE_PATH);
    if (status < 0 && !http.lastError().empty()) {
      error = http.lastError();
    } else {
      error = status == 401 || status == 403 ? "Notion denied access; check the token and database sharing"
                                             : "Notion query failed or returned too much data";
    }
    return false;
  }

  FsFile input;
  if (!Storage.openFileForRead(LOG_TAG, RESPONSE_PATH, input)) {
    Storage.remove(RESPONSE_PATH);
    error = "Could not read the Notion response";
    return false;
  }
  JsonDocument filter;
  filter["has_more"] = true;
  filter["next_cursor"] = true;
  filter["results"][0]["properties"][schema.titleName]["title"] = true;
  filter["results"][0]["properties"][schema.dateName]["date"]["start"] = true;
  const DeserializationError parseError =
      deserializeJson(output, input, DeserializationOption::Filter(filter.as<JsonVariantConst>()));
  input.close();
  Storage.remove(RESPONSE_PATH);
  if (parseError) {
    error = "Could not parse the Notion agenda";
    return false;
  }
  return true;
}

bool parseDate(const char* start, uint16_t& year, uint8_t& month, uint8_t& day) {
  if (!start || strlen(start) < 10 || start[4] != '-' || start[7] != '-') return false;
  unsigned parsedYear = 0;
  unsigned parsedMonth = 0;
  unsigned parsedDay = 0;
  if (sscanf(start, "%4u-%2u-%2u", &parsedYear, &parsedMonth, &parsedDay) != 3 ||
      !sticky_note::validDate(parsedYear, parsedMonth, parsedDay)) {
    return false;
  }
  year = static_cast<uint16_t>(parsedYear);
  month = static_cast<uint8_t>(parsedMonth);
  day = static_cast<uint8_t>(parsedDay);
  return true;
}

std::string pageTitle(JsonVariantConst page, const Schema& schema) {
  std::string title;
  for (JsonObjectConst part : page["properties"][schema.titleName]["title"].as<JsonArrayConst>()) {
    const char* text = part["plain_text"] | "";
    if (title.size() + strlen(text) > sticky_note::MAX_MESSAGE_BYTES) break;
    title += text;
  }
  return title.empty() ? "Untitled" : title;
}

bool appendEntry(sticky_note::Note& note, const char* dateStart, const std::string& title, size_t& truncatedDays) {
  char timePrefix[7] = {};
  const bool hasTime = strlen(dateStart) >= 16 && dateStart[10] == 'T';
  if (hasTime) {
    snprintf(timePrefix, sizeof(timePrefix), "%c%c:%c%c ", dateStart[11], dateStart[12], dateStart[14], dateStart[15]);
  }
  const size_t prefixLength = hasTime ? strlen(timePrefix) : 0;
  const size_t separator = note.messageLength == 0 ? 0 : 1;
  if (note.messageLength + separator + prefixLength + title.size() > sticky_note::MAX_MESSAGE_BYTES) {
    ++truncatedDays;
    return false;
  }
  if (separator) note.message[note.messageLength++] = '\n';
  if (prefixLength) {
    memcpy(note.message.data() + note.messageLength, timePrefix, prefixLength);
    note.messageLength += static_cast<uint16_t>(prefixLength);
  }
  memcpy(note.message.data() + note.messageLength, title.data(), title.size());
  note.messageLength += static_cast<uint16_t>(title.size());
  note.message[note.messageLength] = '\0';
  return true;
}
}  // namespace

NotionSyncResult syncFromNotion(const std::string& token, const std::string& databaseId) {
  NotionSyncResult result;
  // Certificate validation uses the ESP32 system clock, which is separate
  // from the external RTC used by the Calendar screen after a cold boot. The
  // RTC is the fast path; NTP remains a fallback for devices without one.
  if (!halClock.syncSystemTimeFromRTC() && !halClock.syncSystemTimeFromNTP()) {
    result.message = "Could not set the device time for the secure Notion connection";
    return result;
  }
  NotionHttp http(token);
  if (!http.valid()) {
    result.message = "Could not initialize secure Notion access";
    return result;
  }

  Schema schema;
  if (!discoverSchema(http, databaseId, schema, result.message)) return result;
  if (!sticky_note::Store::beginSnapshot()) {
    result.message = "Could not start the calendar import";
    return result;
  }

  // A calendar note is just over 2 KB. Keep one fallible, reusable heap object
  // instead of consuming that much of the web-server task's stack.
  auto pending = makeUniqueNoThrow<sticky_note::Note>();
  if (!pending) {
    sticky_note::Store::abortSnapshot();
    result.message = "Not enough memory to import the calendar";
    return result;
  }
  bool havePending = false;
  std::string cursor;
  bool hasMore = true;
  size_t pageCount = 0;
  while (hasMore && pageCount++ < MAX_PAGES) {
    JsonDocument page;
    if (!queryPage(http, schema, cursor, page, result.message)) {
      sticky_note::Store::abortSnapshot();
      return result;
    }
    for (JsonVariantConst item : page["results"].as<JsonArrayConst>()) {
      const char* dateStart = item["properties"][schema.dateName]["date"]["start"] | "";
      uint16_t year = 0;
      uint8_t month = 0;
      uint8_t day = 0;
      if (!parseDate(dateStart, year, month, day)) continue;

      if (havePending && (pending->year != year || pending->month != month || pending->day != day)) {
        if (!sticky_note::Store::saveSnapshot(*pending)) {
          sticky_note::Store::abortSnapshot();
          result.message = "Could not save the imported calendar";
          return result;
        }
        ++result.importedDays;
        *pending = {};
        havePending = false;
      }
      if (!havePending) {
        pending->year = year;
        pending->month = month;
        pending->day = day;
        havePending = true;
      }
      if (appendEntry(*pending, dateStart, pageTitle(item, schema), result.truncatedDays)) ++result.importedEntries;
    }
    hasMore = page["has_more"] | false;
    cursor = page["next_cursor"] | "";
    if (hasMore && cursor.empty()) {
      sticky_note::Store::abortSnapshot();
      result.message = "Notion pagination was incomplete";
      return result;
    }
  }
  if (hasMore) {
    sticky_note::Store::abortSnapshot();
    result.message = "The Notion agenda exceeds the 10,000-entry sync limit";
    return result;
  }
  if (havePending) {
    if (!sticky_note::Store::saveSnapshot(*pending)) {
      sticky_note::Store::abortSnapshot();
      result.message = "Could not save the imported calendar";
      return result;
    }
    ++result.importedDays;
  }
  if (!sticky_note::Store::commitSnapshot()) {
    sticky_note::Store::abortSnapshot();
    result.message = "Could not install the imported calendar";
    return result;
  }

  result.success = true;
  result.message = "Notion calendar synchronized";
  LOG_INF(LOG_TAG, "Imported %u Notion entries across %u days", static_cast<unsigned>(result.importedEntries),
          static_cast<unsigned>(result.importedDays));
  return result;
}

}  // namespace calendar_app

#endif
#endif
