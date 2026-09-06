# Calendar extension

This folder contains the optional, receive-only Sticky Notes extension. It is
kept separate from the reader activities so a personal fork can carry or drop
the feature with minimal merge conflicts. Define
`CROSSINK_ENABLE_STICKY_NOTES=0` in the PlatformIO build flags to remove its
menu entry and activity.

The complete list of fork-owned files, upstream integration points, persistent
settings, and update procedure is maintained in the
[fork delta guide](../../../docs/crossink-sticky-fork-delta.md).
The planned calendar application and optional half-hour lock-screen refresh are
described in the [calendar blueprint](../../../docs/calendar-blueprint.md).

Opening **Menu > Calendar** shows the full month view, immediately starts Wi-Fi
station mode, and listens for ESP-NOW notes on channel 1 for 60 seconds. The short **Browse** shortcut
beside Back opens the local web Calendar workflow. Left and Right select adjacent
dates and load their retained entries; navigating ends the current listening window. The normal
deep-sleep path is unchanged and never listens for notes.

## Sender protocol

Any ESP32 firmware can act as the sender; BrokenSignal-Pro is not required. See
the [standalone sender protocol guide](../../../docs/sticky-notes-esp-now-sender.md)
for the checklist text format, packet and ACK layouts, and retry behavior.

Send unencrypted ESP-NOW on Wi-Fi channel 1. All multi-byte values are
little-endian. The table below is the legacy v1 format for notes up to 220 bytes.
Updated senders can transfer up to 2048 bytes using v2 numbered chunks; see the
standalone guide above for the 24-byte header, CRC, and retry contract.
Protocol v3 adds transactional full-calendar snapshots. The X3 stages all
dated v1/v2 entries between a snapshot Begin and Commit, verifies their count
and digest, then swaps the staged directory into place. An interrupted or
invalid snapshot is discarded, so deleted dates disappear only after a valid
commit and the previous calendar remains usable on failure. A sender update is
required before BrokenSignal-Pro uses this receiver capability.

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `CINT` |
| 4 | 1 | Protocol version: `1` |
| 5 | 1 | Packet type: `1` (note) |
| 6 | 1 | Reserved: `0` |
| 7 | 1 | UTF-8 message length, 1-220 bytes |
| 8 | 4 | Non-zero sender sequence number |
| 12 | 2 | Year, 2024-2099 |
| 14 | 1 | Month, 1-12 |
| 15 | 1 | Day, valid for the month |
| 16 | N | UTF-8 message bytes, without a trailing NUL |

The Xteink validates the whole packet before changing storage. Line feeds split
checklist rows; carriage returns and tabs are rendered as spaces. After a
successful render and settings save, it sends a 16-byte acknowledgement to the
source MAC using the same header:
packet type `2` at offset 5 and the received sequence number at offset 8.
Rows prefixed with `[ ] ` use a light-gray card; completed rows prefixed with
`[x] ` render without the marker or gray background.

The sender should transmit every 250-500 ms until it receives the matching
acknowledgement or its own timeout expires. Increment the sequence number for
each new note.

For v2, repeat the complete chunk sequence at 100 ms intervals until the final
save ACK (version 2), with a 30-second sender timeout. The receiver does not
touch the old image until all chunks pass date, size, CRC and UTF-8 checks.
The activity holds one 2060-byte Note and small assembly metadata, replacing
the previous pair of 232-byte Note members and avoiding full-message locals.
The receiver repeats final ACKs for two seconds so packet loss does not force
another render/save. Original v1 senders still work.
Each dated entry also gets a dated 1-bit sleep image under
`/.crosspoint/calendar/`. When sleeping, the X3 uses its current local RTC date
to select that day's image, so batch order does not determine the displayed
sleep-screen date. These images use about 48 KB per synced day (roughly 1.5 MB
for a 31-day month) and require no second framebuffer.
SD-card fonts use an optional 2090-byte prewarm scratch allocation for the
activity lifetime, replacing a growing render-time string. Allocation failure
falls back to the built-in font; no second framebuffer is allocated.

The output remains one image. Long transfers use compact card spacing and a
More indicator for remaining rows; wrapped row text can still be ellipsized.
Each validated message is also retained by date under
`/.crosspoint/calendar/YYYY-MM-DD.bin`. Sending the same date again atomically
replaces that day's entry. Calendar layout marks every retained day in the
displayed month; the selected day's rows remain subject to the available
screen space.

The generated dated 1-bit bitmaps are installed atomically under
`/.crosspoint/calendar/`, selected through the Calendar sleep-screen mode, and
picked up by the firmware's sleep path according to the current RTC date. Calendar can also be selected
manually under **Settings > Display > Sleep Screen**; it falls back to the
normal sleep screen until the first successful sync creates an image.
Choose **Settings > System > Calendar > Calendar Layout > Calendar**
to put a Monday-first month grid above the received note rows. The received
date is highlighted with a light-gray rounded marker. The default Classic
layout remains unchanged. Font, font size, and bold content settings are in the
same **System > Calendar** menu.
