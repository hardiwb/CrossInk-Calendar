#pragma once

#include "StickyNotesConfig.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

namespace calendar_app {

class NotionConfig : public PersistableStore<NotionConfig> {
 private:
  std::string token_;
  std::string databaseId_;

  NotionConfig() = default;
  friend class PersistableStore<NotionConfig>;

 public:
  static const char* getFilePath() { return "/.crosspoint/notion-calendar.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool set(const std::string& token, const std::string& databaseInput, bool preserveToken);
  bool clear();

  const std::string& token() const { return token_; }
  const std::string& databaseId() const { return databaseId_; }
  bool hasToken() const { return !token_.empty(); }
  bool isConfigured() const { return hasToken() && !databaseId_.empty(); }

  static bool normalizeDatabaseId(const std::string& input, std::string& output);
};

#define NOTION_CALENDAR_CONFIG calendar_app::NotionConfig::getInstance()

}  // namespace calendar_app

#endif
