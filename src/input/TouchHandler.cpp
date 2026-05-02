#include "input/TouchHandler.h"

#include <algorithm>
#include <Wire.h>

#include "board/BoardConfig.h"

namespace {

constexpr uint32_t kPollIntervalMs = 20;
constexpr uint32_t kFailureBackoffMs = 250;
constexpr uint8_t kReleaseConfirmSamples = 2;

#ifndef BOARD_LILYGO_TDISPLAY_S3_PRO
constexpr uint8_t kReadTouchCommand[] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
};
#endif

uint16_t clampDisplayX(uint16_t x) {
  return std::min<uint16_t>(x, static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 1));
}

uint16_t clampDisplayY(uint16_t y) {
  return std::min<uint16_t>(y, static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - 1));
}

}  // namespace

#ifdef BOARD_LILYGO_TDISPLAY_S3_PRO

// ── CST226SE (T-Display-S3-Pro) ────────────────────────────────────────────

bool TouchHandler::begin() {
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  touchActive_ = false;
  lastX_ = 0;
  lastY_ = 0;

  // Hardware-reset the touch controller.
  pinMode(BoardConfig::PIN_TOUCH_RST, OUTPUT);
  digitalWrite(BoardConfig::PIN_TOUCH_RST, LOW);
  delay(100);
  digitalWrite(BoardConfig::PIN_TOUCH_RST, HIGH);
  delay(100);

  Wire.beginTransmission(kAddress);
  const uint8_t error = Wire.endTransmission();
  initialized_ = (error == 0);

  if (!initialized_) {
    Serial.printf("[touch] CST226SE not found at 0x%02X\n", kAddress);
  } else {
    Serial.println("[touch] Initialized (CST226SE)");
  }
  return initialized_;
}

void TouchHandler::end() {
  cancel();
  initialized_ = false;
}

void TouchHandler::cancel() {
  touchActive_ = false;
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
}

bool TouchHandler::readTouchPacket(uint8_t *buffer, size_t len) {
  // CST226SE: write register 0x00, read back 28 bytes.
  Wire.beginTransmission(kAddress);
  Wire.write(static_cast<uint8_t>(0x00));
  if (Wire.endTransmission(false) != 0) {
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

  // CST226SE packet: 28 bytes from register 0x00.
  // buffer[6] == 0xAB: valid data marker.
  // buffer[5] & 0x7F: touch point count.
  // Per-point layout (index=0 for first, +7 then +5 each):
  //   x = (buf[idx+1] << 4) | ((buf[idx+3] >> 4) & 0x0F)
  //   y = (buf[idx+2] << 4) | (buf[idx+3] & 0x0F)
  static constexpr size_t kPacketSize = 28;
  uint8_t data[kPacketSize] = {0};
  if (!readTouchPacket(data, kPacketSize)) {
    backoffUntilMs_ = now + kFailureBackoffMs;
    if (++consecutiveReadFailures_ >= 5) {
      initialized_ = false;
      Serial.println("[touch] Read failed repeatedly, disabling touch polling");
    }
    return false;
  }
  consecutiveReadFailures_ = 0;

  if (data[5] == 0x80) {
    const uint32_t nowMs = millis();
    if (nowMs - lastHomeButtonMs_ >= 400) {
      Serial.println("[touch] home button pressed");
      homeButtonPressed_ = true;
      lastHomeButtonMs_ = nowMs;
    }
  }

  const bool validMarker =
      (data[6] == 0xAB) && (data[0] != 0xAB) && (data[0] != 0x00) && (data[5] != 0x80);
  const uint8_t points = validMarker ? (data[5] & 0x7F) : 0;

  if (points == 0 || points > 5) {
    // Acknowledge the read to allow next packet.
    Wire.beginTransmission(kAddress);
    Wire.write(static_cast<uint8_t>(0x00));
    Wire.write(static_cast<uint8_t>(0xAB));
    Wire.endTransmission();

    if (touchActive_) {
      ++emptyTouchSamples_;
      if (emptyTouchSamples_ < kReleaseConfirmSamples) {
        return false;
      }
      touchActive_ = false;
      emptyTouchSamples_ = 0;
      event.touched = false;
      event.x = lastX_;
      event.y = lastY_;
      event.phase = TouchPhase::End;
      return true;
    }
    return false;
  }

  backoffUntilMs_ = 0;
  emptyTouchSamples_ = 0;
  lastTouchSampleMs_ = now;

  // Read first touch point.
  const uint16_t rawX =
      static_cast<uint16_t>((data[1] << 4) | ((data[3] >> 4) & 0x0F));
  const uint16_t rawY =
      static_cast<uint16_t>((data[2] << 4) | (data[3] & 0x0F));

  // Coordinate transform: CST226SE portrait (rawX=0..221, rawY=0..479)
  // → landscape display (X=0..479, Y=0..221).
  // Mirrors Y axis to match physical orientation (UI_ROTATED_180=false default).
  uint16_t mappedX, mappedY;
  if (BoardConfig::UI_ROTATED_180) {
    mappedX = clampDisplayX(static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 1 - rawY));
    mappedY = clampDisplayY(rawX);
  } else {
    mappedX = clampDisplayX(rawY);
    mappedY = clampDisplayY(
        static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - 1 - rawX));
  }

  event.touched = true;
  event.gesture = 0;
  event.phase = touchActive_ ? TouchPhase::Move : TouchPhase::Start;
  event.x = mappedX;
  event.y = mappedY;
  touchActive_ = true;
  lastX_ = event.x;
  lastY_ = event.y;
  return true;
}

bool TouchHandler::homeButtonPressedAndClear() {
  if (homeButtonPressed_) {
    homeButtonPressed_ = false;
    return true;
  }
  return false;
}

#else  // Waveshare ESP32-S3 — AXS15231B touch

bool TouchHandler::begin() {
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  touchActive_ = false;
  lastX_ = 0;
  lastY_ = 0;
  Wire.beginTransmission(kAddress);
  const uint8_t error = Wire.endTransmission();
  initialized_ = (error == 0);

  if (!initialized_) {
    Serial.println("[touch] Controller not detected at 0x3B");
  } else {
    Serial.println("[touch] Initialized (AXS15231B)");
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
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
}

bool TouchHandler::readTouchPacket(uint8_t *buffer, size_t len) {
  Wire.beginTransmission(kAddress);
  Wire.write(kReadTouchCommand, sizeof(kReadTouchCommand));
  if (Wire.endTransmission(false) != 0) {
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

  uint8_t data[8] = {0};
  if (!readTouchPacket(data, sizeof(data))) {
    backoffUntilMs_ = now + kFailureBackoffMs;
    if (++consecutiveReadFailures_ >= 5) {
      initialized_ = false;
      Serial.println("[touch] Read failed repeatedly, disabling touch polling");
    }
    return false;
  }
  consecutiveReadFailures_ = 0;

  const uint8_t points = data[1];
  if (points == 0 || points >= 5) {
    if (touchActive_) {
      ++emptyTouchSamples_;
      if (emptyTouchSamples_ < kReleaseConfirmSamples) {
        return false;
      }

      touchActive_ = false;
      emptyTouchSamples_ = 0;
      event.touched = false;
      event.x = lastX_;
      event.y = lastY_;
      event.phase = TouchPhase::End;
      return true;
    }
    return false;
  }

  backoffUntilMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  lastTouchSampleMs_ = now;

  event.touched = true;
  event.gesture = 0;
  event.phase = touchActive_ ? TouchPhase::Move : TouchPhase::Start;
  const uint16_t rawLongAxis = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
  const uint16_t rawShortAxis = static_cast<uint16_t>(((data[4] & 0x0F) << 8) | data[5]);
  const uint16_t mappedX = clampDisplayX(rawLongAxis);
  const uint16_t mappedY = clampDisplayY(rawShortAxis);
  if (BoardConfig::UI_ROTATED_180) {
    event.x = static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 1 - mappedX);
    event.y = static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - 1 - mappedY);
  } else {
    event.x = mappedX;
    event.y = mappedY;
  }
  touchActive_ = true;
  lastX_ = event.x;
  lastY_ = event.y;

  return true;
}

#endif  // BOARD_LILYGO_TDISPLAY_S3_PRO

#ifndef BOARD_LILYGO_TDISPLAY_S3_PRO
bool TouchHandler::homeButtonPressedAndClear() { return false; }
#endif
