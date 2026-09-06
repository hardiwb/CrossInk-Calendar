#include "CalendarHourglassFooter.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CalendarDate.h"
#include "CrossPointSettings.h"
#include "StickyNotesConfig.h"

namespace calendar_app {

namespace {
constexpr bool TURN_OFF_SCREEN_AFTER_REFRESH = true;

uint32_t refreshIntervalSeconds(const uint8_t interval) {
  switch (interval) {
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_30_MINUTES:
      return 30UL * 60UL;
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_1_HOUR:
      return 60UL * 60UL;
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_3_HOURS:
      return 3UL * 60UL * 60UL;
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_6_HOURS:
      return 6UL * 60UL * 60UL;
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_12_HOURS:
      return 12UL * 60UL * 60UL;
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_24_HOURS:
      return 24UL * 60UL * 60UL;
    case CrossPointSettings::CALENDAR_CLOCK_REFRESH_NEVER:
    default:
      return 0;
  }
}

bool currentCalendarSleepState(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                               uint8_t& second, uint32_t& wakeSeconds) {
  const uint32_t intervalSeconds = refreshIntervalSeconds(SETTINGS.calendarClockRefreshInterval);
  if (intervalSeconds == 0 || SETTINGS.calendarHourglassFooter == 0 || SETTINGS.clockDateHasBeenSynced == 0 ||
      SETTINGS.sleepScreen != CrossPointSettings::CALENDAR_SLEEP ||
      SETTINGS.stickyNoteLayout != CrossPointSettings::STICKY_NOTE_CALENDAR ||
      !currentLocalDateTime(year, month, day, hour, minute, second)) {
    return false;
  }

  char imagePath[64];
  if (!formatSleepImagePath(imagePath, sizeof(imagePath), year, month, day) || !Storage.exists(imagePath)) {
    return false;
  }

  const uint32_t secondsOfDay = static_cast<uint32_t>(hour) * 60UL * 60UL +
                                static_cast<uint32_t>(minute) * 60UL + static_cast<uint32_t>(second);
  const uint32_t remainder = secondsOfDay % intervalSeconds;
  wakeSeconds = intervalSeconds - remainder;
  return wakeSeconds > 0;
}
}  // namespace

void drawHourglassFooter(const GfxRenderer& renderer, const uint8_t hour, const uint8_t minute) {
  if (hour >= 24 || minute >= 60) return;

  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);

  const int footerBottom = renderer.getScreenHeight() - marginBottom;
  const int footerTop = footerBottom - HOURGLASS_FOOTER_HEIGHT;
  const int screenWidth = renderer.getScreenWidth();
  const int noteSideInset = std::max(12, screenWidth / 24);
  const int footerLeft = std::max(marginLeft, noteSideInset);
  const int footerRight = std::min(screenWidth - marginRight, screenWidth - noteSideInset);
  const int availableWidth = footerRight - footerLeft;
  if (availableWidth <= HOURGLASS_COLUMNS * HOURGLASS_BLOCK_MARGIN * 2 || footerTop < marginTop) return;

  renderer.fillRect(marginLeft, footerTop, screenWidth - marginLeft - marginRight, HOURGLASS_FOOTER_HEIGHT, false);

  for (int segment = 0; segment < HOURGLASS_COLUMNS * HOURGLASS_ROWS; ++segment) {
    const int row = segment / HOURGLASS_COLUMNS;
    const int column = segment % HOURGLASS_COLUMNS;
    const int slotLeft = footerLeft + column * availableWidth / HOURGLASS_COLUMNS;
    const int slotRight = footerLeft + (column + 1) * availableWidth / HOURGLASS_COLUMNS;
    const int x = slotLeft + HOURGLASS_BLOCK_MARGIN;
    const int y = footerTop + row * (HOURGLASS_BLOCK_HEIGHT + HOURGLASS_BLOCK_MARGIN * 2) +
                  HOURGLASS_BLOCK_MARGIN;
    const int width = slotRight - slotLeft - HOURGLASS_BLOCK_MARGIN * 2;
    if (width <= 0) continue;

    if (segment > hour || (segment == hour && minute < 30)) {
      renderer.fillRoundedRect(x, y, width, HOURGLASS_BLOCK_HEIGHT, HOURGLASS_CORNER_RADIUS, Color::Black);
      continue;
    }

    if (segment == hour) {
      renderer.fillRoundedRect(x, y, width, HOURGLASS_BLOCK_HEIGHT, HOURGLASS_CORNER_RADIUS, Color::DarkGray);
      continue;
    }
  }
}

bool calendarHourglassWakeDelay(uint32_t& wakeSeconds) {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  wakeSeconds = 0;
  return currentCalendarSleepState(year, month, day, hour, minute, second, wakeSeconds);
}

bool refreshCalendarHourglassAfterTimerWake(GfxRenderer& renderer, HalDisplay& display, uint32_t& nextWakeSeconds) {
  const unsigned long refreshStartedMs = millis();
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  if (!currentCalendarSleepState(year, month, day, hour, minute, second, nextWakeSeconds)) {
    LOG_ERR("CAL", "Timer wake ignored because the Calendar refresh state is not valid");
    return false;
  }

  char imagePath[64];
  if (!formatSleepImagePath(imagePath, sizeof(imagePath), year, month, day)) return false;

  HalFile file;
  if (!Storage.openFileForRead("CAL", imagePath, file)) return false;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("CAL", "Timer wake found an invalid Calendar sleep image: %s", imagePath);
    file.close();
    return false;
  }

  display.begin(true);
  renderer.begin();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (bitmap.getWidth() != screenWidth || bitmap.getHeight() != screenHeight) {
    LOG_ERR("CAL", "Timer wake Calendar image dimensions do not match the display");
    file.close();
    display.deepSleep();
    return false;
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, 0, 0, screenWidth, screenHeight, 0, 0);
  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }
  drawHourglassFooter(renderer, hour, minute);
  file.close();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_REFRESH);
  display.deepSleep();

  const uint32_t elapsedSeconds = static_cast<uint32_t>((millis() - refreshStartedMs + 999UL) / 1000UL);
  nextWakeSeconds = nextWakeSeconds > elapsedSeconds ? nextWakeSeconds - elapsedSeconds : 1;
  return true;
}

}  // namespace calendar_app
