#include "CalendarHourglassFooter.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "CalendarDate.h"
#include "CrossPointSettings.h"
#include "StickyNotesConfig.h"
#include "StickyNotesStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace calendar_app {

namespace {
constexpr bool TURN_OFF_SCREEN_AFTER_REFRESH = true;

int mondayFirstWeekday(const uint16_t year, const uint8_t month, const uint8_t day) {
  static constexpr int OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int adjustedYear = year;
  if (month < 3) --adjustedYear;
  const int sundayFirst =
      (adjustedYear + adjustedYear / 4 - adjustedYear / 100 + adjustedYear / 400 + OFFSETS[month - 1] + day) % 7;
  return (sundayFirst + 6) % 7;
}

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
#if CROSSINK_ENABLE_STICKY_NOTES
  if (!sticky_note::Store::recoverSnapshot()) return false;
#endif
  const uint32_t intervalSeconds = refreshIntervalSeconds(SETTINGS.calendarClockRefreshInterval);
  if (intervalSeconds == 0 || SETTINGS.calendarHourglassFooter == 0 || SETTINGS.clockDateHasBeenSynced == 0 ||
      SETTINGS.sleepScreen != CrossPointSettings::CALENDAR_SLEEP ||
      SETTINGS.stickyNoteLayout != CrossPointSettings::STICKY_NOTE_CALENDAR ||
      !currentLocalDateTime(year, month, day, hour, minute, second)) {
    return false;
  }

  char imagePath[64];
  if (!formatSleepImagePath(imagePath, sizeof(imagePath), year, month, day)) return false;
  // Empty dates have no stored bitmap. They are rendered directly from the
  // RTC date, while a missing bitmap for a real entry remains an error.
  if (!Storage.exists(imagePath) && sticky_note::Store::has(year, month, day)) return false;

  const uint32_t secondsOfDay = static_cast<uint32_t>(hour) * 60UL * 60UL +
                                static_cast<uint32_t>(minute) * 60UL + static_cast<uint32_t>(second);
  const uint32_t remainder = secondsOfDay % intervalSeconds;
  wakeSeconds = intervalSeconds - remainder;
  return wakeSeconds > 0;
}
}  // namespace

void drawEmptyCalendarScreen(const GfxRenderer& renderer, const uint16_t year, const uint8_t month,
                             const uint8_t day, const bool reserveHourglassFooter) {
  static constexpr StrId WEEKDAY_LABELS[] = {StrId::STR_WEEKDAY_MO, StrId::STR_WEEKDAY_TU, StrId::STR_WEEKDAY_WE,
                                             StrId::STR_WEEKDAY_TH, StrId::STR_WEEKDAY_FR, StrId::STR_WEEKDAY_SA,
                                             StrId::STR_WEEKDAY_SU};
  static constexpr const char* WEEKDAY_NAMES[] = {"Monday", "Tuesday", "Wednesday", "Thursday",
                                                  "Friday", "Saturday", "Sunday"};
  static constexpr const char* MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  renderer.clearScreen();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int sideInset = std::max(12, safeArea.width / 24);
  const int left = safeArea.x + sideInset;
  const int right = safeArea.x + safeArea.width - sideInset - 1;
  const int width = right - left + 1;

  char dateLine[40];
  const int selectedWeekday = mondayFirstWeekday(year, month, day);
  snprintf(dateLine, sizeof(dateLine), "%s, %s %02u %04u", WEEKDAY_NAMES[selectedWeekday], MONTH_NAMES[month - 1],
           static_cast<unsigned>(day), static_cast<unsigned>(year));
  const int dateY = safeArea.y + 16;
  UITheme::drawCenteredText(renderer, safeArea, UI_12_FONT_ID, dateY, dateLine, true, EpdFontFamily::BOLD);

  const int cellWidth = width / 7;
  const int weekdayY = dateY + renderer.getLineHeight(UI_12_FONT_ID) + 22;
  for (int column = 0; column < 7; ++column) {
    const char* label = I18N.get(WEEKDAY_LABELS[column]);
    const int labelX =
        left + column * cellWidth +
        (cellWidth - renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD)) / 2;
    renderer.drawText(UI_10_FONT_ID, labelX, weekdayY, label, true, EpdFontFamily::BOLD);
  }

  const int firstWeekday = mondayFirstWeekday(year, month, 1);
  const int monthDays = detail::daysInMonth(year, month);
  const uint8_t previousMonth = month == 1 ? 12 : month - 1;
  const uint16_t previousYear = month == 1 ? year - 1 : year;
  const int previousMonthDays = detail::daysInMonth(previousYear, previousMonth);
  const int rowCount = (firstWeekday + monthDays + 6) / 7;
  const int cellHeight = renderer.getLineHeight(UI_10_FONT_ID) + 14;
  const int calendarTop = weekdayY + renderer.getLineHeight(UI_10_FONT_ID) + 12;

  for (int cell = 0; cell < rowCount * 7; ++cell) {
    const int monthDay = cell - firstWeekday + 1;
    const bool inCurrentMonth = monthDay >= 1 && monthDay <= monthDays;
    const int shownDay = monthDay < 1 ? previousMonthDays + monthDay
                                     : (monthDay > monthDays ? monthDay - monthDays : monthDay);
    const int column = cell % 7;
    const int row = cell / 7;
    const int cellX = left + column * cellWidth;
    const int cellY = calendarTop + row * cellHeight;
    const bool selected = inCurrentMonth && monthDay == day;
    const int dayFont = inCurrentMonth ? UI_10_FONT_ID : SMALL_FONT_ID;
    const auto dayStyle = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    char dayText[3];
    snprintf(dayText, sizeof(dayText), "%d", shownDay);
    const int dayWidth = renderer.getTextWidth(dayFont, dayText, dayStyle);
    const int dayLineHeight = renderer.getLineHeight(dayFont);
    const int dayX = cellX + (cellWidth - dayWidth) / 2;
    const int dayY = cellY + (cellHeight - dayLineHeight) / 2;
    if (selected) {
      const int highlightWidth = std::min(cellWidth - 8, dayWidth + 20);
      renderer.fillRoundedRect(cellX + (cellWidth - highlightWidth) / 2, cellY + 2, highlightWidth, cellHeight - 4,
                               (cellHeight - 4) / 2, Color::LightGray);
    }
    renderer.drawText(dayFont, dayX, dayY, dayText, true, dayStyle);
    if (inCurrentMonth && sticky_note::Store::has(year, month, static_cast<uint8_t>(monthDay))) {
      constexpr int MARKER_SIZE = 4;
      renderer.fillRoundedRect(cellX + (cellWidth - MARKER_SIZE) / 2, cellY + cellHeight - MARKER_SIZE - 1,
                               MARKER_SIZE, MARKER_SIZE, MARKER_SIZE / 2, Color::Black);
    }
  }

  const int calendarBottom = calendarTop + rowCount * cellHeight;
  const int notesY = calendarBottom + 16;
  renderer.drawText(UI_10_FONT_ID, left, notesY, tr(STR_NOTES_TITLE), true, EpdFontFamily::BOLD);
  const int ruleY = notesY + renderer.getLineHeight(UI_10_FONT_ID) + 9;
  renderer.drawLine(left, ruleY, right, ruleY, 2, true);
  const int hourglassReserve = reserveHourglassFooter ? HOURGLASS_FOOTER_HEIGHT : 0;
  UITheme::drawCenteredText(renderer, Rect{safeArea.x, safeArea.y, safeArea.width, safeArea.height - hourglassReserve},
                            SMALL_FONT_ID, ruleY + 18, tr(STR_NO_ENTRIES));
}

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
  const int blockAreaWidth = availableWidth - HOURGLASS_GROUP_GAP;
  if (blockAreaWidth <= HOURGLASS_COLUMNS * HOURGLASS_BLOCK_MARGIN * 2 || footerTop < marginTop) return;

  renderer.fillRect(marginLeft, footerTop, screenWidth - marginLeft - marginRight, HOURGLASS_FOOTER_HEIGHT, false);

  for (int segment = 0; segment < HOURGLASS_COLUMNS * HOURGLASS_ROWS; ++segment) {
    const int row = segment / HOURGLASS_COLUMNS;
    const int column = segment % HOURGLASS_COLUMNS;
    const int groupGapLeft = column >= HOURGLASS_COLUMNS / 2 ? HOURGLASS_GROUP_GAP : 0;
    const int groupGapRight = column + 1 > HOURGLASS_COLUMNS / 2 ? HOURGLASS_GROUP_GAP : 0;
    const int slotLeft = footerLeft + column * blockAreaWidth / HOURGLASS_COLUMNS + groupGapLeft;
    const int slotRight = footerLeft + (column + 1) * blockAreaWidth / HOURGLASS_COLUMNS + groupGapRight;
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

  display.begin(true);
  renderer.begin();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  HalFile file;
  if (Storage.openFileForRead("CAL", imagePath, file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() != screenWidth ||
        bitmap.getHeight() != screenHeight) {
      LOG_ERR("CAL", "Timer wake found an invalid Calendar sleep image: %s", imagePath);
      file.close();
      display.deepSleep();
      return false;
    }
    renderer.clearScreen();
    renderer.drawBitmap(bitmap, 0, 0, screenWidth, screenHeight, 0, 0);
    file.close();
  } else if (!sticky_note::Store::has(year, month, day)) {
    drawEmptyCalendarScreen(renderer, year, month, day, true);
  } else {
    LOG_ERR("CAL", "Timer wake could not open Calendar sleep image: %s", imagePath);
    display.deepSleep();
    return false;
  }
  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }
  drawHourglassFooter(renderer, hour, minute);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, TURN_OFF_SCREEN_AFTER_REFRESH);
  display.deepSleep();

  const uint32_t elapsedSeconds = static_cast<uint32_t>((millis() - refreshStartedMs + 999UL) / 1000UL);
  nextWakeSeconds = nextWakeSeconds > elapsedSeconds ? nextWakeSeconds - elapsedSeconds : 1;
  return true;
}

}  // namespace calendar_app
