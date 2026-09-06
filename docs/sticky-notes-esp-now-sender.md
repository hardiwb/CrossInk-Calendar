---
title: Sticky Notes ESP-NOW Sender Protocol
nav_order: 12
---

# Build a Sticky Notes sender

Any ESP32 firmware can send a Sticky Notes sleep screen to CrossInk-Sticky;
BrokenSignal-Pro is not required.

## Receiver workflow

1. On the Xteink X3, open **Menu > Calendar**. Listening starts immediately.
2. The Xteink listens on ESP-NOW channel 1 for 60 seconds.
3. Broadcast the note packet described below.
4. Wait for the Xteink ACK.

There is no persistent pairing and no shared encryption key. The receiver runs
only during the Calendar view's listening window, so normal deep sleep is not interrupted.

## Text format

Encode one checklist item per UTF-8 line:

```text
[ ] Buy groceries
[ ] Call Alice
[x] Archive invoices
```

- The entire text must be 1-2048 UTF-8 bytes. Count bytes, not characters.
- Use v1 for messages up to 220 bytes; use the chunked v2 format below for larger messages.
- Separate items with one `LF` byte (`0x0A`, `\n`).
- Do not add `LF` or NUL after the final item.
- Prefix an unfinished item with the exact four bytes `[ ] `.
- Prefix a completed item with the exact four bytes `[x] ` (lowercase `x`).
- The receiver hides both prefixes and renders completed items without the
  light-gray card background.
- Replace CR, LF, or tab inside an item's text with a normal space.
- Do not include the date in the text; encode it in the packet header.

A sender may omit completed items to produce an active-tasks-only note.
Ordinary UTF-8 is accepted, but these prefixes and row separators produce the
intended checklist layout.

## Version 1 note packet (legacy)

Use unencrypted ESP-NOW on Wi-Fi channel 1. The packet is exactly
`16 + messageLength` bytes. Multi-byte integers are unsigned little-endian.

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `CINT` (`43 49 4E 54`) |
| 4 | 1 | Protocol version `1` |
| 5 | 1 | Packet type `1` (note) |
| 6 | 1 | Reserved `0` |
| 7 | 1 | UTF-8 message length, `1`-`220` |
| 8 | 4 | Non-zero sequence number |
| 12 | 2 | Year, `2024`-`2099` |
| 14 | 1 | Month, `1`-`12` |
| 15 | 1 | Valid day for the month and year |
| 16 | N | Exactly N UTF-8 message bytes, without NUL |

Build the header byte-by-byte instead of relying on a packed struct. For
example, encode a 32-bit sequence number as:

```cpp
packet[8]  = sequence;
packet[9]  = sequence >> 8;
packet[10] = sequence >> 16;
packet[11] = sequence >> 24;
```

The receiver rejects a mismatched packet length, zero sequence, invalid date,
malformed UTF-8, embedded NUL, or unsupported control characters. It preserves
`LF` as a row separator and normalizes CR and tab to spaces.

## Version 2 chunked note packets

Keep the magic, packet type, sequence, and date offsets from v1. Byte 4 is `2`,
byte 6 is the zero-based chunk index, and byte 7 is this chunk's byte length.
Append this extension instead of placing text at offset 16:

| Offset | Size | Value |
| ---: | ---: | --- |
| 16 | 2 | Total message bytes, 1-2048 |
| 18 | 1 | Chunk count: ceil(total / 220), maximum 10 |
| 19 | 1 | Reserved zero |
| 20 | 4 | CRC-32/ISO-HDLC of the entire raw UTF-8 message |
| 24 | N | Payload: 220 bytes except the last chunk |

All integers are little-endian. CRC uses reflected polynomial `0xEDB88320`,
initial value `0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. Each packet is exactly
`24 + N` bytes, at most 244 bytes. All chunks carry identical sequence, date,
total, count and CRC fields. UTF-8 characters may cross chunk boundaries.

Send chunks in a repeating cycle at 100 ms intervals until the final ACK, with
a 30-second sender timeout. There is no per-chunk ACK. The receiver accepts
out-of-order and identical duplicate chunks, ignores other source MACs and
sequences during assembly, and releases an incomplete transfer after five
seconds without an accepted packet. Invalid lengths, offsets, dates, CRC or
full-message UTF-8 never trigger a render/save. CRC is not authentication.

The receiver uses one activity-owned 2 KB buffer, not a full-message stack
temporary. Callbacks copy only one small packet into a mailbox; the activity
loop handles reassembly, validation, rendering and storage.

## Version 3 full-calendar snapshot

Use a snapshot when the sender's complete calendar should replace the X3
calendar, including removal of dates deleted at the source. Send this sequence:

1. A v3 `Begin` control packet.
2. Every non-empty dated entry as an ordinary v1 or v2 note, sorted by ascending
   `year`, `month`, and `day`. Each note needs its own non-zero sequence number.
3. A v3 `Commit` control packet identical to `Begin` except for its type.

Both control packets are exactly 20 bytes:

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `CINT` |
| 4 | 1 | Protocol version `3` |
| 5 | 1 | Packet type `3` (Begin) or `4` (Commit) |
| 6 | 2 | Reserved zeros |
| 8 | 4 | Non-zero snapshot sequence number |
| 12 | 2 | Number of dated entries in the snapshot |
| 14 | 2 | Reserved zeros |
| 16 | 4 | Snapshot CRC-32 digest |

The snapshot digest uses the same reflected CRC-32 parameters as v2. Start at
`0xFFFFFFFF`; for each sorted entry update the CRC with these bytes:

1. Year as two little-endian bytes.
2. Month and day as one byte each.
3. Message length as two little-endian bytes.
4. The message bytes after replacing CR and tab with a normal space.

Apply the final XOR `0xFFFFFFFF` after the last entry. The digest of an empty
snapshot is zero. The entry count and digest must be identical in Begin and
Commit. Retry each control until its matching version-3 ACK arrives. Send the
next dated note only after the prior note's v1/v2 ACK arrives.

The X3 writes all snapshot `.bin` entries and generated `.bmp` lock screens to
`/.crosspoint/calendar.snapshot`. Commit succeeds only if the sender MAC,
snapshot identity, strictly increasing dates, entry count, and digest match.
It then swaps the staging directory into `/.crosspoint/calendar`, using
`/.crosspoint/calendar.backup` for reboot recovery. Timeout, validation,
rendering, storage, or interrupted-transfer failures discard staging and leave
the old live calendar intact. A valid empty snapshot removes every dated entry.

## ACK packet

After rendering and saving succeeds, the Xteink sends this 16-byte unicast
packet to the sender's source MAC:

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `CINT` |
| 4 | 1 | Protocol version copied from the note/control (`1`, `2`, or `3`) |
| 5 | 1 | Packet type `2` (ACK) |
| 6 | 1 | Reserved `0` |
| 7 | 1 | Message length `0` |
| 8 | 4 | Sequence number copied from the note |
| 12 | 4 | Reserved zeros |

An ESP-NOW send callback confirms only radio delivery. Report success only when
a valid ACK contains the sequence number and version of the note being sent.
The receiver repeats the final ACK every 350 ms for two seconds before exiting,
without rendering or saving duplicate packets again.

## Radio and retry requirements

- Use `WIFI_STA`, set channel 1, and initialize unencrypted ESP-NOW.
- Add `FF:FF:FF:FF:FF:FF` as an unencrypted broadcast peer on channel 1.
- Register the receive callback before transmitting.
- Use a new random non-zero sequence for each new note.
- For v1, retry the unchanged packet every 250-500 ms until its ACK arrives.
- For v2, cycle all chunks every 100 ms; stop only on the final save ACK.
- For v3, retry Begin and Commit independently, and wait for each dated note's
  ACK before sending the next sorted entry.
- Allow up to 30 seconds for reception, rendering and storage before timing out.
- Restore the sender's previous Wi-Fi state/channel when finished.

Repeated packets carrying the same sequence and content are retries, not new
notes. ESP-NOW callback signatures differ between Arduino-ESP32 2.x and 3.x;
use the signatures required by the sender project's installed core.

## Compatibility and display limits

Both devices must be updated to send more than 220 bytes or use full snapshots.
Old v1/v2 senders remain supported. Receivers without v3 support ignore its
control packets and therefore keep their existing upsert-only behavior.

Each accepted standalone v1/v2 transfer remains an upsert for its header date.
Inside a v3 snapshot, those dated transfers are staged until Commit; only then
does the snapshot replace the complete calendar. In Calendar layout, stored
dates in the displayed month receive a dot and the selected date is highlighted.

The lock screen remains a single image, not a paginated notebook. Larger
messages use tighter card spacing. Text can be ellipsized within a row, and
**More** indicates remaining rows below the screen. The 2 KB transfer allowance
does not guarantee that every row fits at the selected font size, but the full
validated message remains in its dated calendar file.

## Verification

Run the host `StickyNoteProtocolTest` and the adjacent BrokenSignal-Pro protocol
tests after copying the finalized v3 contract to the sender. After flashing
both devices, test a 221-2048 byte day through Calendar Sync. Interrupt a
snapshot and confirm the existing calendar remains intact. Then sync a source
calendar with a removed date and verify that the X3 removes it only after the
Commit ACK. Also test an empty snapshot and power interruption before and after
the directory swap.
