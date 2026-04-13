#include "display/DisplayManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "board/BoardConfig.h"
#include "display/EmbeddedSerifFont.h"
#include "display/rm67162.h"

namespace {
constexpr int kDisplayWidth = BoardConfig::DISPLAY_WIDTH;
constexpr int kDisplayHeight = BoardConfig::DISPLAY_HEIGHT;

constexpr int kBaseGlyphHeight = kEmbeddedSerifHeight;
constexpr int kMinTextScale = 1;
constexpr int kMaxTextScale = 1;
constexpr uint8_t kGlyphAlphaThreshold = 16;
constexpr uint16_t kTrueBlack = 0x0000;
constexpr uint16_t kWordColor = 0xFFFF;
constexpr uint16_t kFocusLetterColor = 0xF800;
constexpr uint16_t kMenuDimColor = 0x8410;
constexpr uint16_t kFooterColor = 0x528A;
constexpr uint16_t kMenuSelectedBarColor = 0xF800;
constexpr int kRsvpAnchorPercent = 43;
constexpr int kRsvpSideMargin = 12;
constexpr int kWpmFeedbackBottomMargin = 16;
constexpr int kTinyGlyphWidth = 5;
constexpr int kTinyGlyphHeight = 7;
constexpr int kTinyGlyphSpacing = 1;
constexpr int kTinyScale = 2;
constexpr int kFooterMarginX = 12;
constexpr int kFooterMarginBottom = 8;
constexpr int kCompactMenuRowHeight = 22;
constexpr int kCompactMenuX = 28;

constexpr int kVirtualBufferWidth = (kDisplayWidth + kMinTextScale - 1) / kMinTextScale;
constexpr int kVirtualBufferHeight = (kDisplayHeight + kMinTextScale - 1) / kMinTextScale;

constexpr size_t kBytesPerPixel = sizeof(uint16_t);
constexpr size_t kMaxChunkBytes = 16 * 1024;
constexpr int kMaxChunkPhysicalRows = kMaxChunkBytes / (kDisplayWidth * kBytesPerPixel);
static_assert(kMaxChunkPhysicalRows > 0, "Display chunk buffer must hold at least one row");

constexpr size_t kTxBufferPixels = static_cast<size_t>(kDisplayWidth) * kMaxChunkPhysicalRows;

struct TinyGlyph {
  char c;
  uint8_t rows[kTinyGlyphHeight];
};

constexpr TinyGlyph kTinyGlyphs[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'"', {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'%', {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}},
    {'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},
    {'\'', {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'*', {0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x06, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x06, 0x04, 0x08}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'>', {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11}},
    {'Y', {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
};

const EmbeddedSerifGlyph &glyphFor(char c) {
  if (c < static_cast<char>(kEmbeddedSerifFirstChar) ||
      c > static_cast<char>(kEmbeddedSerifLastChar)) {
    c = '?';
  }

  return kEmbeddedSerifGlyphs[static_cast<uint8_t>(c) - kEmbeddedSerifFirstChar];
}

const uint8_t *tinyRowsFor(char c) {
  if (c >= 'a' && c <= 'z') {
    c = static_cast<char>(c - 'a' + 'A');
  }

  for (const TinyGlyph &glyph : kTinyGlyphs) {
    if (glyph.c == c) {
      return glyph.rows;
    }
  }

  for (const TinyGlyph &glyph : kTinyGlyphs) {
    if (glyph.c == ' ') {
      return glyph.rows;
    }
  }

  return kTinyGlyphs[0].rows;
}

uint16_t panelColor(uint16_t rgb565) {
  return static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
}

uint16_t blendOverTrueBlack(uint16_t rgb565, uint8_t alpha) {
  if (alpha >= 250) {
    return rgb565;
  }

  const uint32_t r = ((rgb565 >> 11) & 0x1F) * alpha / 255;
  const uint32_t g = ((rgb565 >> 5) & 0x3F) * alpha / 255;
  const uint32_t b = (rgb565 & 0x1F) * alpha / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

bool isWordCharacter(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

int serifWordWidth(const String &word) {
  int width = 0;
  for (size_t i = 0; i < word.length(); ++i) {
    width += glyphFor(word[i]).xAdvance;
  }
  return width;
}

int orpOrdinalForLength(int length) {
  if (length <= 1) {
    return 0;
  }
  if (length <= 5) {
    return 1;
  }
  if (length <= 9) {
    return 2;
  }
  if (length <= 13) {
    return 3;
  }
  return 4;
}

int findFocusLetterIndex(const String &word) {
  int wordCharacterCount = 0;
  for (size_t i = 0; i < word.length(); ++i) {
    if (isWordCharacter(word[i])) {
      ++wordCharacterCount;
    }
  }

  if (wordCharacterCount == 0) {
    return word.length() > 0 ? 0 : -1;
  }

  const int targetOrdinal = std::min(orpOrdinalForLength(wordCharacterCount), wordCharacterCount - 1);
  int currentOrdinal = 0;
  for (size_t i = 0; i < word.length(); ++i) {
    if (!isWordCharacter(word[i])) {
      continue;
    }
    if (currentOrdinal == targetOrdinal) {
      return static_cast<int>(i);
    }
    ++currentOrdinal;
  }

  return 0;
}

int rsvpStartX(const String &word, int focusIndex, int virtualWidth) {
  const int wordWidth = serifWordWidth(word);
  if (focusIndex < 0) {
    return (virtualWidth - wordWidth) / 2;
  }

  int focusAdvance = 0;
  for (int i = 0; i < focusIndex; ++i) {
    focusAdvance += glyphFor(word[i]).xAdvance;
  }

  const int focusGlyphWidth = glyphFor(word[focusIndex]).width;
  const int anchorX = (virtualWidth * kRsvpAnchorPercent) / 100;
  const int x = anchorX - focusAdvance - (focusGlyphWidth / 2);
  const int maxX = virtualWidth - kRsvpSideMargin - wordWidth;

  if (maxX < kRsvpSideMargin) {
    return x;
  }

  return std::max(kRsvpSideMargin, std::min(maxX, x));
}

}  // namespace

static const char *kDisplayTag = "display";

DisplayManager::~DisplayManager() {
  if (virtualFrame_ != nullptr) {
    heap_caps_free(virtualFrame_);
    virtualFrame_ = nullptr;
  }

  if (txBuffer_ != nullptr) {
    heap_caps_free(txBuffer_);
    txBuffer_ = nullptr;
  }
}

bool DisplayManager::begin() {
  ESP_LOGI(kDisplayTag, "Begin");

  if (!allocateBuffers()) {
    ESP_LOGE(kDisplayTag, "Buffer allocation failed");
    return false;
  }
  ESP_LOGI(kDisplayTag, "Buffers ready");

  if (!initPanel()) {
    ESP_LOGE(kDisplayTag, "Panel init failed");
    return false;
  }

  initialized_ = true;
  lastRenderKey_ = "";
  fillScreen(kTrueBlack);
  ESP_LOGI(kDisplayTag, "RM67162 AMOLED initialized");
  return true;
}

void DisplayManager::prepareForSleep() {
  if (!initialized_) {
    return;
  }

  fillScreen(kTrueBlack);
  rm67162Sleep();
  initialized_ = false;
  lastRenderKey_ = "";
}

bool DisplayManager::allocateBuffers() {
  if (virtualFrame_ == nullptr) {
    virtualFrame_ = static_cast<uint16_t *>(
        heap_caps_malloc(kVirtualBufferWidth * kVirtualBufferHeight * sizeof(uint16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (virtualFrame_ == nullptr) {
      virtualFrame_ = static_cast<uint16_t *>(
          heap_caps_malloc(kVirtualBufferWidth * kVirtualBufferHeight * sizeof(uint16_t),
                           MALLOC_CAP_8BIT));
    }
  }

  if (txBuffer_ == nullptr) {
    txBufferBytes_ = kTxBufferPixels * sizeof(uint16_t);
    txBuffer_ = static_cast<uint16_t *>(
        heap_caps_malloc(txBufferBytes_, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  }

  return virtualFrame_ != nullptr && txBuffer_ != nullptr;
}

bool DisplayManager::initPanel() {
  rm67162Init();
  rm67162SetRotation(1);
  ESP_LOGI(kDisplayTag, "Panel init sequence complete");
  return true;
}

bool DisplayManager::drawBitmap(int xStart, int yStart, int xEnd, int yEnd, const void *colorData) {
  if (colorData == nullptr || xEnd <= xStart || yEnd <= yStart) {
    return false;
  }

  rm67162PushColors(static_cast<uint16_t>(xStart), static_cast<uint16_t>(yStart),
                    static_cast<uint16_t>(xEnd - xStart), static_cast<uint16_t>(yEnd - yStart),
                    static_cast<const uint16_t *>(colorData));
  return true;
}

void DisplayManager::fillScreen(uint16_t color) {
  if (txBuffer_ == nullptr) {
    return;
  }

  for (size_t i = 0; i < kTxBufferPixels; ++i) {
    txBuffer_[i] = panelColor(color);
  }

  for (int yStart = 0; yStart < kDisplayHeight; yStart += kMaxChunkPhysicalRows) {
    const int rows = std::min(kMaxChunkPhysicalRows, kDisplayHeight - yStart);
    if (!drawBitmap(0, yStart, kDisplayWidth, yStart + rows, txBuffer_)) {
      return;
    }
  }
}

void DisplayManager::clearVirtualBuffer(int width, int height) {
  for (int row = 0; row < height; ++row) {
    std::memset(virtualFrame_ + row * kVirtualBufferWidth, 0, width * sizeof(uint16_t));
  }
}

int DisplayManager::chooseTextScale(const String &word) const {
  const int usableWidth = std::max(1, measureTextWidth(word));
  const int maxScaleX = kDisplayWidth / usableWidth;
  const int maxScaleY = kDisplayHeight / kBaseGlyphHeight;
  const int maxScale = std::min(kMaxTextScale, std::min(maxScaleX, maxScaleY));
  return std::max(1, maxScale);
}

int DisplayManager::measureTextWidth(const String &word) const {
  int width = 0;
  for (size_t i = 0; i < word.length(); ++i) {
    width += glyphFor(word[i]).xAdvance;
  }
  return width;
}

int DisplayManager::measureTinyTextWidth(const String &text, int scale) const {
  if (text.isEmpty()) {
    return 0;
  }
  return static_cast<int>(text.length()) * (kTinyGlyphWidth + kTinyGlyphSpacing) * scale -
         kTinyGlyphSpacing * scale;
}

String DisplayManager::fitTinyText(const String &text, int maxWidth, int scale) const {
  if (measureTinyTextWidth(text, scale) <= maxWidth) {
    return text;
  }

  String fitted = text;
  const String ellipsis = "...";
  while (!fitted.isEmpty() && measureTinyTextWidth(fitted + ellipsis, scale) > maxWidth) {
    fitted.remove(fitted.length() - 1);
  }
  fitted.trim();
  return fitted.isEmpty() ? ellipsis : fitted + ellipsis;
}

void DisplayManager::drawGlyph(int x, int y, char c, uint16_t color) {
  const EmbeddedSerifGlyph &glyph = glyphFor(c);
  if (glyph.width == 0) {
    return;
  }

  const uint8_t *bitmap = kEmbeddedSerifBitmaps + glyph.bitmapOffset;
  for (int row = 0; row < kBaseGlyphHeight; ++row) {
    const int dstY = y + row;
    if (dstY < 0 || dstY >= kVirtualBufferHeight) {
      continue;
    }

    for (int col = 0; col < glyph.width; ++col) {
      const int dstX = x + col;
      if (dstX < 0 || dstX >= kVirtualBufferWidth) {
        continue;
      }

      const uint8_t alpha = bitmap[row * glyph.width + col];
      if (alpha < kGlyphAlphaThreshold) {
        continue;
      }

      virtualFrame_[dstY * kVirtualBufferWidth + dstX] =
          panelColor(blendOverTrueBlack(color, alpha));
    }
  }
}

void DisplayManager::fillVirtualRect(int x, int y, int width, int height, uint16_t color) {
  const uint16_t panel = panelColor(color);
  const int xEnd = std::min(kVirtualBufferWidth, x + width);
  const int yEnd = std::min(kVirtualBufferHeight, y + height);
  x = std::max(0, x);
  y = std::max(0, y);

  for (int row = y; row < yEnd; ++row) {
    for (int col = x; col < xEnd; ++col) {
      virtualFrame_[row * kVirtualBufferWidth + col] = panel;
    }
  }
}

void DisplayManager::drawTinyGlyph(int x, int y, char c, uint16_t color, int scale) {
  const uint8_t *rows = tinyRowsFor(c);
  const uint16_t panel = panelColor(color);

  for (int row = 0; row < kTinyGlyphHeight; ++row) {
    for (int col = 0; col < kTinyGlyphWidth; ++col) {
      if ((rows[row] & (1 << (kTinyGlyphWidth - 1 - col))) == 0) {
        continue;
      }

      for (int yy = 0; yy < scale; ++yy) {
        const int dstY = y + row * scale + yy;
        if (dstY < 0 || dstY >= kVirtualBufferHeight) {
          continue;
        }

        for (int xx = 0; xx < scale; ++xx) {
          const int dstX = x + col * scale + xx;
          if (dstX < 0 || dstX >= kVirtualBufferWidth) {
            continue;
          }
          virtualFrame_[dstY * kVirtualBufferWidth + dstX] = panel;
        }
      }
    }
  }
}

void DisplayManager::drawTinyTextAt(const String &text, int x, int y, uint16_t color, int scale) {
  int cursorX = x;
  for (size_t i = 0; i < text.length(); ++i) {
    drawTinyGlyph(cursorX, y, text[i], color, scale);
    cursorX += (kTinyGlyphWidth + kTinyGlyphSpacing) * scale;
  }
}

void DisplayManager::drawTinyTextCentered(const String &text, int y, uint16_t color, int scale) {
  const int textWidth = measureTinyTextWidth(text, scale);
  drawTinyTextAt(text, std::max(0, (kVirtualBufferWidth - textWidth) / 2), y, color, scale);
}

void DisplayManager::drawFooter(const String &chapterLabel, uint8_t progressPercent) {
  const String percent = String(progressPercent) + "%";
  const int y = kDisplayHeight - kTinyGlyphHeight * kTinyScale - kFooterMarginBottom;
  const int percentWidth = measureTinyTextWidth(percent, kTinyScale);
  const int rightX = std::max(kFooterMarginX, kDisplayWidth - kFooterMarginX - percentWidth);
  const int maxChapterWidth = std::max(0, rightX - kFooterMarginX - 18);
  const String chapter = fitTinyText(chapterLabel.isEmpty() ? "START" : chapterLabel,
                                    maxChapterWidth, kTinyScale);

  drawTinyTextAt(chapter, kFooterMarginX, y, kFooterColor, kTinyScale);
  drawTinyTextAt(percent, rightX, y, kFooterColor, kTinyScale);
}

void DisplayManager::drawWordAt(const String &word, int x, int y, uint16_t color) {
  int cursorX = x;
  for (size_t i = 0; i < word.length(); ++i) {
    const EmbeddedSerifGlyph &glyph = glyphFor(word[i]);
    drawGlyph(cursorX, y, word[i], color);
    cursorX += glyph.xAdvance;
  }
}

void DisplayManager::drawRsvpWordAt(const String &word, int x, int y, int focusIndex) {
  int cursorX = x;
  for (size_t i = 0; i < word.length(); ++i) {
    const EmbeddedSerifGlyph &glyph = glyphFor(word[i]);
    const uint16_t color = (static_cast<int>(i) == focusIndex) ? kFocusLetterColor : kWordColor;
    drawGlyph(cursorX, y, word[i], color);
    cursorX += glyph.xAdvance;
  }
}

void DisplayManager::drawWordLine(const String &word, int y, uint16_t color) {
  const int textWidth = std::max(0, measureTextWidth(word));
  const int x = std::max(0, (kVirtualBufferWidth - textWidth) / 2);
  drawWordAt(word, x, y, color);
}

void DisplayManager::drawMenuItem(const String &item, int y, bool selected) {
  drawWordLine(item, y, selected ? kFocusLetterColor : kMenuDimColor);
}

void DisplayManager::flushScaledFrame(int scale, int virtualWidth, int virtualHeight) {
  const int virtualRowsPerChunk = std::max(1, kMaxChunkPhysicalRows / scale);

  for (int vyStart = 0; vyStart < virtualHeight; vyStart += virtualRowsPerChunk) {
    const int virtualRows = std::min(virtualRowsPerChunk, virtualHeight - vyStart);
    const int yStart = vyStart * scale;
    const int yEnd = std::min(kDisplayHeight, yStart + virtualRows * scale);
    const int physicalRows = yEnd - yStart;

    std::memset(txBuffer_, 0, txBufferBytes_);

    for (int localVy = 0; localVy < virtualRows; ++localVy) {
      const uint16_t *sourceRow = virtualFrame_ + (vyStart + localVy) * kVirtualBufferWidth;

      for (int repeatY = 0; repeatY < scale; ++repeatY) {
        const int dstRowIndex = localVy * scale + repeatY;
        if (dstRowIndex >= physicalRows || dstRowIndex >= kMaxChunkPhysicalRows) {
          break;
        }

        uint16_t *dstRow = txBuffer_ + dstRowIndex * kDisplayWidth;
        for (int vx = 0; vx < virtualWidth; ++vx) {
          const uint16_t pixel = sourceRow[vx];
          const int xBase = vx * scale;
          for (int repeatX = 0; repeatX < scale; ++repeatX) {
            const int dstX = xBase + repeatX;
            if (dstX >= kDisplayWidth) {
              break;
            }
            dstRow[dstX] = pixel;
          }
        }
      }
    }

    if (!drawBitmap(0, yStart, kDisplayWidth, yEnd, txBuffer_)) {
      return;
    }
  }
}

void DisplayManager::renderCenteredWord(const String &word, uint16_t color) {
  String normalized = word;
  const String renderKey = "center|" + normalized + "|" + String(color);

  if (!initialized_ || renderKey == lastRenderKey_) {
    return;
  }

  lastRenderKey_ = renderKey;

  const int scale = chooseTextScale(normalized);
  const int virtualWidth = (kDisplayWidth + scale - 1) / scale;
  const int virtualHeight = (kDisplayHeight + scale - 1) / scale;

  clearVirtualBuffer(virtualWidth, virtualHeight);
  const int y = std::max(0, (virtualHeight - kBaseGlyphHeight) / 2);
  drawWordLine(normalized, y, color);

  flushScaledFrame(scale, virtualWidth, virtualHeight);
}

void DisplayManager::renderRsvpWord(const String &word, const String &chapterLabel,
                                    uint8_t progressPercent, bool showFooter) {
  const String renderKey =
      "rsvp|" + word + "|" + chapterLabel + "|" + String(progressPercent) + "|" +
      String(showFooter ? 1 : 0);
  if (!initialized_ || renderKey == lastRenderKey_) {
    return;
  }

  lastRenderKey_ = renderKey;

  const int scale = 1;
  const int virtualWidth = kDisplayWidth;
  const int virtualHeight = kDisplayHeight;
  const int y = std::max(0, (virtualHeight - kBaseGlyphHeight) / 2);
  const int focusIndex = findFocusLetterIndex(word);
  const int x = rsvpStartX(word, focusIndex, virtualWidth);

  clearVirtualBuffer(virtualWidth, virtualHeight);
  drawRsvpWordAt(word, x, y, focusIndex);
  if (showFooter) {
    drawFooter(chapterLabel, progressPercent);
  }

  flushScaledFrame(scale, virtualWidth, virtualHeight);
}

void DisplayManager::renderRsvpWordWithWpm(const String &word, uint16_t wpm,
                                           const String &chapterLabel, uint8_t progressPercent,
                                           bool showFooter) {
  const String wpmText = String(wpm) + " WPM";
  const String renderKey =
      "rsvp_wpm|" + word + "|" + wpmText + "|" + chapterLabel + "|" +
      String(progressPercent) + "|" + String(showFooter ? 1 : 0);
  if (!initialized_ || renderKey == lastRenderKey_) {
    return;
  }

  lastRenderKey_ = renderKey;

  const int scale = 1;
  const int virtualWidth = kDisplayWidth;
  const int virtualHeight = kDisplayHeight;
  const int wordY = std::max(0, (virtualHeight - kBaseGlyphHeight) / 2);
  const int wpmY =
      std::max(0, virtualHeight - kTinyGlyphHeight * kTinyScale - kWpmFeedbackBottomMargin - 24);
  const int focusIndex = findFocusLetterIndex(word);
  const int x = rsvpStartX(word, focusIndex, virtualWidth);

  clearVirtualBuffer(virtualWidth, virtualHeight);
  drawRsvpWordAt(word, x, wordY, focusIndex);
  drawTinyTextCentered(wpmText, wpmY, kFocusLetterColor, kTinyScale);
  if (showFooter) {
    drawFooter(chapterLabel, progressPercent);
  }

  flushScaledFrame(scale, virtualWidth, virtualHeight);
}

void DisplayManager::renderMenu(const char *const *items, size_t itemCount, size_t selectedIndex) {
  if (items == nullptr || itemCount == 0) {
    renderCenteredWord("MENU");
    return;
  }

  std::vector<String> menuItems;
  menuItems.reserve(itemCount);
  for (size_t i = 0; i < itemCount; ++i) {
    menuItems.push_back(items[i] == nullptr ? "" : items[i]);
  }

  renderMenu(menuItems, selectedIndex);
}

void DisplayManager::renderMenu(const std::vector<String> &items, size_t selectedIndex) {
  if (items.empty()) {
    renderCenteredWord("MENU");
    return;
  }

  if (selectedIndex >= items.size()) {
    selectedIndex = items.size() - 1;
  }

  String renderKey = "menuv|";
  renderKey += String(selectedIndex);
  for (const String &item : items) {
    renderKey += "|";
    renderKey += item;
  }

  if (!initialized_ || renderKey == lastRenderKey_) {
    return;
  }

  lastRenderKey_ = renderKey;

  const int scale = 1;
  const int virtualWidth = kDisplayWidth;
  const int virtualHeight = kDisplayHeight;
  const size_t itemCount = items.size();
  const size_t visibleCount =
      std::min(itemCount, static_cast<size_t>(std::max(1, virtualHeight / kCompactMenuRowHeight)));
  size_t firstVisible = 0;
  if (selectedIndex >= visibleCount / 2) {
    firstVisible = selectedIndex - visibleCount / 2;
  }
  if (firstVisible + visibleCount > itemCount) {
    firstVisible = itemCount - visibleCount;
  }

  const int rowHeight = kCompactMenuRowHeight;
  const int totalHeight = rowHeight * static_cast<int>(visibleCount);
  int y = std::max(0, (virtualHeight - totalHeight) / 2);

  clearVirtualBuffer(virtualWidth, virtualHeight);

  for (size_t row = 0; row < visibleCount; ++row) {
    const size_t itemIndex = firstVisible + row;
    const bool selected = itemIndex == selectedIndex;
    const uint16_t color = selected ? kFocusLetterColor : kMenuDimColor;
    const int maxWidth = virtualWidth - kCompactMenuX - 16;
    if (selected) {
      fillVirtualRect(10, y + 2, 5, kTinyGlyphHeight * kTinyScale + 2, kMenuSelectedBarColor);
    }
    drawTinyTextAt(fitTinyText(items[itemIndex], maxWidth, kTinyScale), kCompactMenuX, y + 3, color,
                   kTinyScale);
    y += rowHeight;
  }

  flushScaledFrame(scale, virtualWidth, virtualHeight);
}
