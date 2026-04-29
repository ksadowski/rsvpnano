#include "input/TouchHandler.h"

#include <algorithm>
#include <Wire.h>

#include "board/BoardConfig.h"

namespace {

constexpr uint8_t kReadTouchCommand[] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
};
constexpr uint32_t kPollIntervalMs = 20;
constexpr uint32_t kFailureBackoffMs = 250;
constexpr uint8_t kReleaseConfirmSamples = 2;
// Stuck-touch watchdog: the AXS15231B occasionally wedges into a state where
// it keeps reporting `points=1` with the same coordinates indefinitely after
// the finger has actually lifted. If the position hasn't moved by at least
// `kStuckMotionPx` for `kStuckTouchTimeoutMs`, we synthesize an End event,
// re-arm the post-release gate, and let the next clean lift unblock new
// touches.
constexpr uint32_t kStuckTouchTimeoutMs = 2500;
constexpr uint16_t kStuckMotionPx = 5;

uint16_t clampDisplayX(uint16_t x) {
  return std::min<uint16_t>(x, static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 1));
}

uint16_t clampDisplayY(uint16_t y) {
  return std::min<uint16_t>(y, static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - 1));
}

}  // namespace

bool TouchHandler::begin() {
  lastPollMs_ = 0;
  backoffUntilMs_ = 0;
  lastTouchSampleMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  touchActive_ = false;
  // Require the controller to report a clean untouched sample before we
  // accept any real events; otherwise the post-wake phantom-touch burst
  // gets delivered as a fake gesture.
  requireReleaseBeforeAccept_ = true;
  lastX_ = 0;
  lastY_ = 0;
  lastSignificantMotionMs_ = 0;
  stuckAnchorX_ = 0;
  stuckAnchorY_ = 0;
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
  // Re-arm the release gate so a stale phantom-held sample from the
  // controller (e.g. after USB transfer or a forced drop) can't be
  // delivered as a Start until the finger is actually off the screen.
  requireReleaseBeforeAccept_ = true;
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
    // Any clean untouched sample disarms the release gate — from now on
    // real touches are allowed through.
    requireReleaseBeforeAccept_ = false;
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

  // Phantom-touch filter: before we've seen a clean lift, all touched=1
  // samples are discarded so the post-wake baseline drift from the
  // AXS15231B never surfaces as a gesture.
  if (requireReleaseBeforeAccept_) {
    return false;
  }

  backoffUntilMs_ = 0;
  consecutiveReadFailures_ = 0;
  emptyTouchSamples_ = 0;
  lastTouchSampleMs_ = now;

  const uint16_t rawLongAxis = static_cast<uint16_t>(((data[2] & 0x0F) << 8) | data[3]);
  const uint16_t rawShortAxis = static_cast<uint16_t>(((data[4] & 0x0F) << 8) | data[5]);

  // Drop packets where the decoded coordinate is outside the panel. The
  // AXS15231B periodically emits corrupt frames with values like x=502/637/638
  // or y=171; clamping them would smear `lastX_/lastY_` and confuse the
  // stuck-touch watchdog and gesture handlers.
  if (rawLongAxis >= static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH) ||
      rawShortAxis >= static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT)) {
    return false;
  }

  const uint16_t mappedX = clampDisplayX(rawLongAxis);
  const uint16_t mappedY = clampDisplayY(rawShortAxis);
  uint16_t outX;
  uint16_t outY;
  if (BoardConfig::UI_ROTATED_180) {
    outX = static_cast<uint16_t>(BoardConfig::DISPLAY_WIDTH - 1 - mappedX);
    outY = static_cast<uint16_t>(BoardConfig::DISPLAY_HEIGHT - 1 - mappedY);
  } else {
    outX = mappedX;
    outY = mappedY;
  }

  // Stuck-touch watchdog. While touchActive_ is true, every sample either
  // moves significantly (re-anchors the watchdog) or accumulates idle time.
  // If the position has been pinned within `kStuckMotionPx` of the anchor
  // for `kStuckTouchTimeoutMs`, synthesize an End event and re-arm the
  // post-release gate so the next clean controller release unblocks input.
  if (touchActive_) {
    const int dx = static_cast<int>(outX) - static_cast<int>(stuckAnchorX_);
    const int dy = static_cast<int>(outY) - static_cast<int>(stuckAnchorY_);
    const bool moved = (dx * dx + dy * dy) >=
                       static_cast<int>(kStuckMotionPx) * static_cast<int>(kStuckMotionPx);
    if (moved) {
      stuckAnchorX_ = outX;
      stuckAnchorY_ = outY;
      lastSignificantMotionMs_ = now;
    } else if (now - lastSignificantMotionMs_ >= kStuckTouchTimeoutMs) {
      Serial.printf("[touch] Stuck-touch watchdog tripped at x=%u y=%u, synthesizing End\n",
                    static_cast<unsigned int>(outX), static_cast<unsigned int>(outY));
      touchActive_ = false;
      emptyTouchSamples_ = 0;
      requireReleaseBeforeAccept_ = true;
      event.touched = false;
      event.x = lastX_;
      event.y = lastY_;
      event.phase = TouchPhase::End;
      return true;
    }
  } else {
    stuckAnchorX_ = outX;
    stuckAnchorY_ = outY;
    lastSignificantMotionMs_ = now;
  }

  event.touched = true;
  event.gesture = 0;
  event.phase = touchActive_ ? TouchPhase::Move : TouchPhase::Start;
  event.x = outX;
  event.y = outY;
  touchActive_ = true;
  lastX_ = outX;
  lastY_ = outY;

  return true;
}
