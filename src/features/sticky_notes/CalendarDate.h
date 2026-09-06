#pragma once

#include <HalClock.h>

#include <algorithm>
#include <cstdint>

#include "CrossPointSettings.h"
#include "StickyNoteProtocol.h"

namespace calendar_app {
namespace detail {
inline bool isLeapYear(const uint16_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && isLeapYear(year) ? 29 : DAYS[month - 1];
}
}  // namespace detail

inline bool currentLocalDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                                 uint8_t& second) {
  if (!halClock.isAvailable()) return false;

  if (!halClock.getDateTime(year, month, day, hour, minute, second) || !sticky_note::validDate(year, month, day)) {
    return false;
  }

  const uint8_t offsetQ = std::min<uint8_t>(SETTINGS.clockUtcOffsetQ, 104);
  int localMinutes = static_cast<int>(hour) * 60 + minute + (static_cast<int>(offsetQ) - 48) * 15;
  if (localMinutes < 0) {
    if (day > 1) {
      --day;
    } else if (month > 1) {
      --month;
      day = detail::daysInMonth(year, month);
    } else {
      --year;
      month = 12;
      day = 31;
    }
    localMinutes += 24 * 60;
  } else if (localMinutes >= 24 * 60) {
    if (day < detail::daysInMonth(year, month)) {
      ++day;
    } else if (month < 12) {
      ++month;
      day = 1;
    } else {
      ++year;
      month = 1;
      day = 1;
    }
    localMinutes -= 24 * 60;
  }
  hour = static_cast<uint8_t>(localMinutes / 60);
  minute = static_cast<uint8_t>(localMinutes % 60);
  return sticky_note::validDate(year, month, day);
}

inline bool currentLocalDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) {
  uint8_t second = 0;
  return currentLocalDateTime(year, month, day, hour, minute, second);
}

inline bool currentLocalDate(uint16_t& year, uint8_t& month, uint8_t& day) {
  uint8_t hour = 0;
  uint8_t minute = 0;
  return currentLocalDateTime(year, month, day, hour, minute);
}
}  // namespace calendar_app
