---
title: Calendar Blueprint
nav_order: 14
---

# Calendar blueprint

This document tracks the planned evolution of the receive-only Sticky Notes
extension into an offline calendar for the Xteink X3. The existing ESP-NOW
protocol and dated note files remain the compatibility foundation.

## Product boundary

- The Xteink X3 is the primary hardware target because its DS3231 provides a
  persistent, accurate wall clock.
- The shared X3/X4 firmware may still be distributed, but opening Calendar on
  the original Xteink X4 must show the RTC-required warning and must not start
  Wi-Fi or ESP-NOW.
- Calendar operation is offline. Connectivity runs only when the user starts a
  note sync.
- The first release keeps one synchronized entry per date. Multiple events,
  event times, recurrence, and on-device text editing require a later versioned
  storage and wire format.

## Existing foundation

- ESP-NOW v1/v2 receives a validated date and up to 2048 bytes of UTF-8 text.
- Each date is atomically stored under `/.crosspoint/calendar/YYYY-MM-DD.bin`.
- Re-sending a date replaces that date's entry.
- ESP-NOW v3 control packets can bracket a complete calendar snapshot. The X3
  stages its dated v1/v2 entries, validates the declared count and digest, then
  replaces the live calendar directory in one recoverable commit. This lets a
  sender remove dates that no longer exist at the source.
- Calendar sleep-screen layout marks dates with retained entries.
- The generated Sticky Notes bitmap remains selectable as the custom sleep
  image.

## Calendar MVP

1. Open on the X3 RTC's current local date without starting the radio.
2. Navigate days, weeks, and months with logical mapped buttons.
3. Load and display the selected date's retained entry.
4. Provide an explicit Sync action that starts the existing ESP-NOW receiver.
5. Refresh the selected date and month markers after a successful sync.
6. Preserve the existing atomic storage, acknowledgement, and sleep-image
   behavior.
7. Treat a full Cardputer calendar sync as the source of truth: retain the old
   X3 calendar until the complete snapshot validates, then replace it. An empty
   snapshot intentionally clears the calendar.

## Notion agenda import

The Calendar page in the device web portal can import an agenda from Notion
while the device is connected to an existing Wi-Fi network. This is an explicit
foreground action; the firmware does not wake or connect in the background.

1. Create a Notion internal integration and copy its token.
2. Share the agenda database with that integration.
3. Ensure the database has at least one title property and one date property.
4. In the web portal, open Calendar, enter the token and database URL or ID,
   and save the connection.

After saving the connection once, choose **File Transfer > Notion Calendar** on
the device and select a Wi-Fi network. The firmware restarts into its minimal
network boot, downloads the agenda directly from Notion, and shows a dedicated
sync result screen. It does not start the file-transfer web server, leaving that
heap available for the ESP32-C3 TLS handshake.

The first title and date properties are detected automatically. Rows on the
same date become separate lines in that day's entry, and a date-time value is
prefixed with its `HH:MM` time. A successful import atomically replaces the
local calendar; if discovery, download, parsing, or storage fails, the previous
calendar remains intact. The integration is read-only: it only retrieves the
database schema and queries rows, and never creates, edits, archives, or deletes
Notion content. The API token is device-bound obfuscated on the SD
card and is never returned by the web API.

## Twenty-four-segment hourglass clock

The bottom of the Calendar lock screen may include a compact day-progress
indicator made from 24 separate rounded rectangles arranged in two rows of 12:

- The top row represents hours `00` through `11`; the bottom row represents
  hours `12` through `23`.
- Each rounded rectangle represents one hour.
- Each rectangle is `20 px` high with a `6 px` corner radius and a `2 px`
  external margin on every side. Its width is calculated from the available
  bottom-strip width so 12 rectangles fit evenly across each row.
- Past-hour rectangles are blank, without outlines.
- Future-hour rectangles are black.
- The current-hour rectangle is black from `:00` through `:29`.
- From `:30` through `:59`, the whole current-hour rectangle uses a 50% black
  checkerboard fill, showing that roughly 30 minutes remain in the hour.
- The blocks are borderless for an LCD-segment appearance, and the footer's
  side edges align with the Calendar note boxes.
- The indicator occupies a reserved bottom strip in the generated Calendar
  lock-screen layout so it cannot cover calendar or note text.
- The footer is enabled by default and can be toggled under
  `Settings > System > Calendar > Hourglass Clock Footer`.

The footer draws a snapshot when the device enters sleep. It reads time through
`HalClock`, draws through the existing renderer, and uses the current
sleep-screen refresh without allocating another framebuffer.

### Optional timed refresh

`Settings > System > Calendar > Clock Refresh` provides `Never`, `Every 30
Minutes`, `Every Hour`, and every `3`, `6`, `12`, or `24` hours. `Never` is the
default. Refreshes align to local clock boundaries: for example, the 30-minute
mode wakes at `:00` and `:30`, the 3-hour mode at `00:00`, `03:00`, and so on,
and the 24-hour mode at local midnight.

The opt-in modes use a dedicated minimal timer-wake path:

1. Before deep sleep, calculate the interval to the next selected boundary and
   arm an ESP32-C3 timer wake alongside the power-button wake.
2. On a timer wake, initialize only the hardware required for the RTC, SD card,
  framebuffer, and display.
3. Read the DS3231, load the pinned Sticky calendar image, redraw the 24-bar
   frame, and perform one sleep-screen refresh.
4. Schedule the next selected boundary and return directly to deep sleep
   without opening Home, a reader, networking, or note reception.

Timer wake must be exposed through `HalPowerManager`; app and sleep-screen code
must not call ESP-IDF sleep APIs directly. The feature must fail closed: if RTC
time is unavailable or invalid, do not schedule a repeating refresh loop.

Timed refresh is disabled by default because the most frequent option causes up
to 48 automatic boot, SD-access, and e-ink refresh cycles per day. It should not
be enabled by default until physical X3 testing measures the battery impact and
confirms acceptable display ghosting.

## Verification gates

- X3: Calendar immediately listens on the correct local date and acknowledges
  v1 and v2 notes; the Browse shortcut remains available beside Back.
- X3: an interrupted v3 snapshot leaves the old calendar intact; a valid
  snapshot removes dates absent from the source and survives a reboot during
  the directory-swap recovery points.
- X4: Calendar shows the RTC-required warning; radio initialization never
  occurs.
- Sleep-entry snapshot: the correct hour rectangle and half-hour fade are shown
  in the 24 rounded rectangles at the bottom of the Calendar lock screen.
- Timer wake: no boot or Home UI flashes before returning to sleep.
- Timer wake: power-button wake remains functional at all times.
- Each cadence wakes on its next local aligned boundary and remains disabled on
  the original X4.
- Invalid RTC state cannot cause a repeated wake loop.
- Measure sleep current, energy per refresh, and 24-hour battery impact on a
  physical X3 before changing the `Never` default.
