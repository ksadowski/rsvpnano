#pragma once

#include <Arduino.h>

namespace BoardConfig {

#ifdef BOARD_LILYGO_TDISPLAY_S3_PRO

// LILYGO T-Display-S3-Pro (ST7796 SPI display, CST226SE touch, SY6970 PMU)
constexpr int PIN_BOOT_BUTTON = 0;
constexpr int PIN_PWR_BUTTON = 12;   // btn2
constexpr int PIN_BUTTON3 = 16;      // btn3
constexpr int PIN_LED = 38;          // onboard green LED (shared with camera LED)
constexpr int PIN_BATTERY_ADC = -1;  // No direct ADC; SY6970 PMU is I2C-only

constexpr int PIN_LCD_CS = 39;
constexpr int PIN_LCD_SCLK = 18;
constexpr int PIN_LCD_MOSI = 17;
constexpr int PIN_LCD_MISO = 8;
constexpr int PIN_LCD_DC = 9;
constexpr int PIN_LCD_RST = 47;
constexpr int PIN_LCD_BACKLIGHT = 48;

constexpr int PANEL_NATIVE_WIDTH = 222;
constexpr int PANEL_NATIVE_HEIGHT = 480;
constexpr int DISPLAY_WIDTH = 480;
constexpr int DISPLAY_HEIGHT = 222;
constexpr bool UI_ROTATED_180 = false;

constexpr int PIN_SD_CS = 14;        // SD shares SPI bus with TFT
constexpr int PIN_I2C_SDA = 5;
constexpr int PIN_I2C_SCL = 6;
constexpr int PIN_TOUCH_RST = 13;
constexpr int PIN_TOUCH_IRQ = 21;

#else  // Waveshare ESP32-S3 3.49" (AXS15231B QSPI display)

constexpr int PIN_BOOT_BUTTON = 0;
constexpr int PIN_PWR_BUTTON = 16;
constexpr int PIN_BATTERY_ADC = 4;

constexpr int PIN_LCD_CS = 9;
constexpr int PIN_LCD_SCLK = 10;
constexpr int PIN_LCD_DATA0 = 11;
constexpr int PIN_LCD_DATA1 = 12;
constexpr int PIN_LCD_DATA2 = 13;
constexpr int PIN_LCD_DATA3 = 14;
constexpr int PIN_LCD_RST = 21;
constexpr int PIN_LCD_BACKLIGHT = 8;

constexpr int PANEL_NATIVE_WIDTH = 172;
constexpr int PANEL_NATIVE_HEIGHT = 640;
constexpr int DISPLAY_WIDTH = 640;
constexpr int DISPLAY_HEIGHT = 172;
constexpr bool UI_ROTATED_180 = true;  // Keep BOOT/PWR at the top edge in landscape.

constexpr int PIN_SD_CLK = 41;
constexpr int PIN_SD_CMD = 39;
constexpr int PIN_SD_D0 = 40;
constexpr int PIN_I2C_SDA = 47;
constexpr int PIN_I2C_SCL = 48;
constexpr int PIN_TOUCH_SDA = 17;
constexpr int PIN_TOUCH_SCL = 18;

constexpr int TCA9554_ADDRESS = 0x20;
constexpr uint8_t TCA9554_PIN_BATTERY_ADC_ENABLE = 1;
constexpr uint8_t TCA9554_PIN_SYS_EN = 6;

#endif  // BOARD_LILYGO_TDISPLAY_S3_PRO

struct BatteryStatus {
  bool present = false;
  float voltage = 0.0f;
  uint8_t percent = 0;
};

void begin();
void lightSleepUntilBootButton();
void prepareForDeepSleep();
bool readBatteryStatus(BatteryStatus &status);
bool releaseBatteryPowerHold();

}  // namespace BoardConfig
