#include "NotionCalendarConfig.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cctype>
#include <utility>

namespace calendar_app {
namespace {
constexpr size_t MAX_TOKEN_BYTES = 256;

bool isHex(const char value) { return std::isxdigit(static_cast<unsigned char>(value)) != 0; }
}  // namespace

void NotionConfig::toJson(JsonDocument& doc) const {
  doc["token_obf"] = obfuscation::obfuscateToBase64(token_);
  doc["databaseId"] = databaseId_;
}

bool NotionConfig::fromJson(JsonVariantConst doc) {
  bool needsResave = false;
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  token_ = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::LEGACY && !token_.empty()) needsResave = true;
  if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY) {
    token_ = doc["token"] | "";
    if (!token_.empty()) needsResave = true;
  }
  databaseId_ = doc["databaseId"] | "";
  if (token_.empty() && databaseId_.empty()) return true;
  std::string normalized;
  if (token_.size() > MAX_TOKEN_BYTES || !normalizeDatabaseId(databaseId_, normalized)) {
    LOG_ERR("NCFG", "Ignoring invalid Notion Calendar configuration");
    token_.clear();
    databaseId_.clear();
    return false;
  }
  databaseId_ = std::move(normalized);
  if (needsResave) requestResave();
  return true;
}

bool NotionConfig::set(const std::string& token, const std::string& databaseInput, const bool preserveToken) {
  std::string normalized;
  if (!normalizeDatabaseId(databaseInput, normalized)) return false;
  if (!preserveToken) {
    if (token.empty() || token.size() > MAX_TOKEN_BYTES) return false;
    token_ = token;
  } else if (token_.empty()) {
    return false;
  }
  databaseId_ = std::move(normalized);
  return saveToFile();
}

bool NotionConfig::clear() {
  token_.clear();
  databaseId_.clear();
  return saveToFile();
}

bool NotionConfig::normalizeDatabaseId(const std::string& input, std::string& output) {
  // A Notion database URL ends in a 32-hex database ID, optionally followed by
  // a view query. Raw compact and hyphenated UUIDs are accepted too.
  for (size_t start = 0; start < input.size(); ++start) {
    if (start > 0 && isHex(input[start - 1])) continue;
    std::string hex;
    hex.reserve(32);
    size_t cursor = start;
    while (cursor < input.size() && hex.size() < 32) {
      const char value = input[cursor];
      if (isHex(value)) {
        hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
      } else if (value != '-') {
        break;
      }
      ++cursor;
    }
    if (hex.size() != 32) continue;
    if (cursor < input.size() && isHex(input[cursor])) continue;

    output.clear();
    output.reserve(36);
    for (size_t i = 0; i < hex.size(); ++i) {
      if (i == 8 || i == 12 || i == 16 || i == 20) output.push_back('-');
      output.push_back(hex[i]);
    }
    return true;
  }
  output.clear();
  return false;
}

}  // namespace calendar_app

#endif
