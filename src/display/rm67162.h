#pragma once

#include <Arduino.h>

void rm67162Init();
void rm67162Sleep();
void rm67162SetRotation(uint8_t rotation);
void rm67162PushColors(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                       const uint16_t *data);
