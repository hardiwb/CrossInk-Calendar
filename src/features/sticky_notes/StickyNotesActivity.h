#pragma once

#include "StickyNotesConfig.h"

#if CROSSINK_ENABLE_STICKY_NOTES

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <memory>

#include <I18n.h>

#include "StickyNoteProtocol.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/UIThemeTokens.h"

#ifndef SIMULATOR
#include <HalEspNow.h>
#include <freertos/semphr.h>
#endif

class StickyNotesActivity final : public Activity {
 public:
  StickyNotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToReader);
  ~StickyNotesActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == State::Listening || state_ == State::Applying || state_ == State::Saved;
  }
  bool skipLoopDelay() override { return state_ == State::Listening; }

 private:
  enum class State : uint8_t { Ready, Listening, Applying, Saved, Error, Unsupported };

  static constexpr uint8_t ESPNOW_CHANNEL = 1;
  static constexpr uint32_t RECEIVE_TIMEOUT_MS = 60000;
  static constexpr freeink::ui::ActionId ACTION_RECEIVE = 1;

  using UiApp = freeink::ui::FreeInkApp<4, 4>;
  freeink::ui::GfxRendererTarget uiTarget_;
  UiApp app_;
  std::atomic<bool> uiReady_{false};
  ScreenTransitionRefresh screenTransitionRefresh_;
  State state_ = State::Ready;
  StrId errorId_ = StrId::STR_STICKY_NOTE_INVALID;
  // One 2 KB buffer in the fallibly allocated activity, not on the task stack.
  sticky_note::Note note_;
  // Optional SD-font prewarm scratch, allocated once in onEnter and reused.
  std::unique_ptr<char[]> noteGlyphs_;
  static constexpr size_t NOTE_GLYPH_BYTES = sticky_note::MAX_MESSAGE_BYTES + 42;
  int noteFontId_ = 0;
  uint32_t listeningStartedMs_ = 0;
  bool radioUsed_ = false;
  bool returnToReader_ = false;
  bool receivedAny_ = false;

#ifndef SIMULATOR
  HalEspNow radio_;
  SemaphoreHandle_t pendingMutex_ = nullptr;
  sticky_note::Reassembler assembly_;
  std::array<uint8_t, sticky_note::MAX_PACKET_BYTES> pendingPacket_{};
  size_t pendingLength_ = 0;
  std::array<uint8_t, 6> pendingSourceMac_{};
  bool pending_ = false;
  uint32_t savedAtMs_ = 0;
  uint32_t lastAckMs_ = 0;
  uint32_t lastSavedSequence_ = 0;
  std::array<uint8_t, 6> lastSavedSourceMac_{};
#endif

  static void menuScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildMenuScreen(UiApp::ScreenType& screen);
  void prepareNoteFont();
  void prepareNoteGlyphCache(const char* dateLine, EpdFontFamily::Style noteStyle);
  bool loadCurrentLocalDate();
  void loadSelectedDate();
  void moveSelectedDay(int deltaDays);
  void startReceiving();
  void stopReceiving();
  void processPendingNote();
  void setState(State state);
  void setError(StrId errorId);
  void exitActivity();
  void drawStatusScreen(const char* status, bool showReceiveAction);
  void drawCalendarScreen();
  void drawNoteTemplate(bool showSavedStatus);
  void drawCalendarTemplate(const Rect& safeArea, const char* dateLine, EpdFontFamily::Style noteStyle,
                            bool showSavedStatus, bool reserveHourglassFooter);
  bool drawNoteCards(int left, int right, int top, int bottom, EpdFontFamily::Style noteStyle, bool compact);
  bool saveNoteSleepImage();
  bool selectNoteSleepImage();

#ifndef SIMULATOR
  static void onReceive(const uint8_t* sourceMac, const uint8_t* data, int length, void* context);
  void enqueueNote(const uint8_t* sourceMac, const uint8_t* data, int length);
  bool sendAck(const uint8_t* peerMac, uint32_t sequence);
#endif
};

#endif  // CROSSINK_ENABLE_STICKY_NOTES
