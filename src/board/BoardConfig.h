#pragma once

#include <Arduino.h>

namespace BoardConfig {

constexpr int PIN_BOOT_BUTTON = 0;

constexpr int PIN_LCD_CS = 6;
constexpr int PIN_LCD_SCLK = 47;
constexpr int PIN_LCD_DATA0 = 18;
constexpr int PIN_LCD_DATA1 = 7;
constexpr int PIN_LCD_DATA2 = 48;
constexpr int PIN_LCD_DATA3 = 5;
constexpr int PIN_LCD_RST = 17;
constexpr int PIN_LCD_EN = 21;

constexpr int DISPLAY_WIDTH = 536;
constexpr int DISPLAY_HEIGHT = 240;

constexpr int PIN_SD_CLK = 9;
constexpr int PIN_SD_CMD = 42;
constexpr int PIN_SD_D0 = 8;
constexpr int PIN_I2C_SDA = 40;
constexpr int PIN_I2C_SCL = 39;

void begin();
void lightSleepUntilBootButton();

}  // namespace BoardConfig
