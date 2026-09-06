---
title: CrossInk-Sticky Fork Delta
nav_order: 13
---

# CrossInk-Sticky fork delta

This is the maintenance record for features carried by this fork but not by
upstream CrossInk. Update it whenever the Sticky Notes behavior, protocol,
settings, integration points, or build requirements change.

## Baseline and history

- Upstream remote: `https://github.com/uxjulia/CrossInk.git`
- Fork remote: `https://github.com/hardiwb/CrossInk-Sticky.git`
- Sticky Notes was introduced in fork commit `2c40617d`.
- Home-menu ordering and truncated-row rendering were revised in `a08022a9`.
- The current recorded fork head is `0921b3c6`.
- Last verified build: 2026-08-30, PlatformIO environment `default`.
- Verified artifact: `.pio/build/default/firmware-x3-x4.bin`
- Verified SHA-256: `9A0166E2419B824C0D71F5805C882A285A3FE7EAC8443B350A8D6A0F12BFA403`

The `default` environment is the build used for the Xteink X3. The resulting
binary also carries `x3-x4` in its generated filename because CrossInk shares
that firmware image and performs device detection at runtime. Do not build the
Seeed Studio Sticky or an unrelated X4-only environment for X3 testing.

### 2026-08-30 refinement set

Changes made after the initial feature implementation:

- Start receiving immediately when the app opens.
- Return automatically after a successful sync, including restoration of the
  previous EPUB, TXT, or XTC reader.
- Add Sticky Notes to the reader long-press menu and configurable power-button
  long-press action.
- Preserve Lyra access to Settings after adding the extra home-menu entry.
- Render newline-separated items as individual rounded gray cards.
- Use `Day, MMM DD YYYY` for the note date.
- Add a Sticky-only custom font family and size setting.
- Add 10 pt and 12 pt sizing for the built-in Sticky Notes font.
- Add the adjustable `Bold Sticky Notes Content` setting, enabled by default.
- Fix SD custom fonts rendering question marks by prewarming the exact regular
  and bold glyph variants before layout and drawing.
- Publish the standalone ESP-NOW sender specification for non-BrokenSignal-Pro
  devices.

## User-visible behavior

CrossInk-Sticky adds a receive-only Calendar application:

1. Opening Calendar shows the offline Calendar on the X3's current local
   RTC date and loads the retained entry for the selected day. Left and Right
   select adjacent dates.
2. Selecting Sync starts unencrypted ESP-NOW reception on Wi-Fi channel
   1 for 60 seconds.
3. There is no persistent pairing. The sender retries until the matching ACK is
   received.
4. The radio runs only during explicit note reception. The normal deep-sleep
   path remains unchanged.
5. A validated note is rendered to a dated BMP under `/.crosspoint/calendar/`
   and selects the dedicated Calendar sleep-screen mode. At sleep time, the X3
   RTC chooses the BMP for the current local date. The mode is also available under
   Settings > Display > Sleep Screen and falls back safely before the first sync.
6. The validated source text is atomically upserted by date under
   `/.crosspoint/calendar/`. Calendar layout marks every stored date in the
   displayed month.
7. An ACK is sent only after calendar storage, rendering, file replacement, and sleep-image
   selection succeed.
8. After a successful sync, the app automatically returns to the previous
   screen. If opened from EPUB, TXT, or XTC reading, the reader is restored
   through the existing silent-restart path.

The generated sleep screen keeps the built-in `NOTES` header, uses the date
format `Day, MMM DD YYYY`, and displays each newline-separated item as its own
rounded light-gray card. The note text is never edited on the Xteink.

## Entry points and shortcuts

Calendar is integrated into these upstream interfaces:

- Home menus, including Dashboard, Lyra, and Minimal variants.
- Reader long-press menu.
- Configurable power-button long-press action.
- EPUB, TXT, and XTC reader quick-action dispatch.

The current power-action storage values were appended to preserve existing
settings compatibility:

- `CrossPointSettings::SHORT_PWRBTN::STICKY_NOTES = 23`
- `CrossPointSettings::LONG_MENU_STICKY_NOTES = 22`

Do not renumber existing enum values when adapting this feature to a newer
upstream release. If upstream has added values, choose new unused values and
add a settings migration if a collision has already shipped.

## Sticky Notes settings

Settings are shown under **Settings > Reader > Font Options** and are persisted
in the normal CrossInk settings JSON.

| JSON key | C++ field | Default | Meaning |
| --- | --- | ---: | --- |
| `stickyNoteFont` | `stickyNoteSdFontFamilyName` | empty | Empty uses the built-in Inter UI font; otherwise names an installed `.cpfont` family. |
| `stickyNoteFontSize` | `stickyNoteFontPointSize` | `12` | Built-in choices are 10 pt and 12 pt. SD fonts expose the sizes installed for that family. |
| `stickyNoteBold` | `stickyNoteBold` | `1` | Bold note content when enabled. The date stays regular. |
| `stickyNoteLayout` | `stickyNoteLayout` | `0` | `0` keeps the classic cards layout; `1` adds a Monday-first calendar with the note date highlighted. |

SD-card glyphs must be prepared before text measurement and drawing. The
Sticky Notes renderer prewarms the date and complete message with the required
regular/bold style mask. Removing this step causes unloaded glyph pages to be
rendered as `?`. If font loading or prewarming fails, rendering falls back to
the selected built-in size.

## Protocol contract

The wire format supports legacy version 1 and chunked version 2 and must remain
compatible with existing senders.
The source of truth is:

- `src/features/sticky_notes/StickyNoteProtocol.h`
- `docs/sticky-notes-esp-now-sender.md`

Important constants:

- Magic: ASCII `CINT`
- Note packet type: `1`
- ACK packet type: `2`
- Header length: 16 bytes for v1, 24 bytes for v2
- Maximum UTF-8 message: 2048 bytes (`220` in one v1 packet; larger messages use v2 chunks)
- Multi-byte integers: little-endian
- Rows: separated by LF (`0x0A`)

Never change packet offsets under protocol version 1. Introduce a new version
and retain version-1 parsing if the packet layout must evolve.

## Files owned by the extension

These files are isolated from upstream application code and should usually be
carried forward as a unit:

- `src/features/sticky_notes/StickyNotesConfig.h`
- `src/features/sticky_notes/StickyNoteProtocol.h`
- `src/features/sticky_notes/StickyNotesStore.h`
- `src/features/sticky_notes/StickyNotesStore.cpp`
- `src/features/sticky_notes/StickyNotesActivity.h`
- `src/features/sticky_notes/StickyNotesActivity.cpp`
- `src/features/sticky_notes/README.md`
- `lib/hal/HalEspNow.h`
- `lib/hal/HalEspNow.cpp`
- `docs/sticky-notes-esp-now-sender.md`
- `docs/crossink-sticky-fork-delta.md`
- `docs/images/StickynotesCrossink.jpeg`

`CROSSINK_ENABLE_STICKY_NOTES` is defined by
`StickyNotesConfig.h` and defaults to `1`. Set the build flag to `0` to compile
out the app and its menu integrations.

## Upstream integration points

These upstream-owned files contain small hooks and are the likely merge
conflict locations:

| Area | Files | Fork responsibility |
| --- | --- | --- |
| Activity routing | `src/activities/ActivityManager.h`, `src/activities/ActivityManager.cpp` | Construct Sticky Notes, record whether it came from a reader, and replace the current activity. |
| Home UI | `src/activities/home/HomeActivity.h`, `src/activities/home/HomeActivity.cpp` | Add the app below File Transfer, keep menu counts/caches correct, expose the Dashboard shortcut, and retain access to Settings in Lyra. |
| Reader actions | `src/activities/reader/EpubReaderActivity.cpp`, `TxtReaderActivity.cpp`, `XtcReaderActivity.cpp` | Dispatch long-menu and long-power actions and restore the prior reader after sync. |
| Settings schema | `src/CrossPointSettings.h`, `src/CrossPointSettings.cpp` | Persist font family, point size, bold toggle, and appended shortcut enum values. |
| Settings UI | `src/SettingsList.h` | Add Sticky font, size, bold toggle, long-menu option, and long-power option. |
| Translation source | `lib/I18n/translations/english.yaml` | Define all `STR_STICKY_*` labels. Other languages may use English fallback. |
| Generated translations | `lib/I18n/I18nKeys.h`, `lib/I18n/I18nStrings.cpp` | Generated during the build; do not hand-edit. |
| Build configuration | `platformio.ini` | Keep `default` as the local default environment unless upstream's environment layout changes. |

The original feature commit also adjusted `scripts/gen_i18n.py` and
`scripts/patch_jpegdec.py` for Windows console encoding and CRLF-tolerant
dependency patching. These are build-host fixes rather than Sticky Notes
requirements. Re-evaluate them after each upstream update instead of blindly
reapplying them if upstream has solved the same problems.

## Adapting to a new upstream release

Use a dedicated integration branch and keep the old working branch intact
until the device tests pass.

```powershell
git fetch upstream
git switch -c integrate-upstream-<version>
git merge upstream/main
```

Rebase may be used instead of merge if the fork history is intentionally kept
linear. Before starting, commit or stash all fork work; never perform an
upstream integration with the current feature changes only in the working
tree.

Resolve the update in this order:

1. Carry the extension-owned files forward.
2. Reconcile `HalEspNow` with any new upstream Wi-Fi/ESP-NOW abstraction.
3. Re-add `ActivityManager::goToStickyNotes()` using the current upstream
   activity-lifetime API.
4. Reinsert home entries through upstream's current menu builders. Verify every
   theme rather than assuming a shared menu count.
5. Reconcile power and long-menu enum values before merging settings code.
6. Restore EPUB, TXT, and XTC quick-action dispatch.
7. Rebuild the Sticky Notes font settings around the current font registry API.
8. Add translation keys only to the YAML source and let the build regenerate
   the compiled translation files.
9. Review sleep-image selection APIs. Preserve the atomic temporary-file rename
   and do not bypass the normal sleep-folder scanner.
10. Review upstream radio shutdown and silent-restart behavior so Wi-Fi is fully
    stopped before returning to deep sleep or a reader.

## Verification checklist

Build from a path without whitespace. The current workspace is
`D:\ESP32-Projects\CrossInk-Sticky`.

```powershell
C:\Users\hardi\.platformio\penv\Scripts\platformio.exe run -e default
```

After every upstream integration, verify on a physical Xteink X3:

- Firmware boots without an I2S legacy/new-driver conflict.
- Every home theme can still reach Settings.
- Sticky Notes appears below File Transfer.
- Opening Sticky Notes starts listening immediately.
- Timeout and retry leave the device responsive.
- A valid sender receives the matching ACK.
- Invalid packet, date, length, sequence, and UTF-8 inputs do not replace the
  existing sleep image.
- Each LF-delimited note becomes a separate gray rounded card.
- Date formatting is `Day, MMM DD YYYY`.
- Default font works at both 10 pt and 12 pt.
- Custom regular and bold fonts render real glyphs rather than `?`.
- Bold toggle affects note content but not the date/header.
- Successful sync returns to Home when launched from Home.
- Successful sync restores EPUB, TXT, and XTC when launched from a reader.
- Reader long-press menu and power-button long-press both open Sticky Notes.
- Normal power-off/deep sleep does not start ESP-NOW reception.
- The dated Calendar BMP matching the X3's current local RTC date is displayed on the next sleep.

Finally, update the baseline commit, verified build date, artifact hash, changed
file table, and this checklist whenever the fork behavior changes.
