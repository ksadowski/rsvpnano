#pragma once

#include <Arduino.h>

enum class TouchPhase {
  Start,
  Move,
  End,
};

struct TouchEvent {
  bool touched = false;
  uint16_t x = 0;
  uint16_t y = 0;
  uint8_t gesture = 0;
  TouchPhase phase = TouchPhase::Move;
};

class TouchHandler {
 public:
  bool begin();
  void end();
  bool poll(TouchEvent &event);
  void cancel();
  bool homeButtonPressedAndClear();

 private:
#ifdef BOARD_LILYGO_TDISPLAY_S3_PRO
  static constexpr uint8_t kAddress = 0x5A;  // CST226SE on T-Display-S3-Pro.
  bool homeButtonPressed_ = false;
  uint32_t lastHomeButtonMs_ = 0;
#else
  static constexpr uint8_t kAddress = 0x3B;  // AXS15231B touch on Waveshare 3.49".
#endif
  bool initialized_ = false;
  uint32_t lastPollMs_ = 0;
  uint32_t backoffUntilMs_ = 0;
  uint32_t lastTouchSampleMs_ = 0;
  uint8_t consecutiveReadFailures_ = 0;
  uint8_t emptyTouchSamples_ = 0;
  bool touchActive_ = false;
  uint16_t lastX_ = 0;
  uint16_t lastY_ = 0;

  bool readTouchPacket(uint8_t *buffer, size_t len);
};
