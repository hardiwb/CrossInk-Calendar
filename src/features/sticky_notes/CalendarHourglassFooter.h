#pragma once

#include <cstdint>

class GfxRenderer;
class HalDisplay;

namespace calendar_app {

constexpr int HOURGLASS_BLOCK_HEIGHT = 20;
constexpr int HOURGLASS_CORNER_RADIUS = 6;
constexpr int HOURGLASS_BLOCK_MARGIN = 2;
constexpr int HOURGLASS_COLUMNS = 12;
constexpr int HOURGLASS_ROWS = 2;
constexpr int HOURGLASS_FOOTER_HEIGHT =
    HOURGLASS_ROWS * (HOURGLASS_BLOCK_HEIGHT + HOURGLASS_BLOCK_MARGIN * 2);

void drawHourglassFooter(const GfxRenderer& renderer, uint8_t hour, uint8_t minute);
bool calendarHourglassWakeDelay(uint32_t& wakeSeconds);
bool refreshCalendarHourglassAfterTimerWake(GfxRenderer& renderer, HalDisplay& display, uint32_t& nextWakeSeconds);

}  // namespace calendar_app
