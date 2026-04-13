#include "input/TouchHandler.h"

#include <Wire.h>

#include "board/BoardConfig.h"

bool TouchHandler::begin() {
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  consecutiveReadFailures_ = 0;
  touchActive_ = false;
  lastX_ = 0;
  lastY_ = 0;
  Wire.beginTransmission(kAddress);
  const uint8_t error = Wire.endTransmission();
  initialized_ = (error == 0);

  if (!initialized_) {
    Serial.println("[touch] Controller not detected at 0x38");
  } else {
    const uint8_t normalMode = 0x00;
    Wire.beginTransmission(kAddress);
    Wire.write(0x00);
    Wire.write(normalMode);
    Wire.endTransmission();
    Serial.println("[touch] Initialized (FT3168)");
  }

  return initialized_;
}

void TouchHandler::end() {
  cancel();
  initialized_ = false;
  Wire.end();
}

void TouchHandler::cancel() {
  touchActive_ = false;
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  consecutiveReadFailures_ = 0;
}

bool TouchHandler::readRegister(uint8_t reg, uint8_t *buffer, size_t len) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  // The ESP32 Arduino repeated-start path is what emits the i2cWriteReadNonStop
  // errors we were seeing in the monitor, so use a plain write-then-read transaction.
  if (Wire.endTransmission(true) != 0) {
    return false;
  }

  const size_t readLen =
      Wire.requestFrom(static_cast<uint8_t>(kAddress), static_cast<size_t>(len), true);
  if (readLen != len) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    buffer[i] = Wire.read();
  }
  return true;
}

bool TouchHandler::poll(TouchEvent &event) {
  static constexpr uint32_t kPollIntervalMs = 40;
  static constexpr uint32_t kFailureBackoffMs = 250;
  event = TouchEvent{};

  if (!initialized_) {
    return false;
  }

  const uint32_t now = millis();
  if (now < backoffUntilMs_) {
    return false;
  }

  if (now - lastPollMs_ < kPollIntervalMs) {
    return false;
  }
  lastPollMs_ = now;

  uint8_t points = 0;
  if (!readRegister(0x02, &points, 1)) {
    backoffUntilMs_ = now + kFailureBackoffMs;
    if (++consecutiveReadFailures_ >= 5) {
      initialized_ = false;
      Serial.println("[touch] Read failed repeatedly, disabling touch polling");
    }
    return false;
  }
  consecutiveReadFailures_ = 0;

  if (points == 0) {
    if (touchActive_) {
      touchActive_ = false;
      event.touched = false;
      event.x = lastX_;
      event.y = lastY_;
      event.phase = TouchPhase::End;
      return true;
    }
    return false;
  }

  uint8_t data[4] = {0};
  if (!readRegister(0x03, data, sizeof(data))) {
    backoffUntilMs_ = now + kFailureBackoffMs;
    if (++consecutiveReadFailures_ >= 5) {
      initialized_ = false;
      Serial.println("[touch] Read failed repeatedly, disabling touch polling");
    }
    return false;
  }
  backoffUntilMs_ = 0;
  consecutiveReadFailures_ = 0;

  event.touched = true;
  event.gesture = 0;
  event.phase = touchActive_ ? TouchPhase::Move : TouchPhase::Start;
  event.y = static_cast<uint16_t>(((data[0] & 0x0F) << 8) | data[1]);
  event.x = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
  if (event.x > BoardConfig::DISPLAY_WIDTH) {
    event.x = BoardConfig::DISPLAY_WIDTH;
  }
  if (event.y > BoardConfig::DISPLAY_HEIGHT) {
    event.y = BoardConfig::DISPLAY_HEIGHT;
  }
  event.y = BoardConfig::DISPLAY_HEIGHT - event.y;
  touchActive_ = true;
  lastX_ = event.x;
  lastY_ = event.y;

  return true;
}
