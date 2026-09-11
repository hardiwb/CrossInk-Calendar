---
title: Calendar Lock Screen and Notion Sync
nav_order: 15
---

# Calendar lock screen and Notion sync

This CrossInk fork can use a Notion agenda as the source for the Xteink X3
Calendar. The X3 downloads the agenda itself over Wi-Fi and keeps a local,
offline snapshot on its SD card. A computer or phone is needed only for the
one-time connection setup in CrossInk's web interface.

> [!WARNING]
> This Calendar build targets the **Xteink X3 only**. Do not install it on an
> Xteink X4. The lock-screen timer wake and GPIO handling rely on X3 hardware.

## What synchronizes

```text
Notion agenda --HTTPS, read only--> X3 staging snapshot
                                      |
                                      +--successful import--> local Calendar
                                                              |
                                                              +--> lock screen
```

- The direction is **Notion to X3 only**. CrossInk never creates, edits,
  archives, or deletes a Notion page.
- A successful sync completely replaces the X3's local Calendar. Entries that
  were removed from Notion are therefore removed from the X3.
- An empty Notion result intentionally clears the local Calendar.
- A failed or interrupted sync discards its staging data and keeps the previous
  local Calendar.
- Sync is an explicit foreground action. The X3 does not contact Notion during
  normal boot, sleep, or timed lock-screen refreshes.

## Notion database requirements

Prepare a Notion database with:

1. One **title** property containing the agenda text.
2. One **date** property containing the day or date and time.
3. One internal integration that has been invited to the database.

The importer detects the first title property and the first date property. The
database must expose exactly one data source. A database with multiple data
sources is rejected to avoid importing the wrong agenda.

Rows are sorted by date. Rows on the same day become separate lines in one X3
Calendar entry. A date-time value gets an `HH:MM` prefix; an all-day value does
not. An empty title becomes `Untitled`.

Current importer limits:

- dates from `2024-01-01` through `2099-12-31`;
- 25 rows per Notion request and at most 10,000 rows per sync;
- 2,048 UTF-8 bytes per day after titles and time prefixes are combined.

If a day's combined text exceeds 2,048 bytes, later rows for that day are
omitted and the sync result reports a truncated day.

## Save the connection

1. In Notion, create an internal integration and copy its API token.
2. Open the agenda database's sharing menu and invite that integration.
3. On the X3, open **File Transfer > Join a Network** and connect to a 2.4 GHz
   Wi-Fi network.
4. Open the address displayed by the X3 in a browser on the same network.
5. Open **Calendar**, enter the integration token and the database URL or ID,
   then choose **Save connection**.

CrossInk accepts a full Notion database URL, a compact 32-character ID, or a
hyphenated UUID. The web API reports only whether a token exists; it never sends
the saved token back to the browser.

The token is device-bound and obfuscated in
`/.crosspoint/notion-calendar.json`. Obfuscation prevents casual recovery or
copying to another device, but it is not equivalent to encrypted secure
storage. Treat physical access to the SD card as sensitive and revoke the
integration token in Notion if the device or card is lost.

## Download the agenda on the X3

After the connection has been saved:

1. Open **File Transfer > Notion Calendar**.
2. Select a saved 2.4 GHz Wi-Fi network.
3. Leave the sync screen open until it reports success or failure.
4. Exit and open **Calendar** to inspect the imported dates.

The firmware reboots into a small network-only path for this operation. It does
not start the file-transfer web server, preserving internal RAM for the ESP32-C3
TLS connection. Before HTTPS begins, the system clock is set from the X3 RTC;
NTP is used only as a fallback.

## Enable the Calendar lock screen

1. Choose **Settings > Display > Sleep Screen > Calendar**.
2. Choose **Settings > System > Calendar > Calendar Layout > Calendar**.
3. Optionally enable **Hourglass Clock Footer**.
4. Optionally choose a **Clock Refresh** interval. `Never` is the default and
   uses the least battery.

The lock screen selects the current local date from the X3 RTC. Dates with
entries show the stored agenda layout. If today has no entry, CrossInk renders
the current month and date directly and displays a no-entries message instead
of falling back to the default CrossInk sleep image. Month markers still show
other imported days that contain entries.

The hourglass footer has 24 blocks, one per hour. Optional refresh intervals
wake only the RTC, SD card, framebuffer, and display path, redraw the footer,
then immediately return to deep sleep. They never start Wi-Fi or contact
Notion.

## SD-card data and recovery

| Path | Purpose |
| --- | --- |
| `/.crosspoint/notion-calendar.json` | Obfuscated token and normalized database ID |
| `/.crosspoint/notion-response.tmp` | Temporary HTTP response, removed after each request |
| `/.crosspoint/calendar.snapshot/` | Staging directory for a new complete import |
| `/.crosspoint/calendar/` | Active local Calendar snapshot |
| `/.crosspoint/calendar.backup/` | Temporary backup used during the atomic directory swap |
| `/.crosspoint/calendar/YYYY-MM-DD.bin` | Validated UTF-8 entry for one day |
| `/.crosspoint/calendar/YYYY-MM-DD.bmp` | Pre-rendered one-bit lock-screen image when available |

Each `.bin` entry has a magic value, format version, byte length, and CRC-32.
See [File Formats](./file-formats.md#calendaryyyy-mm-ddbin) for the binary
layout. On boot or before displaying the Calendar lock screen, CrossInk removes
an incomplete staging directory or restores the backup if a directory swap was
interrupted.

## Troubleshooting

| Message or symptom | What to check |
| --- | --- |
| **Not configured** | Save both the token and database URL/ID from the web Calendar page. |
| **Notion denied access** / HTTP 401 or 403 | Confirm the token, then invite the integration to this database—not only to a parent page. |
| **Could not reach Notion** / `ESP_ERR_HTTP_CONNECT` | Confirm Internet access, use 2.4 GHz Wi-Fi, move closer to the access point, and retry. A local IP address alone does not prove Internet or TLS connectivity. |
| **Could not set the device time** | Verify the X3 RTC contains a valid date. NTP must also be reachable if RTC setup fails. Incorrect time prevents TLS certificate validation. |
| **Needs a title property and a date property** | Add the required property types; their display names do not matter. |
| **Multiple data sources** | Use a database containing one agenda data source. |
| Sync succeeds but today is empty | This is valid. The lock screen should render today's calendar with a no-entries message and markers for other populated days. |
| Lock screen shows stale/missing content for an entry | Open **Calendar** after syncing so the selected day's layout can be rendered, then sleep the device again. Check SD write errors in the serial log. |

Useful serial tags are `[WIFI]` for association and signal strength, `[CLK]`
for RTC/NTP setup, `[NSYNC]` for Notion HTTPS and import details, `[NCAL]` for
snapshot storage, `[WEBACT]` for the sync screen, and `[SLP]`/`[CAL]` for the
lock-screen and timed-refresh paths.

## Hardware verification checklist

- Save a valid connection, sync, and confirm the result screen reports the
  expected entry and day counts.
- Remove a row in Notion, sync again, and confirm its local date disappears.
- Disconnect Internet access during a sync and confirm the previous Calendar
  remains intact.
- Sync an agenda with no entry for today and confirm the Calendar lock screen
  still shows today's date and the no-entries state.
- Power-cycle the X3 and confirm imported entries remain available offline.
- If using timed refresh, verify power-button wake still works and the device
  returns directly to sleep without showing Home.

## Implementation map

- Web setup UI: `web/pages/calendar.html`, `calendar.css`, and `calendar.js`
- Web API: `src/network/CrossPointWebServer.cpp`
- Saved credentials: `src/features/sticky_notes/NotionCalendarConfig.*`
- Notion HTTP importer: `src/features/sticky_notes/NotionCalendarSync.*`
- Atomic local storage: `src/features/sticky_notes/StickyNotesStore.*`
- Calendar UI and image rendering: `src/features/sticky_notes/StickyNotesActivity.*`
- Sleep and empty-day rendering: `src/activities/boot_sleep/SleepActivity.cpp`
- Hourglass/timer wake: `src/features/sticky_notes/CalendarHourglassFooter.*`
