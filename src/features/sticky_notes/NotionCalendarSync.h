#pragma once

#include "StickyNotesConfig.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#include <cstddef>
#include <string>

namespace calendar_app {

struct NotionSyncResult {
  bool success = false;
  size_t importedEntries = 0;
  size_t importedDays = 0;
  size_t truncatedDays = 0;
  std::string message;
};

NotionSyncResult syncFromNotion(const std::string& token, const std::string& databaseId);

}  // namespace calendar_app

#endif
