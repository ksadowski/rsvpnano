#pragma once

#include <Arduino.h>
#include <vector>

class DisplayManager {
 public:
  ~DisplayManager();

  bool begin();
  void prepareForSleep();
  void renderCenteredWord(const String &word, uint16_t color = 0xFFFF);
  void renderRsvpWord(const String &word, const String &chapterLabel = "",
                      uint8_t progressPercent = 0, bool showFooter = true);
  void renderRsvpWordWithWpm(const String &word, uint16_t wpm, const String &chapterLabel = "",
                             uint8_t progressPercent = 0, bool showFooter = true);
  void renderMenu(const char *const *items, size_t itemCount, size_t selectedIndex);
  void renderMenu(const std::vector<String> &items, size_t selectedIndex);

 private:
  bool initPanel();
  bool allocateBuffers();
  bool drawBitmap(int xStart, int yStart, int xEnd, int yEnd, const void *colorData);
  void fillScreen(uint16_t color);
  void clearVirtualBuffer(int width, int height);
  int chooseTextScale(const String &word) const;
  int measureTextWidth(const String &word) const;
  int measureTinyTextWidth(const String &text, int scale) const;
  String fitTinyText(const String &text, int maxWidth, int scale) const;
  void drawGlyph(int x, int y, char c, uint16_t color);
  void fillVirtualRect(int x, int y, int width, int height, uint16_t color);
  void drawTinyGlyph(int x, int y, char c, uint16_t color, int scale);
  void drawTinyTextAt(const String &text, int x, int y, uint16_t color, int scale);
  void drawTinyTextCentered(const String &text, int y, uint16_t color, int scale);
  void drawFooter(const String &chapterLabel, uint8_t progressPercent);
  void drawWordAt(const String &word, int x, int y, uint16_t color);
  void drawRsvpWordAt(const String &word, int x, int y, int focusIndex);
  void drawWordLine(const String &word, int y, uint16_t color);
  void drawMenuItem(const String &item, int y, bool selected);
  void flushScaledFrame(int scale, int virtualWidth, int virtualHeight);

  uint16_t *virtualFrame_ = nullptr;
  uint16_t *txBuffer_ = nullptr;
  size_t txBufferBytes_ = 0;
  bool initialized_ = false;
  String lastRenderKey_;
};
