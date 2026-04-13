#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

#include "app/AppState.h"
#include "display/DisplayManager.h"
#include "input/ButtonHandler.h"
#include "input/TouchHandler.h"
#include "reader/ReadingLoop.h"
#include "storage/StorageManager.h"

class App {
 public:
  App();

  void begin();
  void update(uint32_t nowMs);

 private:
  struct PausedTouchSession {
    bool active = false;
    uint16_t startX = 0;
    uint16_t startY = 0;
    uint16_t lastX = 0;
    uint16_t lastY = 0;
    uint32_t startMs = 0;
    uint32_t lastMs = 0;
    size_t startWordIndex = 0;
    int scrubStepsApplied = 0;
  };

  enum class TouchIntent {
    None,
    Scrub,
    Wpm,
  };

  enum class MenuScreen {
    Main,
    BookPicker,
    ChapterPicker,
  };

  void setState(AppState nextState, uint32_t nowMs);
  void updateState(uint32_t nowMs);
  void updateReader(uint32_t nowMs);
  void updateWpmFeedback(uint32_t nowMs);
  void maybeSaveReadingPosition(uint32_t nowMs);
  void handleTouch(uint32_t nowMs);
  void applyPausedTouchGesture(const TouchEvent &event, uint32_t nowMs);
  int scrubStepsForDrag(int deltaX) const;
  void applyScrubTarget(int targetSteps);
  void applyMenuTouchGesture(const TouchEvent &event, uint32_t nowMs);
  void moveMenuSelection(int direction);
  void selectMenuItem(uint32_t nowMs);
  void openBookPicker();
  void selectBookPickerItem(uint32_t nowMs);
  void openChapterPicker();
  void selectChapterPickerItem(uint32_t nowMs);
  void enterSleep(uint32_t nowMs);
  void wakeFromSleep();
  bool restoreSavedBook(uint32_t nowMs);
  void saveReadingPosition(bool force = false);
  bool loadBookAtIndex(size_t index, uint32_t nowMs, bool allowLegacyPositionFallback = false);
  String bookPositionKey(const String &bookPath) const;
  String bookWordCountKey(const String &bookPath) const;
  String bookRecentKey(const String &bookPath) const;
  uint32_t nextRecentSequence();
  uint32_t bookRecentSequence(const String &bookPath);
  void markBookRecent(const String &bookPath);
  uint32_t savedWordIndexForBook(const String &bookPath, bool allowLegacyFallback = false);
  bool bookProgressPercent(size_t bookIndex, uint8_t &percent);
  int findBookIndexByPath(const String &path) const;
  void renderMenu();
  void renderMainMenu();
  void renderBookPicker();
  void renderChapterPicker();
  String bookMenuLabel(size_t bookIndex);
  String chapterMenuLabel(size_t chapterIndex) const;
  String currentChapterLabel() const;
  uint8_t readingProgressPercent() const;
  void renderReaderWord();
  void renderWpmFeedback(uint32_t nowMs);
  const char *stateName(AppState state) const;
  const char *touchPhaseName(TouchPhase phase) const;

  AppState state_ = AppState::Booting;
  DisplayManager display_;
  ReadingLoop reader_;
  ButtonHandler button_;
  TouchHandler touch_;
  StorageManager storage_;
  Preferences preferences_;
  PausedTouchSession pausedTouch_;
  TouchIntent pausedTouchIntent_ = TouchIntent::None;

  uint32_t bootStartedMs_ = 0;
  uint32_t lastStateLogMs_ = 0;
  uint32_t wpmFeedbackUntilMs_ = 0;
  uint32_t lastProgressSaveMs_ = 0;
  size_t lastSavedWordIndex_ = static_cast<size_t>(-1);
  size_t currentBookIndex_ = 0;
  size_t menuSelectedIndex_ = 0;
  size_t bookPickerSelectedIndex_ = 0;
  size_t chapterPickerSelectedIndex_ = 0;
  MenuScreen menuScreen_ = MenuScreen::Main;
  std::vector<String> bookMenuItems_;
  std::vector<size_t> bookPickerBookIndices_;
  std::vector<String> chapterMenuItems_;
  std::vector<ChapterMarker> chapterMarkers_;
  String currentBookPath_;
  String currentBookTitle_;
  bool touchInitialized_ = false;
  bool wpmFeedbackVisible_ = false;
  bool usingStorageBook_ = false;
};
