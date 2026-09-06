#include "StickyNotesStore.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace sticky_note {
namespace {
constexpr char LOG_TAG[] = "NCAL";
constexpr char STORE_ROOT[] = "/.crosspoint/calendar";
constexpr char SNAPSHOT_ROOT[] = "/.crosspoint/calendar.snapshot";
constexpr char BACKUP_ROOT[] = "/.crosspoint/calendar.backup";
constexpr std::array<uint8_t, 4> FILE_MAGIC = {'C', 'A', 'L', 'S'};
constexpr uint8_t FILE_VERSION = 1;
constexpr size_t FILE_HEADER_BYTES = 12;
constexpr size_t PATH_BYTES = 64;

bool installTempFile(const char* path, const char* tempPath, const char* backupPath) {
  bool hadExisting = Storage.exists(path);
  if (!hadExisting && Storage.exists(backupPath)) {
    if (!Storage.rename(backupPath, path)) {
      LOG_ERR(LOG_TAG, "Failed to recover calendar note backup: %s", path);
      Storage.remove(tempPath);
      return false;
    }
    hadExisting = true;
  }
  if (Storage.exists(backupPath) && hadExisting) Storage.remove(backupPath);
  if (hadExisting && !Storage.rename(path, backupPath)) {
    LOG_ERR(LOG_TAG, "Failed to back up calendar note: %s", path);
    Storage.remove(tempPath);
    return false;
  }
  if (!Storage.rename(tempPath, path)) {
    LOG_ERR(LOG_TAG, "Failed to install calendar note: %s", path);
    if (hadExisting) Storage.rename(backupPath, path);
    Storage.remove(tempPath);
    return false;
  }
  if (Storage.exists(backupPath)) Storage.remove(backupPath);
  return true;
}
}  // namespace

bool Store::formatPath(char* output, const size_t outputSize, const char* root, const uint16_t year,
                       const uint8_t month, const uint8_t day, const char* suffix) {
  if (!output || outputSize == 0 || !root || !suffix || !validDate(year, month, day)) return false;
  const int length = snprintf(output, outputSize, "%s/%04u-%02u-%02u.bin%s", root,
                              static_cast<unsigned>(year), static_cast<unsigned>(month), static_cast<unsigned>(day),
                              suffix);
  return length > 0 && static_cast<size_t>(length) < outputSize;
}

bool Store::saveToRoot(const Note& note, const char* root) {
  if (note.messageLength == 0 || note.messageLength > MAX_MESSAGE_BYTES ||
      !validDate(note.year, note.month, note.day) ||
      !validUtf8(reinterpret_cast<const uint8_t*>(note.message.data()), note.messageLength)) {
    LOG_ERR(LOG_TAG, "Refusing invalid calendar note");
    return false;
  }
  if (!root || (!Storage.exists(root) && !Storage.mkdir(root))) {
    LOG_ERR(LOG_TAG, "Failed to create calendar directory: %s", root ? root : "(null)");
    return false;
  }

  char path[PATH_BYTES];
  char tempPath[PATH_BYTES];
  char backupPath[PATH_BYTES];
  if (!formatPath(path, sizeof(path), root, note.year, note.month, note.day) ||
      !formatPath(tempPath, sizeof(tempPath), root, note.year, note.month, note.day, ".tmp") ||
      !formatPath(backupPath, sizeof(backupPath), root, note.year, note.month, note.day, ".bak")) {
    LOG_ERR(LOG_TAG, "Failed to build calendar note path");
    return false;
  }
  if (Storage.exists(tempPath)) Storage.remove(tempPath);

  FsFile file = Storage.open(tempPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR(LOG_TAG, "Failed to open calendar temp file: %s", tempPath);
    return false;
  }
  std::array<uint8_t, FILE_HEADER_BYTES> header{};
  std::copy(FILE_MAGIC.begin(), FILE_MAGIC.end(), header.begin());
  header[4] = FILE_VERSION;
  writeU16Le(header.data() + 6, note.messageLength);
  writeU32Le(header.data() + 8,
             crc32(reinterpret_cast<const uint8_t*>(note.message.data()), note.messageLength));
  const bool wrote = file.write(header.data(), header.size()) == header.size() &&
                     file.write(reinterpret_cast<const uint8_t*>(note.message.data()), note.messageLength) ==
                         note.messageLength &&
                     file.sync();
  file.close();
  if (!wrote) {
    LOG_ERR(LOG_TAG, "Failed to write calendar note: %s", tempPath);
    Storage.remove(tempPath);
    return false;
  }
  if (!installTempFile(path, tempPath, backupPath)) return false;
  LOG_INF(LOG_TAG, "Saved calendar note %04u-%02u-%02u (%u bytes)", static_cast<unsigned>(note.year),
          static_cast<unsigned>(note.month), static_cast<unsigned>(note.day),
          static_cast<unsigned>(note.messageLength));
  return true;
}

bool Store::save(const Note& note) {
  // A web edit can replace the text without rendering a new sleep image. Drop
  // the old bitmap so the device never shows content from the previous entry.
  char sleepImagePath[PATH_BYTES];
  if (calendar_app::formatSleepImagePath(sleepImagePath, sizeof(sleepImagePath), note.year, note.month, note.day) &&
      Storage.exists(sleepImagePath) && !Storage.remove(sleepImagePath)) {
    LOG_ERR(LOG_TAG, "Failed to invalidate calendar sleep image: %s", sleepImagePath);
    return false;
  }
  return saveToRoot(note, STORE_ROOT);
}

bool Store::load(const uint16_t year, const uint8_t month, const uint8_t day, Note& note) {
  char path[PATH_BYTES];
  if (!formatPath(path, sizeof(path), STORE_ROOT, year, month, day)) return false;
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file)) return false;

  std::array<uint8_t, FILE_HEADER_BYTES> header{};
  const bool headerRead = file.read(header.data(), header.size()) == static_cast<int>(header.size());
  const uint16_t messageLength = headerRead ? readU16Le(header.data() + 6) : 0;
  const bool validHeader = headerRead && std::equal(FILE_MAGIC.begin(), FILE_MAGIC.end(), header.begin()) &&
                           header[4] == FILE_VERSION && header[5] == 0 && messageLength > 0 &&
                           messageLength <= MAX_MESSAGE_BYTES && file.fileSize() == FILE_HEADER_BYTES + messageLength;
  if (!validHeader || file.read(note.message.data(), messageLength) != static_cast<int>(messageLength)) {
    file.close();
    LOG_ERR(LOG_TAG, "Invalid calendar note file: %s", path);
    return false;
  }
  file.close();

  const auto* text = reinterpret_cast<const uint8_t*>(note.message.data());
  if (crc32(text, messageLength) != readU32Le(header.data() + 8) || !validUtf8(text, messageLength)) {
    LOG_ERR(LOG_TAG, "Calendar note checksum/text failed: %s", path);
    return false;
  }
  note.sequence = 0;
  note.year = year;
  note.month = month;
  note.day = day;
  note.messageLength = messageLength;
  note.message[messageLength] = '\0';
  return true;
}

bool Store::has(const uint16_t year, const uint8_t month, const uint8_t day) {
  char path[PATH_BYTES];
  return formatPath(path, sizeof(path), STORE_ROOT, year, month, day) && Storage.exists(path);
}

bool Store::remove(const uint16_t year, const uint8_t month, const uint8_t day) {
  char path[PATH_BYTES];
  if (!formatPath(path, sizeof(path), STORE_ROOT, year, month, day)) return false;
  const bool removedNote = !Storage.exists(path) || Storage.remove(path);

  char sleepImagePath[PATH_BYTES];
  const bool formattedImage =
      calendar_app::formatSleepImagePath(sleepImagePath, sizeof(sleepImagePath), year, month, day);
  const bool removedImage = !formattedImage || !Storage.exists(sleepImagePath) || Storage.remove(sleepImagePath);
  if (!removedNote || !removedImage) {
    LOG_ERR(LOG_TAG, "Failed to remove calendar entry %04u-%02u-%02u", static_cast<unsigned>(year),
            static_cast<unsigned>(month), static_cast<unsigned>(day));
  }
  return removedNote && removedImage;
}

bool Store::recoverSnapshot() {
  const bool liveExists = Storage.exists(STORE_ROOT);
  const bool backupExists = Storage.exists(BACKUP_ROOT);
  if (!liveExists && backupExists && !Storage.rename(BACKUP_ROOT, STORE_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to restore Calendar snapshot backup");
    return false;
  }
  if (Storage.exists(STORE_ROOT) && Storage.exists(BACKUP_ROOT) && !Storage.removeDir(BACKUP_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to remove committed Calendar snapshot backup");
    return false;
  }
  if (Storage.exists(SNAPSHOT_ROOT) && !Storage.removeDir(SNAPSHOT_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to remove incomplete Calendar snapshot");
    return false;
  }
  return true;
}

bool Store::beginSnapshot() {
  if (!recoverSnapshot()) return false;
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) {
    LOG_ERR(LOG_TAG, "Failed to create Calendar snapshot parent directory");
    return false;
  }
  if (!Storage.mkdir(SNAPSHOT_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to create Calendar snapshot directory");
    return false;
  }
  return true;
}

bool Store::saveSnapshot(const Note& note) { return saveToRoot(note, SNAPSHOT_ROOT); }

bool Store::commitSnapshot() {
  if (!Storage.exists(SNAPSHOT_ROOT)) {
    LOG_ERR(LOG_TAG, "Calendar snapshot staging directory is missing");
    return false;
  }
  if (Storage.exists(BACKUP_ROOT) && !Storage.removeDir(BACKUP_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to clear old Calendar snapshot backup");
    return false;
  }

  const bool hadLiveCalendar = Storage.exists(STORE_ROOT);
  if (hadLiveCalendar && !Storage.rename(STORE_ROOT, BACKUP_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to back up live Calendar directory");
    return false;
  }
  if (!Storage.rename(SNAPSHOT_ROOT, STORE_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to install Calendar snapshot");
    if (hadLiveCalendar && !Storage.rename(BACKUP_ROOT, STORE_ROOT)) {
      LOG_ERR(LOG_TAG, "Failed to restore live Calendar directory after snapshot failure");
    }
    return false;
  }
  if (hadLiveCalendar && Storage.exists(BACKUP_ROOT) && !Storage.removeDir(BACKUP_ROOT)) {
    LOG_ERR(LOG_TAG, "Calendar snapshot installed but its backup could not be removed");
  }
  return true;
}

void Store::abortSnapshot() {
  if (Storage.exists(SNAPSHOT_ROOT) && !Storage.removeDir(SNAPSHOT_ROOT)) {
    LOG_ERR(LOG_TAG, "Failed to discard incomplete Calendar snapshot");
  }
}

const char* Store::snapshotRoot() { return SNAPSHOT_ROOT; }

}  // namespace sticky_note

#endif  // CROSSINK_ENABLE_STICKY_NOTES
