#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Personal-fork feature gate. Override with
// -DCROSSINK_ENABLE_STICKY_NOTES=0 to build without the menu and activity.
#ifndef CROSSINK_ENABLE_STICKY_NOTES
#define CROSSINK_ENABLE_STICKY_NOTES 1
#endif

#if CROSSINK_ENABLE_STICKY_NOTES != 0 && CROSSINK_ENABLE_STICKY_NOTES != 1
#error "CROSSINK_ENABLE_STICKY_NOTES must be 0 or 1"
#endif

namespace calendar_app {
inline bool formatSleepImagePath(char* output, const size_t outputSize, const uint16_t year, const uint8_t month,
                                 const uint8_t day, const char* suffix = "") {
  if (!output || outputSize == 0 || !suffix) return false;
  const int length = snprintf(output, outputSize, "/.crosspoint/calendar/%04u-%02u-%02u.bmp%s",
                              static_cast<unsigned>(year), static_cast<unsigned>(month),
                              static_cast<unsigned>(day), suffix);
  return length > 0 && static_cast<size_t>(length) < outputSize;
}
}  // namespace calendar_app
