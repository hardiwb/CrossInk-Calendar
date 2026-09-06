#include "StickyNotesActivity.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#include <GfxRenderer.h>
#include <FontCacheManager.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "CalendarDate.h"
#include "CalendarHourglassFooter.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "StickyNotesStore.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace fui = freeink::ui;

namespace {
constexpr const char* LOG_TAG = "NOTE";
constexpr const char* WEEKDAY_NAMES[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr const char* MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool noteRowHasMarker(const char* row, const char marker) {
  return row && row[0] == '[' && row[1] == marker && row[2] == ']' && row[3] == ' ';
}

bool isCompletedNoteRow(const char* row) { return noteRowHasMarker(row, 'x'); }

const char* noteRowText(const char* row) {
  return noteRowHasMarker(row, ' ') || isCompletedNoteRow(row) ? row + 4 : row;
}

constexpr bool isLeapYear(const uint16_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && isLeapYear(year) ? 29 : DAYS[month - 1];
}

void adjustDateByDays(uint16_t& year, uint8_t& month, uint8_t& day, int deltaDays) {
  while (deltaDays > 0) {
    if (day < daysInMonth(year, month)) {
      ++day;
    } else {
      day = 1;
      if (month < 12)
        ++month;
      else {
        month = 1;
        ++year;
      }
    }
    --deltaDays;
  }
  while (deltaDays < 0) {
    if (day > 1) {
      --day;
    } else {
      if (month > 1)
        --month;
      else {
        month = 12;
        --year;
      }
      day = daysInMonth(year, month);
    }
    ++deltaDays;
  }
}

// Sakamoto's algorithm returns Sunday as zero. Convert it to a Monday-first index.
int mondayFirstWeekday(const uint16_t year, const uint8_t month, const uint8_t day) {
  static constexpr int OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int adjustedYear = year;
  if (month < 3) --adjustedYear;
  const int sundayFirst =
      (adjustedYear + adjustedYear / 4 - adjustedYear / 100 + adjustedYear / 400 + OFFSETS[month - 1] + day) % 7;
  return (sundayFirst + 6) % 7;
}

int builtInNoteFontId(const uint8_t pointSize) {
  if (pointSize <= 10) return UI_10_FONT_ID;
  return UI_12_FONT_ID;
}

void formatNoteDate(const sticky_note::Note& note, char* output, const size_t outputSize) {
  std::tm date{};
  date.tm_year = static_cast<int>(note.year) - 1900;
  date.tm_mon = static_cast<int>(note.month) - 1;
  date.tm_mday = note.day;
  date.tm_hour = 12;
  date.tm_isdst = -1;
  mktime(&date);
  const int weekday = std::clamp(date.tm_wday, 0, 6);
  snprintf(output, outputSize, "%s, %s %02u %04u", WEEKDAY_NAMES[weekday], MONTH_NAMES[note.month - 1],
           static_cast<unsigned>(note.day), static_cast<unsigned>(note.year));
}
}  // namespace

StickyNotesActivity::StickyNotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const bool returnToReader)
    : Activity("Calendar", renderer, mappedInput),
      uiTarget_(makeUiTarget(renderer)),
      app_(uiTarget_, uiTarget_.deviceContext()),
      returnToReader_(returnToReader)
#ifndef SIMULATOR
      ,
      pendingMutex_(xSemaphoreCreateMutex())
#endif
{
}

StickyNotesActivity::~StickyNotesActivity() {
#ifndef SIMULATOR
  if (pendingMutex_) {
    vSemaphoreDelete(pendingMutex_);
    pendingMutex_ = nullptr;
  }
#endif
}

void StickyNotesActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  app_.setTheme(uiThemeTokens(uiTarget_));
  app_.on(ACTION_RECEIVE, &StickyNotesActivity::onRowEvent, this);
  app_.setScreen(&StickyNotesActivity::menuScreen, this);
#if FREEINK_DEVICE_X4 && !defined(SIMULATOR)
  if (gpio.deviceIsX4()) {
    state_ = State::Unsupported;
    requestUpdate();
    return;
  }
#endif
  if (!sticky_note::Store::recoverSnapshot()) {
    setError(StrId::STR_STICKY_NOTE_SAVE_FAILED);
    return;
  }
  if (!loadCurrentLocalDate()) {
    state_ = State::Unsupported;
    requestUpdate();
    return;
  }
  startReceiving();
}

void StickyNotesActivity::onExit() {
  Activity::onExit();
  stopReceiving();
  sdFontSystem.releaseLoadedFont(renderer);
  sdFontSystem.releaseRegistry();
  noteGlyphs_.reset();
  if (radioUsed_ && !leavingForWeb_) {
    if (returnToReader_)
      silentRestartToReader();
    else
      silentRestart();
  }
}

void StickyNotesActivity::loop() {
#ifndef SIMULATOR
  // Repeat the final acknowledgement briefly: a lost ACK must not cause a
  // second render/save, or make the sender report failure after a good save.
  if (state_ == State::Saved) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      stopReceiving();
      state_ = State::Ready;
      moveSelectedDay(-1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      stopReceiving();
      state_ = State::Ready;
      moveSelectedDay(1);
      return;
    }
    const uint32_t now = millis();
    if (now - savedAtMs_ >= 2000) {
      // Keep the radio session alive for a Cardputer calendar batch and return
      // to the full Calendar while the next dated note is pending.
      assembly_.reset();
      listeningStartedMs_ = now;
      state_ = State::Listening;
      requestUpdate();
      return;
    }
    if (now - lastAckMs_ >= 350) {
      lastAckMs_ = now;
      sendAck(lastSavedSourceMac_.data(), lastSavedSequence_, lastAckVersion_);
    }
    return;
  }
#endif
  processPendingNote();

  if ((state_ == State::Ready || state_ == State::Listening) && sleepImageNeedsRefresh_ &&
      calendarFrameReady_.exchange(false)) {
    sleepImageNeedsRefresh_ = false;
    if (!saveNoteSleepImage() || !selectNoteSleepImage()) {
      LOG_ERR(LOG_TAG, "Could not refresh web-edited Calendar sleep image");
    }
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitActivity();
    return;
  }

  if ((state_ == State::Ready || state_ == State::Listening || state_ == State::Error) &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    openWebCalendar();
    return;
  }

  if ((state_ == State::Ready || state_ == State::Listening) &&
      (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
       mappedInput.wasPressed(MappedInputManager::Button::PageBack))) {
    if (state_ == State::Listening) {
      stopReceiving();
      state_ = State::Ready;
    }
    moveSelectedDay(-1);
    return;
  }

  if ((state_ == State::Ready || state_ == State::Listening) &&
      (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
       mappedInput.wasPressed(MappedInputManager::Button::PageForward))) {
    if (state_ == State::Listening) {
      stopReceiving();
      state_ = State::Ready;
    }
    moveSelectedDay(1);
    return;
  }

  if ((state_ == State::Listening || state_ == State::Error) && uiReady_) {
    const fui::InputSnapshot snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app_.route(snapshot);
      if (app_.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  if (state_ == State::Listening && millis() - listeningStartedMs_ >= RECEIVE_TIMEOUT_MS) {
    stopReceiving();
    state_ = State::Ready;
    requestUpdate();
    return;
  }
}

void StickyNotesActivity::render(RenderLock&&) {
  if (state_ == State::Ready || state_ == State::Listening) {
    drawCalendarScreen(state_ == State::Listening);
    renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
    calendarFrameReady_ = true;
    return;
  }

  if (state_ == State::Saved || state_ == State::Applying) {
    drawNoteTemplate(state_ == State::Saved);
    renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
    return;
  }

  if (state_ == State::Error && sleepImageNeedsRefresh_.exchange(false)) {
    // Web edits remove the old bitmap. Rebuild it from the dated entry in the
    // current framebuffer, then replace the buffer with the error screen
    // before anything is sent to the e-ink panel.
    drawCalendarScreen();
    if (!saveNoteSleepImage() || !selectNoteSleepImage()) {
      LOG_ERR(LOG_TAG, "Could not refresh web-edited Calendar sleep image");
    }
  }

  const char* status = tr(STR_STICKY_NOTE_READY);
  if (state_ == State::Error) {
    status = I18n::getInstance().get(errorId_);
  } else if (state_ == State::Unsupported) {
    status = tr(STR_STICKY_NOTE_X4_UNSUPPORTED);
  }
  drawStatusScreen(status, state_ == State::Listening || state_ == State::Error);
  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}

void StickyNotesActivity::menuScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<StickyNotesActivity*>(user)->buildMenuScreen(screen);
}

void StickyNotesActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<StickyNotesActivity*>(user);
  if (event.value != 0 || (self->state_ != State::Listening && self->state_ != State::Error)) return;
  self->app_.clearTapFlash();
  self->openWebCalendar();
}

void StickyNotesActivity::buildMenuScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                                       metrics.verticalSpacing),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  fui::ListItem item;
  item.label = tr(STR_BROWSE);
  item.actionValue = 0;
  fui::ListProps props;
  props.items = &item;
  props.count = 1;
  props.selectedIndex = 0;
  props.action = ACTION_RECEIVE;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  configureUiList(props, screen.theme(), screen.body());
  screen.list(props);
}

void StickyNotesActivity::prepareNoteFont() {
  sdFontSystem.releaseLoadedFont(renderer);
  noteFontId_ = builtInNoteFontId(SETTINGS.stickyNoteFontPointSize);
  if (SETTINGS.stickyNoteSdFontFamilyName[0] != '\0' && !noteGlyphs_) {
    // This optional scratch area is too large for the C3 task stack. It is
    // allocated once for the activity and released in onExit().
    noteGlyphs_ = makeUniqueNoThrow<char[]>(NOTE_GLYPH_BYTES);
    if (!noteGlyphs_) LOG_ERR(LOG_TAG, "Cannot allocate %u font scratch bytes; using built-in font",
                            static_cast<unsigned>(NOTE_GLYPH_BYTES));
  }
  if (!noteGlyphs_ || SETTINGS.stickyNoteSdFontFamilyName[0] == '\0') return;

  const auto activation = sdFontSystem.activateDictionaryFont(renderer, SETTINGS.stickyNoteSdFontFamilyName,
                                                               SETTINGS.stickyNoteFontPointSize);
  if (activation.usingDictionaryFont && activation.fontId != 0) {
    noteFontId_ = activation.fontId;
  } else {
    LOG_ERR(LOG_TAG, "Failed to activate Sticky Notes font %s; using built-in font",
            SETTINGS.stickyNoteSdFontFamilyName);
    sdFontSystem.releaseLoadedFont(renderer);
  }
  sdFontSystem.releaseRegistry();
}

void StickyNotesActivity::prepareNoteGlyphCache(const char* dateLine, const EpdFontFamily::Style noteStyle) {
  if (!renderer.isSdCardFont(noteFontId_)) return;
  if (noteGlyphs_) snprintf(noteGlyphs_.get(), NOTE_GLYPH_BYTES, "%s\n%s", dateLine, note_.message.data());
  auto* fontCache = renderer.getFontCacheManager();
  const uint8_t styleMask = noteStyle == EpdFontFamily::BOLD ? 0x03 : 0x01;
  if (!noteGlyphs_ || !fontCache ||
      !fontCache->prewarmCache(noteFontId_, noteGlyphs_.get(), styleMask,
                               FontCacheManager::PreparationPolicy::DictionaryLean)) {
    LOG_ERR(LOG_TAG, "Failed to prepare Sticky Notes SD font; using built-in font");
    noteFontId_ = builtInNoteFontId(SETTINGS.stickyNoteFontPointSize);
  }
}

bool StickyNotesActivity::loadCurrentLocalDate() {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  if (!calendar_app::currentLocalDate(year, month, day)) {
    LOG_ERR(LOG_TAG, "Could not read a valid RTC date");
    return false;
  }

  note_.year = year;
  note_.month = month;
  note_.day = day;
  loadSelectedDate();
  return true;
}

void StickyNotesActivity::loadSelectedDate() {
  const uint16_t year = note_.year;
  const uint8_t month = note_.month;
  const uint8_t day = note_.day;
  note_.sequence = 0;
  note_.messageLength = 0;
  note_.message[0] = '\0';
  sleepImageNeedsRefresh_ = false;
  calendarFrameReady_ = false;
  if (sticky_note::Store::has(year, month, day) && !sticky_note::Store::load(year, month, day, note_)) {
    LOG_ERR(LOG_TAG, "Could not load calendar entry %04u-%02u-%02u", static_cast<unsigned>(year),
            static_cast<unsigned>(month), static_cast<unsigned>(day));
    note_.messageLength = 0;
    note_.message[0] = '\0';
    note_.year = year;
    note_.month = month;
    note_.day = day;
  }
  if (note_.messageLength > 0) {
    char imagePath[64];
    if (calendar_app::formatSleepImagePath(imagePath, sizeof(imagePath), year, month, day)) {
      sleepImageNeedsRefresh_ = !Storage.exists(imagePath);
    }
  }
}

void StickyNotesActivity::moveSelectedDay(const int deltaDays) {
  uint16_t year = note_.year;
  uint8_t month = note_.month;
  uint8_t day = note_.day;
  adjustDateByDays(year, month, day, deltaDays);
  if (!sticky_note::validDate(year, month, day)) return;
  note_.year = year;
  note_.month = month;
  note_.day = day;
  loadSelectedDate();
  requestUpdate();
}

void StickyNotesActivity::openWebCalendar() {
  stopReceiving();
  leavingForWeb_ = true;
  activityManager.goToFileTransfer();
}

void StickyNotesActivity::startReceiving() {
#ifdef SIMULATOR
  setError(StrId::STR_STICKY_NOTE_SIMULATOR_UNAVAILABLE);
#else
  stopReceiving();
  prepareNoteFont();
  radioUsed_ = true;
  if (!pendingMutex_) {
    setError(StrId::STR_STICKY_NOTE_RADIO_FAILED);
    return;
  }
  xSemaphoreTake(pendingMutex_, portMAX_DELAY);
  pending_ = false;
  pendingLength_ = 0;
  assembly_.reset();
  xSemaphoreGive(pendingMutex_);
  resetSnapshotState();
  receivedAny_ = false;
  lastSavedSequence_ = 0;
  lastAckVersion_ = sticky_note::LEGACY_VERSION;
  lastSavedSourceMac_.fill(0);
  if (!radio_.begin(ESPNOW_CHANNEL, &StickyNotesActivity::onReceive, this)) {
    setError(StrId::STR_STICKY_NOTE_RADIO_FAILED);
    return;
  }
  listeningStartedMs_ = millis();
  setState(State::Listening);
#endif
}

void StickyNotesActivity::stopReceiving() {
#ifndef SIMULATOR
  if (snapshotActive_) {
    sticky_note::Store::abortSnapshot();
    resetSnapshotState();
  }
  radio_.end();
#endif
}

void StickyNotesActivity::processPendingNote() {
#ifndef SIMULATOR
  if (!pendingMutex_ || state_ != State::Listening) return;
  sticky_note::ReceiveResult result = sticky_note::ReceiveResult::Rejected;
  sticky_note::SnapshotControl snapshotControl;
  std::array<uint8_t, 6> controlSource{};
  bool hasSnapshotControl = false;
  xSemaphoreTake(pendingMutex_, portMAX_DELAY);
  if (pending_) {
    hasSnapshotControl =
        sticky_note::decodeSnapshotControl(pendingPacket_.data(), pendingLength_, snapshotControl);
    if (hasSnapshotControl) {
      controlSource = pendingSourceMac_;
    } else if (!snapshotActive_ ||
               std::equal(snapshotSourceMac_.begin(), snapshotSourceMac_.end(), pendingSourceMac_.begin())) {
      result = assembly_.accept(pendingSourceMac_.data(), pendingPacket_.data(), pendingLength_, millis(), note_);
    }
    pending_ = false;
  }
  xSemaphoreGive(pendingMutex_);
  if (hasSnapshotControl) {
    processSnapshotControl(controlSource.data(), snapshotControl);
    return;
  }
  if (result != sticky_note::ReceiveResult::Complete) return;

  if (snapshotActive_ && !std::equal(snapshotSourceMac_.begin(), snapshotSourceMac_.end(), assembly_.source())) {
    assembly_.reset();
    return;
  }

  if (receivedAny_ && note_.sequence == lastSavedSequence_ &&
      std::equal(lastSavedSourceMac_.begin(), lastSavedSourceMac_.end(), assembly_.source())) {
    sendAck(assembly_.source(), note_.sequence, assembly_.version());
    assembly_.reset();
    return;
  }

  LOG_INF(LOG_TAG, "Received %u bytes in %u packet(s), protocol v%u",
          static_cast<unsigned>(note_.messageLength),
          static_cast<unsigned>(sticky_note::chunkCount(note_.messageLength)),
          static_cast<unsigned>(assembly_.version()));
  const uint32_t dateKey = static_cast<uint32_t>(note_.year) * 10000UL + static_cast<uint32_t>(note_.month) * 100UL +
                           static_cast<uint32_t>(note_.day);
  if (snapshotActive_ && dateKey <= snapshotLastDate_) {
    LOG_ERR(LOG_TAG, "Calendar snapshot dates are not strictly increasing");
    setError(StrId::STR_STICKY_NOTE_INVALID);
    return;
  }
  if (snapshotActive_ && snapshotReceivedEntries_ >= snapshotExpectedEntries_) {
    LOG_ERR(LOG_TAG, "Calendar snapshot contains more entries than declared");
    setError(StrId::STR_STICKY_NOTE_INVALID);
    return;
  }
  if (!(snapshotActive_ ? sticky_note::Store::saveSnapshot(note_) : sticky_note::Store::save(note_))) {
    setError(StrId::STR_STICKY_NOTE_SAVE_FAILED);
    return;
  }

  // Keep the tiny ACK identity independent from the Note buffer. The rendered
  // bitmap is stored by its own date, so sleep can select today's file by RTC.
  const uint32_t receivedSequence = note_.sequence;
  lastSavedSequence_ = receivedSequence;
  lastAckVersion_ = assembly_.version();
  std::copy_n(assembly_.source(), lastSavedSourceMac_.size(), lastSavedSourceMac_.begin());
  setState(State::Applying);
  const char* imageRoot = snapshotActive_ ? sticky_note::Store::snapshotRoot() : "/.crosspoint/calendar";
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered || !saveNoteSleepImage(imageRoot) ||
      (!snapshotActive_ && !selectNoteSleepImage())) {
    setError(StrId::STR_STICKY_NOTE_SAVE_FAILED);
    return;
  }

  if (snapshotActive_) {
    snapshotDigestState_ = sticky_note::snapshotDigestUpdate(snapshotDigestState_, note_);
    snapshotLastDate_ = dateKey;
    ++snapshotReceivedEntries_;
  }

  const bool ackQueued = sendAck(assembly_.source(), receivedSequence, assembly_.version());
  if (!ackQueued) LOG_ERR(LOG_TAG, "Note saved but ACK could not be queued");
  receivedAny_ = true;
  savedAtMs_ = lastAckMs_ = millis();
  state_ = State::Saved;
#endif
}

void StickyNotesActivity::setState(const State state) {
  state_ = state;
  requestUpdate();
}

void StickyNotesActivity::setError(const StrId errorId) {
  errorId_ = errorId;
  LOG_ERR(LOG_TAG, "%s", I18n::getInstance().get(errorId));
  stopReceiving();
  setState(State::Error);
}

void StickyNotesActivity::exitActivity() {
  mappedInput.suppressNextBackRelease();
  if (returnToReader_ && !APP_STATE.openEpubPath.empty())
    onSelectBook(APP_STATE.openEpubPath);
  else
    finish();
}

void StickyNotesActivity::drawStatusScreen(const char* status, const bool showWebAction) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, tr(STR_CALENDAR), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_CALENDAR));
  }

  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int statusY = safeArea.y + safeArea.height / 3;
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, status, safeArea.width - 32, 3);
  int y = statusY;
  for (const auto& line : lines) {
    UITheme::drawCenteredText(renderer, safeArea, UI_10_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }

  uiReady_ = false;
  if (showWebAction) app_.render();
  uiReady_ = showWebAction;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), showWebAction ? tr(STR_BROWSE) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void StickyNotesActivity::drawCalendarScreen(const bool showListening) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char title[96];
  if (showListening) {
    snprintf(title, sizeof(title), "%s - %s", tr(STR_CALENDAR), tr(STR_CALENDAR_SYNC));
  } else {
    snprintf(title, sizeof(title), "%s", tr(STR_CALENDAR));
  }
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = header.y + header.height + UITheme::getInstance().getMetrics().verticalSpacing;
  safeArea.height -= contentTop - safeArea.y;
  safeArea.y = contentTop;
  char dateLine[40];
  formatNoteDate(note_, dateLine, sizeof(dateLine));
  const auto noteStyle = SETTINGS.stickyNoteBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  prepareNoteGlyphCache(dateLine, noteStyle);
  drawCalendarTemplate(safeArea, dateLine, noteStyle, false, false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BROWSE), tr(STR_DIR_LEFT),
                                            tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  uiReady_ = false;
}

void StickyNotesActivity::drawNoteTemplate(const bool showSavedStatus) {
  renderer.clearScreen();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int sideInset = std::max(12, safeArea.width / 24);
  const int left = safeArea.x + sideInset;
  const int right = safeArea.x + safeArea.width - sideInset - 1;
  char dateLine[40];
  formatNoteDate(note_, dateLine, sizeof(dateLine));
  const auto noteStyle = SETTINGS.stickyNoteBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  prepareNoteGlyphCache(dateLine, noteStyle);

  if (SETTINGS.stickyNoteLayout == CrossPointSettings::STICKY_NOTE_CALENDAR) {
    drawCalendarTemplate(safeArea, dateLine, noteStyle, showSavedStatus, SETTINGS.calendarHourglassFooter != 0);
    return;
  }

  const int titleY = safeArea.y + 16;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  UITheme::drawCenteredText(renderer, safeArea, UI_12_FONT_ID, titleY, tr(STR_NOTES_TITLE), true, EpdFontFamily::BOLD);

  const int firstRuleY = titleY + titleLineHeight + 14;
  renderer.drawLine(left, firstRuleY, right, firstRuleY, 2, true);
  const int dateY = firstRuleY + 16;
  UITheme::drawCenteredText(renderer, safeArea, noteFontId_, dateY, dateLine);

  const int secondRuleY = dateY + renderer.getLineHeight(noteFontId_) + 14;
  renderer.drawLine(left, secondRuleY, right, secondRuleY, 2, true);
  const bool largeNote = note_.messageLength > sticky_note::CHUNK_BYTES;
  const int footerReserve = showSavedStatus || largeNote ? renderer.getLineHeight(SMALL_FONT_ID) + 20 : 0;
  const int messageTop = secondRuleY + 22;
  const int messageBottom = safeArea.y + safeArea.height - footerReserve;
  const bool truncated = drawNoteCards(left, right, messageTop, messageBottom, noteStyle, largeNote);

  if (largeNote && truncated) {
    UITheme::drawCenteredText(renderer, safeArea, SMALL_FONT_ID,
                              safeArea.y + safeArea.height - renderer.getLineHeight(SMALL_FONT_ID) - 4,
                              tr(STR_MORE));
  } else if (showSavedStatus) {
    UITheme::drawCenteredText(renderer, safeArea, SMALL_FONT_ID,
                              safeArea.y + safeArea.height - renderer.getLineHeight(SMALL_FONT_ID) - 4,
                              tr(STR_STICKY_NOTE_SAVED));
  }
}

void StickyNotesActivity::drawCalendarTemplate(const Rect& safeArea, const char* dateLine,
                                               const EpdFontFamily::Style noteStyle,
                                               const bool showSavedStatus, const bool reserveHourglassFooter) {
  const int sideInset = std::max(12, safeArea.width / 24);
  const int left = safeArea.x + sideInset;
  const int right = safeArea.x + safeArea.width - sideInset - 1;
  const int width = right - left + 1;
  const int dateY = safeArea.y + 16;
  UITheme::drawCenteredText(renderer, safeArea, UI_12_FONT_ID, dateY, dateLine, true, EpdFontFamily::BOLD);

  static constexpr StrId WEEKDAY_LABELS[] = {StrId::STR_WEEKDAY_MO, StrId::STR_WEEKDAY_TU, StrId::STR_WEEKDAY_WE,
                                             StrId::STR_WEEKDAY_TH, StrId::STR_WEEKDAY_FR, StrId::STR_WEEKDAY_SA,
                                             StrId::STR_WEEKDAY_SU};
  const int cellWidth = width / 7;
  const int weekdayY = dateY + renderer.getLineHeight(UI_12_FONT_ID) + 22;
  for (int column = 0; column < 7; ++column) {
    const char* label = I18n::getInstance().get(WEEKDAY_LABELS[column]);
    const int labelX = left + column * cellWidth + (cellWidth - renderer.getTextWidth(UI_10_FONT_ID, label,
                                                                                     EpdFontFamily::BOLD)) /
                                                     2;
    renderer.drawText(UI_10_FONT_ID, labelX, weekdayY, label, true, EpdFontFamily::BOLD);
  }

  const int firstWeekday = mondayFirstWeekday(note_.year, note_.month, 1);
  const int monthDays = daysInMonth(note_.year, note_.month);
  const uint8_t previousMonth = note_.month == 1 ? 12 : note_.month - 1;
  const uint16_t previousYear = note_.month == 1 ? note_.year - 1 : note_.year;
  const int previousMonthDays = daysInMonth(previousYear, previousMonth);
  const int rowCount = (firstWeekday + monthDays + 6) / 7;
  const int cellHeight = renderer.getLineHeight(UI_10_FONT_ID) + 14;
  const int calendarTop = weekdayY + renderer.getLineHeight(UI_10_FONT_ID) + 12;

  for (int cell = 0; cell < rowCount * 7; ++cell) {
    const int monthDay = cell - firstWeekday + 1;
    const bool inCurrentMonth = monthDay >= 1 && monthDay <= monthDays;
    const int shownDay = monthDay < 1 ? previousMonthDays + monthDay : (monthDay > monthDays ? monthDay - monthDays
                                                                                             : monthDay);
    const int column = cell % 7;
    const int row = cell / 7;
    const int cellX = left + column * cellWidth;
    const int cellY = calendarTop + row * cellHeight;
    const bool selected = inCurrentMonth && monthDay == note_.day;
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
    if (inCurrentMonth && sticky_note::Store::has(note_.year, note_.month, static_cast<uint8_t>(monthDay))) {
      constexpr int markerSize = 4;
      renderer.fillRoundedRect(cellX + (cellWidth - markerSize) / 2, cellY + cellHeight - markerSize - 1, markerSize,
                               markerSize, markerSize / 2, Color::Black);
    }
  }

  const int calendarBottom = calendarTop + rowCount * cellHeight;
  const int notesY = calendarBottom + 16;
  renderer.drawText(UI_10_FONT_ID, left, notesY, tr(STR_NOTES_TITLE), true, EpdFontFamily::BOLD);
  const int ruleY = notesY + renderer.getLineHeight(UI_10_FONT_ID) + 9;
  renderer.drawLine(left, ruleY, right, ruleY, 2, true);

  const bool largeNote = note_.messageLength > sticky_note::CHUNK_BYTES;
  const int hourglassReserve = reserveHourglassFooter ? calendar_app::HOURGLASS_FOOTER_HEIGHT : 0;
  const int footerReserve = (showSavedStatus || largeNote ? renderer.getLineHeight(SMALL_FONT_ID) + 20 : 0) +
                            hourglassReserve;
  const int messageBottom = safeArea.y + safeArea.height - footerReserve;
  if (note_.messageLength == 0) {
    UITheme::drawCenteredText(renderer, safeArea, SMALL_FONT_ID, ruleY + 18, tr(STR_NO_ENTRIES));
    return;
  }
  const bool truncated = drawNoteCards(left, right, ruleY + 14, messageBottom, noteStyle, true);
  if (largeNote && truncated) {
    UITheme::drawCenteredText(renderer, safeArea, SMALL_FONT_ID,
                              safeArea.y + safeArea.height - hourglassReserve - renderer.getLineHeight(SMALL_FONT_ID) -
                                  4,
                              tr(STR_MORE));
  } else if (showSavedStatus) {
    UITheme::drawCenteredText(renderer, safeArea, SMALL_FONT_ID,
                              safeArea.y + safeArea.height - hourglassReserve - renderer.getLineHeight(SMALL_FONT_ID) -
                                  4,
                              tr(STR_STICKY_NOTE_SAVED));
  }
}

bool StickyNotesActivity::drawNoteCards(const int left, const int right, const int top, const int bottom,
                                        const EpdFontFamily::Style noteStyle, const bool compact) {
  const int lineHeight = renderer.getLineHeight(noteFontId_) + (compact ? 2 : 5);
  constexpr int cardPaddingX = 12;
  const int cardPaddingY = compact ? 4 : 8;
  const int cardGap = compact ? 3 : 8;
  constexpr int cardRadius = 10;
  const int cardLeft = left;
  const int cardWidth = std::max(1, right - cardLeft + 1);
  const int textLeft = cardLeft + cardPaddingX;
  const int textWidth = std::max(1, cardWidth - cardPaddingX * 2);
  int cardY = top;
  char* row = note_.message.data();

  while (row && *row && cardY + cardPaddingY * 2 + lineHeight <= bottom) {
    char* newline = strchr(row, '\n');
    if (newline) *newline = '\0';
    const int remainingLines = std::max(1, (bottom - cardY - cardPaddingY * 2) / lineHeight);
    const bool completed = isCompletedNoteRow(row);
    const auto lines = renderer.wrappedText(noteFontId_, noteRowText(row), textWidth, remainingLines, noteStyle);
    const int cardHeight = cardPaddingY * 2 + static_cast<int>(lines.size()) * lineHeight;
    if (!completed) {
      renderer.fillRoundedRect(cardLeft, cardY, cardWidth, cardHeight, cardRadius, Color::LightGray);
    }

    int textY = cardY + cardPaddingY;
    for (const auto& line : lines) {
      renderer.drawText(noteFontId_, textLeft, textY, line.c_str(), true, noteStyle);
      textY += lineHeight;
    }

    if (newline) {
      *newline = '\n';
      row = newline + 1;
      cardY += cardHeight + cardGap;
    } else {
      row = nullptr;
      break;
    }
  }
  return row && *row;
}

bool StickyNotesActivity::saveNoteSleepImage(const char* root) {
  constexpr size_t PATH_BYTES = 64;
  char imagePath[PATH_BYTES];
  char tempPath[PATH_BYTES];
  char backupPath[PATH_BYTES];
  if (!calendar_app::formatSleepImagePathInRoot(imagePath, sizeof(imagePath), root, note_.year, note_.month,
                                                note_.day) ||
      !calendar_app::formatSleepImagePathInRoot(tempPath, sizeof(tempPath), root, note_.year, note_.month, note_.day,
                                                ".tmp") ||
      !calendar_app::formatSleepImagePathInRoot(backupPath, sizeof(backupPath), root, note_.year, note_.month,
                                                note_.day, ".bak")) {
    LOG_ERR(LOG_TAG, "Failed to build dated Calendar sleep-image path");
    return false;
  }
  if (Storage.exists(tempPath)) Storage.remove(tempPath);
  if (!ScreenshotUtil::saveFramebufferAsBmp(tempPath, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                            renderer.getDisplayHeight())) {
    LOG_ERR(LOG_TAG, "Failed to write temporary note bitmap");
    return false;
  }

  if (Storage.exists(backupPath)) Storage.remove(backupPath);
  const bool hadExisting = Storage.exists(imagePath);
  if (hadExisting && !Storage.rename(imagePath, backupPath)) {
    Storage.remove(tempPath);
    LOG_ERR(LOG_TAG, "Failed to back up current note bitmap");
    return false;
  }
  if (!Storage.rename(tempPath, imagePath)) {
    if (hadExisting) Storage.rename(backupPath, imagePath);
    Storage.remove(tempPath);
    LOG_ERR(LOG_TAG, "Failed to install note bitmap");
    return false;
  }
  if (hadExisting) Storage.remove(backupPath);
  return true;
}

bool StickyNotesActivity::selectNoteSleepImage() {
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CALENDAR_SLEEP;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR(LOG_TAG, "Failed to select Calendar sleep mode");
    return false;
  }
  return true;
}

#ifndef SIMULATOR
void StickyNotesActivity::onReceive(const uint8_t* sourceMac, const uint8_t* data, const int length, void* context) {
  if (!context) return;
  static_cast<StickyNotesActivity*>(context)->enqueueNote(sourceMac, data, length);
}

void StickyNotesActivity::enqueueNote(const uint8_t* sourceMac, const uint8_t* data, const int length) {
  if (!pendingMutex_ || !sourceMac || !data || length <= 0 ||
      static_cast<size_t>(length) > pendingPacket_.size()) return;
  if (xSemaphoreTake(pendingMutex_, 0) != pdTRUE) return;
  // Callback only copies one bounded packet. Parsing/CRC and all storage work
  // run on the activity loop; a full mailbox is retried by the sender.
  if (!pending_) {
    memcpy(pendingPacket_.data(), data, static_cast<size_t>(length));
    pendingLength_ = static_cast<size_t>(length);
    std::copy(sourceMac, sourceMac + pendingSourceMac_.size(), pendingSourceMac_.begin());
    pending_ = true;
  }
  xSemaphoreGive(pendingMutex_);
}

void StickyNotesActivity::resetSnapshotState() {
  snapshotActive_ = false;
  snapshotSequence_ = 0;
  snapshotExpectedDigest_ = 0;
  snapshotDigestState_ = 0xffffffffU;
  snapshotLastDate_ = 0;
  snapshotExpectedEntries_ = 0;
  snapshotReceivedEntries_ = 0;
  snapshotSourceMac_.fill(0);
}

void StickyNotesActivity::processSnapshotControl(const uint8_t* sourceMac,
                                                 const sticky_note::SnapshotControl& control) {
  if (!sourceMac) return;
  if (snapshotActive_ &&
      !std::equal(snapshotSourceMac_.begin(), snapshotSourceMac_.end(), sourceMac)) {
    return;
  }

  if (control.type == sticky_note::TYPE_SNAPSHOT_BEGIN) {
    const bool repeatsCommittedSnapshot =
        control.sequence == lastCommittedSnapshotSequence_ && control.entryCount == lastCommittedSnapshotEntries_ &&
        control.digest == lastCommittedSnapshotDigest_ &&
        std::equal(lastCommittedSnapshotSourceMac_.begin(), lastCommittedSnapshotSourceMac_.end(), sourceMac);
    if (repeatsCommittedSnapshot) {
      sendAck(sourceMac, control.sequence, sticky_note::SNAPSHOT_VERSION);
      return;
    }
    if (snapshotActive_) {
      const bool sameSnapshot = control.sequence == snapshotSequence_ &&
                                control.entryCount == snapshotExpectedEntries_ &&
                                control.digest == snapshotExpectedDigest_ &&
                                std::equal(snapshotSourceMac_.begin(), snapshotSourceMac_.end(), sourceMac);
      if (sameSnapshot) sendAck(sourceMac, control.sequence, sticky_note::SNAPSHOT_VERSION);
      return;
    }
    if (!sticky_note::Store::beginSnapshot()) {
      setError(StrId::STR_STICKY_NOTE_SAVE_FAILED);
      return;
    }
    resetSnapshotState();
    snapshotActive_ = true;
    snapshotSequence_ = control.sequence;
    snapshotExpectedDigest_ = control.digest;
    snapshotExpectedEntries_ = control.entryCount;
    std::copy_n(sourceMac, snapshotSourceMac_.size(), snapshotSourceMac_.begin());
    LOG_INF(LOG_TAG, "Calendar snapshot started: %u entries", static_cast<unsigned>(control.entryCount));
    sendAck(sourceMac, control.sequence, sticky_note::SNAPSHOT_VERSION);
    listeningStartedMs_ = millis();
    return;
  }

  if (!snapshotActive_) {
    if (control.sequence == lastCommittedSnapshotSequence_ && control.entryCount == lastCommittedSnapshotEntries_ &&
        control.digest == lastCommittedSnapshotDigest_ &&
        std::equal(lastCommittedSnapshotSourceMac_.begin(), lastCommittedSnapshotSourceMac_.end(), sourceMac)) {
      sendAck(sourceMac, control.sequence, sticky_note::SNAPSHOT_VERSION);
    }
    return;
  }
  const bool matchesSnapshot = control.sequence == snapshotSequence_ &&
                               control.entryCount == snapshotExpectedEntries_ &&
                               control.digest == snapshotExpectedDigest_ &&
                               std::equal(snapshotSourceMac_.begin(), snapshotSourceMac_.end(), sourceMac);
  const uint32_t receivedDigest = sticky_note::snapshotDigestFinish(snapshotDigestState_);
  if (!matchesSnapshot || snapshotReceivedEntries_ != snapshotExpectedEntries_ || control.digest != receivedDigest) {
    LOG_ERR(LOG_TAG, "Calendar snapshot commit validation failed (%u/%u entries)",
            static_cast<unsigned>(snapshotReceivedEntries_), static_cast<unsigned>(snapshotExpectedEntries_));
    setError(StrId::STR_STICKY_NOTE_INVALID);
    return;
  }
  if (!sticky_note::Store::commitSnapshot()) {
    setError(StrId::STR_STICKY_NOTE_SAVE_FAILED);
    return;
  }

  lastCommittedSnapshotSequence_ = control.sequence;
  lastCommittedSnapshotDigest_ = control.digest;
  lastCommittedSnapshotEntries_ = control.entryCount;
  std::copy_n(sourceMac, lastCommittedSnapshotSourceMac_.size(), lastCommittedSnapshotSourceMac_.begin());
  resetSnapshotState();
  receivedAny_ = true;
  lastSavedSequence_ = control.sequence;
  lastAckVersion_ = sticky_note::SNAPSHOT_VERSION;
  std::copy_n(sourceMac, lastSavedSourceMac_.size(), lastSavedSourceMac_.begin());
  if (!loadCurrentLocalDate()) {
    LOG_ERR(LOG_TAG, "Calendar snapshot committed but the current RTC date could not be loaded");
  }
  if (!selectNoteSleepImage()) {
    LOG_ERR(LOG_TAG, "Calendar snapshot committed but Calendar sleep mode could not be selected");
  }
  requestUpdate();
  sendAck(sourceMac, control.sequence, sticky_note::SNAPSHOT_VERSION);
  savedAtMs_ = lastAckMs_ = millis();
  state_ = State::Saved;
  LOG_INF(LOG_TAG, "Calendar snapshot committed: %u entries", static_cast<unsigned>(control.entryCount));
}

bool StickyNotesActivity::sendAck(const uint8_t* peerMac, const uint32_t sequence, const uint8_t version) {
  const auto ack = sticky_note::makeAck(sequence, version);
  return radio_.send(peerMac, ack.data(), ack.size());
}
#endif

#endif  // CROSSINK_ENABLE_STICKY_NOTES
