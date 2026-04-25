#include "storage/StorageManager.h"

#include <SD_MMC.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <driver/sdmmc_types.h>
#include <esp_heap_caps.h>
#include <utility>

#include "board/BoardConfig.h"
#include "storage/BookIndex.h"
#include "storage/EpubConverter.h"

namespace {
constexpr const char *kIdxConverterTag = "idx-v1";
}

#ifndef RSVP_ON_DEVICE_EPUB_CONVERSION
#define RSVP_ON_DEVICE_EPUB_CONVERSION 0
#endif

#ifndef RSVP_MAX_BOOK_WORDS
#define RSVP_MAX_BOOK_WORDS 0
#endif

namespace {

constexpr const char *kMountPoint = "/sdcard";
constexpr const char *kBooksPath = "/books";
constexpr size_t kMaxBookWords = static_cast<size_t>(RSVP_MAX_BOOK_WORDS);
constexpr size_t kMaxChapterTitleChars = 64;
constexpr int kSdFrequenciesKhz[] = {
    SDMMC_FREQ_DEFAULT,
    10000,
    SDMMC_FREQ_PROBING,
};

bool hasBookWordLimit() { return kMaxBookWords > 0; }

bool reachedBookWordLimit(size_t wordCount) {
  return hasBookWordLimit() && wordCount >= kMaxBookWords;
}

bool isWordBoundary(char c) {
  // Use unsigned comparison so UTF-8 continuation / lead bytes (>= 0x80), which
  // are negative on signed-char platforms, are not mistaken for control
  // characters and used to split Polish or other non-ASCII words.
  return static_cast<unsigned char>(c) <= ' ';
}

bool prefixHasBoundary(const String &lowered, const char *prefix) {
  const size_t prefixLength = std::strlen(prefix);
  if (!lowered.startsWith(prefix)) {
    return false;
  }
  if (lowered.length() == prefixLength) {
    return true;
  }

  const char next = lowered[prefixLength];
  return next <= ' ' || next == ':' || next == '.' || next == '-';
}

bool booksDirectoryExists() {
  File dir = SD_MMC.open(kBooksPath);
  const bool exists = dir && dir.isDirectory();
  if (dir) {
    dir.close();
  }
  return exists;
}

bool hasTextExtension(const String &path) {
  String lowered = path;
  lowered.toLowerCase();
  return lowered.endsWith(".txt");
}

bool hasRsvpExtension(const String &path) {
  String lowered = path;
  lowered.toLowerCase();
  return lowered.endsWith(".rsvp");
}

bool hasEpubExtension(const String &path) {
  String lowered = path;
  lowered.toLowerCase();
  return lowered.endsWith(".epub");
}

bool hasRsvpSibling(const String &path) {
  String siblingPath = path;
  const int dot = siblingPath.lastIndexOf('.');
  if (dot > 0) {
    siblingPath = siblingPath.substring(0, dot);
  }
  siblingPath += ".rsvp";

  File sibling = SD_MMC.open(siblingPath);
  const bool exists = sibling && !sibling.isDirectory() && sibling.size() > 0;
  if (sibling) {
    sibling.close();
  }
  return exists;
}

String epubSiblingPathForRsvp(const String &rsvpPath) {
  String epubPath = rsvpPath;
  const int dot = epubPath.lastIndexOf('.');
  if (dot > 0) {
    epubPath = epubPath.substring(0, dot);
  }
  epubPath += ".epub";
  return epubPath;
}

String normalizeBookPath(const String &path) {
  if (path.startsWith("/")) {
    return path;
  }
  return String(kBooksPath) + "/" + path;
}

String displayNameForPath(const String &path) {
  const int separator = path.lastIndexOf('/');
  if (separator < 0) {
    return path;
  }
  return path.substring(separator + 1);
}

String displayNameWithoutExtension(const String &path) {
  String name = displayNameForPath(path);
  String lowered = name;
  lowered.toLowerCase();
  if (lowered.endsWith(".txt")) {
    name.remove(name.length() - 4);
  } else if (lowered.endsWith(".rsvp")) {
    name.remove(name.length() - 5);
  } else if (lowered.endsWith(".epub")) {
    name.remove(name.length() - 5);
  }
  return name;
}

String rsvpCachePathForEpub(const String &epubPath) {
  String outputPath = epubPath;
  const int dot = outputPath.lastIndexOf('.');
  if (dot > 0) {
    outputPath = outputPath.substring(0, dot);
  }
  outputPath += ".rsvp";
  return outputPath;
}

struct EpubProgressContext {
  StorageManager::StatusCallback statusCallback = nullptr;
  void *statusContext = nullptr;
  String title;
  String label;
  int basePercent = 0;
  int spanPercent = 100;
};

void handleEpubProgress(void *rawContext, const char *line1, const char *line2,
                        int progressPercent) {
  EpubProgressContext *context = static_cast<EpubProgressContext *>(rawContext);
  if (context == nullptr) {
    return;
  }

  progressPercent = std::max(0, std::min(100, progressPercent));
  const int overallPercent =
      context->basePercent + ((context->spanPercent * progressPercent) / 100);
  const String detail = String(line1 == nullptr ? "" : line1) + " - " +
                        String(line2 == nullptr ? "" : line2);
  const char *title = context->title.isEmpty() ? "EPUB" : context->title.c_str();
  Serial.printf("[epub-progress] %d%% %s | %s | %s\n", overallPercent, title,
                context->label.c_str(), detail.c_str());

  // Keep the display on the static "Converting EPUB" screen while ZIP work is active.
  // Full-screen redraws from inside this callback have proven risky while the SD archive is open.
  yield();
  delay(0);
}

bool fileExistsAndHasBytes(const String &path) {
  File file = SD_MMC.open(path);
  const bool exists = file && !file.isDirectory() && file.size() > 0;
  if (file) {
    file.close();
  }
  return exists;
}

bool hasCurrentEpubCache(const String &epubPath) {
  const String rsvpPath = rsvpCachePathForEpub(epubPath);
  return fileExistsAndHasBytes(rsvpPath) && EpubConverter::isCurrentCache(rsvpPath);
}

bool markerExists(const String &path) {
  File file = SD_MMC.open(path);
  const bool exists = file && !file.isDirectory();
  if (file) {
    file.close();
  }
  return exists;
}

String epubLibraryLabel(const String &path) {
  const String rsvpPath = rsvpCachePathForEpub(path);
  if (markerExists(rsvpPath + ".failed")) {
    return "EPUB failed - check serial";
  }
  if (markerExists(rsvpPath + ".converting") || markerExists(rsvpPath + ".tmp")) {
    return "EPUB interrupted";
  }
  return "EPUB - converts on open";
}

int pathIndexIn(const std::vector<String> &paths, const String &target) {
  for (size_t i = 0; i < paths.size(); ++i) {
    if (paths[i] == target) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void logHeapSnapshot(const char *label) {
  Serial.printf("[heap] %s free8=%lu free_spiram=%lu largest8=%lu largest_spiram=%lu\n",
                label == nullptr ? "" : label,
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

std::vector<String> collectBookPaths() {
  std::vector<String> bookPaths;

  File dir = SD_MMC.open(kBooksPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return bookPaths;
  }

  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const String path = normalizeBookPath(String(entry.name()));
      const bool staleGeneratedRsvp =
          hasRsvpExtension(path) && fileExistsAndHasBytes(epubSiblingPathForRsvp(path)) &&
          !EpubConverter::isCurrentCache(path);
      const bool readableText = hasTextExtension(path) && !hasRsvpSibling(path);
      const bool pendingEpub =
          RSVP_ON_DEVICE_EPUB_CONVERSION && hasEpubExtension(path) && !hasCurrentEpubCache(path);
      if ((!staleGeneratedRsvp && hasRsvpExtension(path)) || readableText || pendingEpub) {
        bookPaths.push_back(path);
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();

  std::sort(bookPaths.begin(), bookPaths.end(), [](const String &left, const String &right) {
    String leftKey = displayNameForPath(left);
    String rightKey = displayNameForPath(right);
    leftKey.toLowerCase();
    rightKey.toLowerCase();
    return leftKey < rightKey;
  });

  return bookPaths;
}

bool isTrimmableEdgeChar(char c) {
  switch (c) {
    case '"':
    case '\'':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '<':
    case '>':
      return true;
    default:
      return false;
  }
}

bool isUtf8Continuation(uint8_t value) { return (value & 0xC0) == 0x80; }

bool decodeUtf8Codepoint(const String &text, size_t &index, uint32_t &codepoint) {
  const uint8_t first = static_cast<uint8_t>(text[index++]);
  if (first < 0x80) {
    codepoint = first;
    return true;
  }

  uint8_t continuationCount = 0;
  uint32_t minimumValue = 0;
  if ((first & 0xE0) == 0xC0) {
    codepoint = first & 0x1F;
    continuationCount = 1;
    minimumValue = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    codepoint = first & 0x0F;
    continuationCount = 2;
    minimumValue = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    codepoint = first & 0x07;
    continuationCount = 3;
    minimumValue = 0x10000;
  } else {
    return false;
  }

  if (index + continuationCount > text.length()) {
    return false;
  }

  for (uint8_t i = 0; i < continuationCount; ++i) {
    const uint8_t next = static_cast<uint8_t>(text[index]);
    if (!isUtf8Continuation(next)) {
      return false;
    }
    ++index;
    codepoint = (codepoint << 6) | (next & 0x3F);
  }

  if (codepoint < minimumValue || codepoint > 0x10FFFF ||
      (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return false;
  }

  return true;
}

void appendAsciiText(String &target, const char *text) {
  while (*text != '\0') {
    target += *text;
    ++text;
  }
}

void appendAsciiApproximation(String &target, uint32_t codepoint) {
  if (codepoint >= 32 && codepoint <= 126) {
    target += static_cast<char>(codepoint);
    return;
  }

  if (codepoint == '\t' || codepoint == '\n' || codepoint == '\r' || codepoint == 0x00A0 ||
      codepoint == 0x1680 || codepoint == 0x180E || codepoint == 0x2028 ||
      codepoint == 0x2029 || codepoint == 0x202F || codepoint == 0x205F ||
      codepoint == 0x3000 || (codepoint >= 0x2000 && codepoint <= 0x200A)) {
    target += ' ';
    return;
  }

  if ((codepoint >= 0x00C0 && codepoint <= 0x00C5) || codepoint == 0x0100 ||
      codepoint == 0x0102 || codepoint == 0x0104) {
    target += 'A';
    return;
  }
  if ((codepoint >= 0x00E0 && codepoint <= 0x00E5) || codepoint == 0x0101 ||
      codepoint == 0x0103 || codepoint == 0x0105) {
    target += 'a';
    return;
  }
  if (codepoint >= 0x00C8 && codepoint <= 0x00CB) {
    target += 'E';
    return;
  }
  if (codepoint >= 0x00E8 && codepoint <= 0x00EB) {
    target += 'e';
    return;
  }
  if (codepoint >= 0x00CC && codepoint <= 0x00CF) {
    target += 'I';
    return;
  }
  if (codepoint >= 0x00EC && codepoint <= 0x00EF) {
    target += 'i';
    return;
  }
  if ((codepoint >= 0x00D2 && codepoint <= 0x00D6) || codepoint == 0x00D8) {
    target += 'O';
    return;
  }
  if ((codepoint >= 0x00F2 && codepoint <= 0x00F6) || codepoint == 0x00F8) {
    target += 'o';
    return;
  }
  if (codepoint >= 0x00D9 && codepoint <= 0x00DC) {
    target += 'U';
    return;
  }
  if (codepoint >= 0x00F9 && codepoint <= 0x00FC) {
    target += 'u';
    return;
  }

  switch (codepoint) {
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
    case 0x2032:
    case 0x2035:
      target += '\'';
      return;
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
    case 0x00AB:
    case 0x00BB:
    case 0x2033:
    case 0x2036:
      target += '"';
      return;
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2043:
    case 0x2212:
      target += '-';
      return;
    case 0x2026:
      appendAsciiText(target, "...");
      return;
    case 0x2022:
    case 0x00B7:
    case 0x2219:
      target += '*';
      return;
    case 0x00A9:
      appendAsciiText(target, "(c)");
      return;
    case 0x00AE:
      appendAsciiText(target, "(r)");
      return;
    case 0x2122:
      appendAsciiText(target, "TM");
      return;
    case 0x00C6:
    case 0x01E2:
    case 0x01FC:
      appendAsciiText(target, "AE");
      return;
    case 0x00E6:
    case 0x01E3:
    case 0x01FD:
      appendAsciiText(target, "ae");
      return;
    case 0x00C7:
    case 0x0106:
    case 0x0108:
    case 0x010A:
    case 0x010C:
      target += 'C';
      return;
    case 0x00E7:
    case 0x0107:
    case 0x0109:
    case 0x010B:
    case 0x010D:
      target += 'c';
      return;
    case 0x00D0:
    case 0x010E:
    case 0x0110:
      target += 'D';
      return;
    case 0x00F0:
    case 0x010F:
    case 0x0111:
      target += 'd';
      return;
    case 0x0112:
    case 0x0114:
    case 0x0116:
    case 0x0118:
    case 0x011A:
      target += 'E';
      return;
    case 0x0113:
    case 0x0115:
    case 0x0117:
    case 0x0119:
    case 0x011B:
      target += 'e';
      return;
    case 0x011C:
    case 0x011E:
    case 0x0120:
    case 0x0122:
      target += 'G';
      return;
    case 0x011D:
    case 0x011F:
    case 0x0121:
    case 0x0123:
      target += 'g';
      return;
    case 0x0124:
    case 0x0126:
      target += 'H';
      return;
    case 0x0125:
    case 0x0127:
      target += 'h';
      return;
    case 0x0128:
    case 0x012A:
    case 0x012C:
    case 0x012E:
    case 0x0130:
      target += 'I';
      return;
    case 0x0129:
    case 0x012B:
    case 0x012D:
    case 0x012F:
    case 0x0131:
      target += 'i';
      return;
    case 0x0134:
      target += 'J';
      return;
    case 0x0135:
      target += 'j';
      return;
    case 0x0136:
      target += 'K';
      return;
    case 0x0137:
      target += 'k';
      return;
    case 0x0141:
      target += 'L';
      return;
    case 0x0142:
      target += 'l';
      return;
    case 0x00D1:
    case 0x0143:
    case 0x0145:
    case 0x0147:
      target += 'N';
      return;
    case 0x00F1:
    case 0x0144:
    case 0x0146:
    case 0x0148:
      target += 'n';
      return;
    case 0x014C:
    case 0x014E:
    case 0x0150:
      target += 'O';
      return;
    case 0x014D:
    case 0x014F:
    case 0x0151:
      target += 'o';
      return;
    case 0x0152:
      appendAsciiText(target, "OE");
      return;
    case 0x0153:
      appendAsciiText(target, "oe");
      return;
    case 0x0154:
    case 0x0156:
    case 0x0158:
      target += 'R';
      return;
    case 0x0155:
    case 0x0157:
    case 0x0159:
      target += 'r';
      return;
    case 0x015A:
    case 0x015C:
    case 0x015E:
    case 0x0160:
      target += 'S';
      return;
    case 0x015B:
    case 0x015D:
    case 0x015F:
    case 0x0161:
      target += 's';
      return;
    case 0x00DF:
      appendAsciiText(target, "ss");
      return;
    case 0x0162:
    case 0x0164:
    case 0x0166:
      target += 'T';
      return;
    case 0x0163:
    case 0x0165:
    case 0x0167:
      target += 't';
      return;
    case 0x0168:
    case 0x016A:
    case 0x016C:
    case 0x016E:
    case 0x0170:
    case 0x0172:
      target += 'U';
      return;
    case 0x0169:
    case 0x016B:
    case 0x016D:
    case 0x016F:
    case 0x0171:
    case 0x0173:
      target += 'u';
      return;
    case 0x0174:
      target += 'W';
      return;
    case 0x0175:
      target += 'w';
      return;
    case 0x00DD:
    case 0x0176:
    case 0x0178:
      target += 'Y';
      return;
    case 0x00FD:
    case 0x00FF:
    case 0x0177:
      target += 'y';
      return;
    case 0x0179:
    case 0x017B:
    case 0x017D:
      target += 'Z';
      return;
    case 0x017A:
    case 0x017C:
    case 0x017E:
      target += 'z';
      return;
    case 0x00DE:
      appendAsciiText(target, "Th");
      return;
    case 0x00FE:
      appendAsciiText(target, "th");
      return;
    case 0xFB00:
      appendAsciiText(target, "ff");
      return;
    case 0xFB01:
      appendAsciiText(target, "fi");
      return;
    case 0xFB02:
      appendAsciiText(target, "fl");
      return;
    case 0xFB03:
      appendAsciiText(target, "ffi");
      return;
    case 0xFB04:
      appendAsciiText(target, "ffl");
      return;
    case 0xFB05:
    case 0xFB06:
      appendAsciiText(target, "st");
      return;
    default:
      return;
  }
}

void appendSingleByteApproximation(String &target, uint8_t value) {
  switch (value) {
    case 0x82:
    case 0x91:
    case 0x92:
      target += '\'';
      return;
    case 0x84:
    case 0x93:
    case 0x94:
      target += '"';
      return;
    case 0x96:
    case 0x97:
      target += '-';
      return;
    case 0x85:
      appendAsciiText(target, "...");
      return;
    case 0x95:
      target += '*';
      return;
    case 0x99:
      appendAsciiText(target, "TM");
      return;
    default:
      if (value >= 0xA0) {
        appendAsciiApproximation(target, value);
      }
      return;
  }
}

String normalizeDisplayText(const String &text) {
  // Decode each UTF-8 code point. Letters, digits, and general punctuation are
  // preserved verbatim as UTF-8 so the display renderer (which now handles
  // Polish and Latin-1) draws them correctly. Only whitespace-like code points
  // and a handful of typographic glyphs that read better as their ASCII
  // equivalents (curly quotes, dashes, ligatures, legal marks) are rewritten.
  String working;
  working.reserve(text.length());

  size_t index = 0;
  while (index < text.length()) {
    const size_t before = index;
    uint32_t codepoint = 0;
    if (!decodeUtf8Codepoint(text, index, codepoint)) {
      // Invalid UTF-8 byte: skip it rather than splicing garbage through.
      index = before + 1;
      continue;
    }

    // Whitespace / unusual spacing: collapse to a regular ASCII space so the
    // tokeniser treats it as a word boundary.
    if (codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
        codepoint == 0x00A0 || codepoint == 0x1680 || codepoint == 0x180E ||
        codepoint == 0x2028 || codepoint == 0x2029 || codepoint == 0x202F ||
        codepoint == 0x205F || codepoint == 0x3000 ||
        (codepoint >= 0x2000 && codepoint <= 0x200A)) {
      working += ' ';
      continue;
    }

    bool rewritten = true;
    switch (codepoint) {
      case 0x2018: case 0x2019: case 0x201A: case 0x201B:
      case 0x2032: case 0x2035:
        working += '\'';
        break;
      case 0x201C: case 0x201D: case 0x201E: case 0x201F:
      case 0x00AB: case 0x00BB: case 0x2033: case 0x2036:
        working += '"';
        break;
      case 0x2010: case 0x2011: case 0x2012: case 0x2013:
      case 0x2014: case 0x2015: case 0x2043: case 0x2212:
        working += '-';
        break;
      case 0x2026:
        appendAsciiText(working, "...");
        break;
      case 0x00A9:
        appendAsciiText(working, "(c)");
        break;
      case 0x00AE:
        appendAsciiText(working, "(r)");
        break;
      case 0x2122:
        appendAsciiText(working, "TM");
        break;
      case 0xFB00:
        appendAsciiText(working, "ff");
        break;
      case 0xFB01:
        appendAsciiText(working, "fi");
        break;
      case 0xFB02:
        appendAsciiText(working, "fl");
        break;
      case 0xFB03:
        appendAsciiText(working, "ffi");
        break;
      case 0xFB04:
        appendAsciiText(working, "ffl");
        break;
      case 0xFB05: case 0xFB06:
        appendAsciiText(working, "st");
        break;
      default:
        rewritten = false;
        break;
    }
    if (rewritten) {
      continue;
    }

    // Anything else (ASCII, Latin-1 letters, Polish letters, ...) is preserved
    // as its original UTF-8 bytes.
    for (size_t i = before; i < index; ++i) {
      working += text[i];
    }
  }

  // Collapse runs of ASCII spaces and strip a trailing space.
  String collapsed;
  collapsed.reserve(working.length());
  bool previousSpace = true;
  for (size_t i = 0; i < working.length(); ++i) {
    const char c = working[i];
    if (c == ' ') {
      if (!previousSpace) {
        collapsed += ' ';
        previousSpace = true;
      }
      continue;
    }
    collapsed += c;
    previousSpace = false;
  }

  if (!collapsed.isEmpty() && collapsed[collapsed.length() - 1] == ' ') {
    collapsed.remove(collapsed.length() - 1, 1);
  }
  return collapsed;
}

// Abstract sink that consumes the cleaned word stream produced by parsing a
// `.rsvp` or `.txt` book. Two implementations exist (further down): an
// in-memory sink for small books and a streaming sink that writes a sidecar
// `.idx` for large books so we never load every word into RAM.
class WordSink {
 public:
  virtual ~WordSink() = default;
  // Returns false if the sink wants the parser to stop.
  virtual bool addWord(const String &word) = 0;
  virtual void addParagraph() = 0;
  virtual void addChapter(const String &title) = 0;
  virtual size_t wordCount() const = 0;
  virtual void setTitle(const String &title) = 0;
  virtual void setAuthor(const String &author) = 0;
};

bool cleanWordToken(String token, String &out) {
  out = "";
  token.trim();

  if (token.length() >= 3 && static_cast<uint8_t>(token[0]) == 0xEF &&
      static_cast<uint8_t>(token[1]) == 0xBB && static_cast<uint8_t>(token[2]) == 0xBF) {
    token.remove(0, 3);
  }

  token = normalizeDisplayText(token);
  token.trim();

  while (!token.isEmpty() && isTrimmableEdgeChar(token[0])) {
    token.remove(0, 1);
  }

  while (!token.isEmpty() && isTrimmableEdgeChar(token[token.length() - 1])) {
    token.remove(token.length() - 1, 1);
  }

  bool hasAlphaNumeric = false;
  for (size_t i = 0; i < token.length(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(token[i]);
    // ASCII letters/digits, or any non-ASCII byte (part of a UTF-8 encoded
    // letter such as Polish diacritics) count as content.
    if (byte >= 0x80 || std::isalnum(byte) != 0) {
      hasAlphaNumeric = true;
      break;
    }
  }

  if (token.isEmpty() || !hasAlphaNumeric) {
    return false;
  }
  out = token;
  return true;
}

String stripBom(String text) {
  text.trim();
  if (text.length() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF &&
      static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF) {
    text.remove(0, 3);
    text.trim();
  }
  return text;
}

bool chapterTitleFromLine(const String &line, String &title) {
  String trimmed = normalizeDisplayText(stripBom(line));
  trimmed.trim();
  if (trimmed.isEmpty() || trimmed.length() > kMaxChapterTitleChars) {
    return false;
  }

  if (trimmed.startsWith("#")) {
    size_t prefixLength = 0;
    while (prefixLength < trimmed.length() && trimmed[prefixLength] == '#') {
      ++prefixLength;
    }
    title = trimmed.substring(prefixLength);
    title.trim();
    return !title.isEmpty();
  }

  String lowered = trimmed;
  lowered.toLowerCase();
  if (prefixHasBoundary(lowered, "chapter") || prefixHasBoundary(lowered, "part") ||
      prefixHasBoundary(lowered, "book")) {
    title = trimmed;
    return true;
  }

  return false;
}

void addChapterMarker(WordSink &sink, const String &title) {
  if (title.isEmpty()) {
    return;
  }
  sink.addChapter(title);
}

void addParagraphMarker(WordSink &sink) { sink.addParagraph(); }

String directiveValue(const String &line, const char *directive) {
  String value = line.substring(std::strlen(directive));
  value.trim();
  if (!value.isEmpty() && (value[0] == ':' || value[0] == '-' || value[0] == '.')) {
    value.remove(0, 1);
    value.trim();
  }
  return normalizeDisplayText(value);
}

bool appendLineWords(const String &line, WordSink &sink) {
  const String normalizedLine = normalizeDisplayText(line);
  String currentWord;

  for (size_t i = 0; i < normalizedLine.length(); ++i) {
    const char c = normalizedLine[i];
    if (isWordBoundary(c)) {
      if (!currentWord.isEmpty()) {
        String cleaned;
        if (cleanWordToken(currentWord, cleaned)) {
          if (!sink.addWord(cleaned)) return false;
          if (reachedBookWordLimit(sink.wordCount())) return false;
        }
        currentWord = "";
      }
      continue;
    }

    currentWord += c;
  }

  if (!currentWord.isEmpty() && !reachedBookWordLimit(sink.wordCount())) {
    String cleaned;
    if (cleanWordToken(currentWord, cleaned)) {
      if (!sink.addWord(cleaned)) return false;
    }
  }

  return !reachedBookWordLimit(sink.wordCount());
}

bool processBookLine(const String &line, WordSink &sink, bool &paragraphPending) {
  const String trimmed = stripBom(line);
  if (trimmed.isEmpty()) {
    paragraphPending = true;
    return true;
  }

  String chapterTitle;
  if (chapterTitleFromLine(line, chapterTitle)) {
    addChapterMarker(sink, chapterTitle);
    paragraphPending = true;
  }

  if (paragraphPending) {
    addParagraphMarker(sink);
    paragraphPending = false;
  }
  return appendLineWords(line, sink);
}

bool processRsvpLine(const String &line, WordSink &sink, bool &paragraphPending) {
  String trimmed = stripBom(line);
  if (trimmed.isEmpty()) {
    paragraphPending = true;
    return true;
  }

  if (trimmed.startsWith("@@")) {
    trimmed.remove(0, 1);
    if (paragraphPending) {
      addParagraphMarker(sink);
      paragraphPending = false;
    }
    return appendLineWords(trimmed, sink);
  }

  if (trimmed.startsWith("@")) {
    String lowered = trimmed;
    lowered.toLowerCase();
    if (prefixHasBoundary(lowered, "@para")) {
      paragraphPending = true;
      return true;
    }
    if (prefixHasBoundary(lowered, "@chapter")) {
      String title = directiveValue(trimmed, "@chapter");
      if (title.isEmpty()) {
        title = "Chapter";
      }
      addChapterMarker(sink, title);
      paragraphPending = true;
      return true;
    }
    if (prefixHasBoundary(lowered, "@title")) {
      sink.setTitle(directiveValue(trimmed, "@title"));
      return true;
    }
    if (prefixHasBoundary(lowered, "@author")) {
      sink.setAuthor(directiveValue(trimmed, "@author"));
      return true;
    }
    return true;
  }

  if (paragraphPending) {
    addParagraphMarker(sink);
    paragraphPending = false;
  }
  return appendLineWords(line, sink);
}

// In-memory sink: stores words into a vector that the caller wraps in an
// InMemoryBookSource at the end. Used for small `.txt` files where it's fine
// to materialize the whole book in DRAM. Chapter and paragraph markers are
// recorded directly on the supplied BookContent.
class InMemoryWordSink : public WordSink {
 public:
  InMemoryWordSink(BookContent &book, std::vector<String> &words)
      : book_(book), words_(words) {}

  bool addWord(const String &word) override {
    if (reachedBookWordLimit(words_.size())) return false;
    words_.push_back(word);
    return !reachedBookWordLimit(words_.size());
  }
  void addParagraph() override {
    const size_t at = words_.size();
    if (!book_.paragraphStarts.empty() && book_.paragraphStarts.back() == at) return;
    book_.paragraphStarts.push_back(at);
  }
  void addChapter(const String &title) override {
    ChapterMarker marker;
    marker.title = title;
    marker.wordIndex = words_.size();
    if (!book_.chapters.empty() && book_.chapters.back().wordIndex == marker.wordIndex) {
      book_.chapters.back() = marker;
      return;
    }
    book_.chapters.push_back(marker);
  }
  size_t wordCount() const override { return words_.size(); }
  void setTitle(const String &title) override { book_.title = title; }
  void setAuthor(const String &author) override { book_.author = author; }

 private:
  BookContent &book_;
  std::vector<String> &words_;
};

// Streaming sink: forwards every cleaned word to a BookIndex::Writer that
// streams bytes straight to the SD card. This keeps DRAM usage bounded even
// for very large books (tens of thousands of words).
class IdxBuilderSink : public WordSink {
 public:
  explicit IdxBuilderSink(BookIndex::Writer &writer) : writer_(writer) {}
  bool addWord(const String &word) override {
    if (!writer_.addWord(word)) return false;
    return !reachedBookWordLimit(writer_.wordCount());
  }
  void addParagraph() override { writer_.addParagraph(); }
  void addChapter(const String &title) override { writer_.addChapter(title); }
  size_t wordCount() const override { return writer_.wordCount(); }
  void setTitle(const String &title) override { writer_.setTitle(title); }
  void setAuthor(const String &author) override { writer_.setAuthor(author); }

 private:
  BookIndex::Writer &writer_;
};

String readRsvpDirectiveValue(const String &path, const char *directive) {
  if (!hasRsvpExtension(path)) {
    return "";
  }

  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return "";
  }

  String line;
  while (file.available()) {
    const char c = static_cast<char>(file.read());
    if (c == '\r') {
      continue;
    }

    if (c != '\n') {
      line += c;
      if (line.length() > kMaxChapterTitleChars + 16) {
        line = "";
        break;
      }
      continue;
    }

    String trimmed = stripBom(line);
    if (trimmed.isEmpty()) {
      line = "";
      continue;
    }

    String lowered = trimmed;
    lowered.toLowerCase();
    if (prefixHasBoundary(lowered, directive)) {
      file.close();
      return directiveValue(trimmed, directive);
    }

    if (!trimmed.startsWith("@")) {
      break;
    }
    line = "";
  }

  file.close();
  return "";
}

}  // namespace

void StorageManager::setStatusCallback(StatusCallback callback, void *context) {
  statusCallback_ = callback;
  statusContext_ = context;
}

void StorageManager::notifyStatus(const char *title, const char *line1, const char *line2,
                                  int progressPercent) {
  Serial.printf("[storage-status] %d%% %s | %s | %s\n", progressPercent,
                title == nullptr ? "" : title, line1 == nullptr ? "" : line1,
                line2 == nullptr ? "" : line2);
  if (statusCallback_ != nullptr) {
    statusCallback_(statusContext_, title, line1, line2, progressPercent);
  }
}

bool StorageManager::begin() {
  mounted_ = false;
  listedOnce_ = false;
  bookPaths_.clear();

  if (!SD_MMC.setPins(BoardConfig::PIN_SD_CLK, BoardConfig::PIN_SD_CMD, BoardConfig::PIN_SD_D0)) {
    Serial.println("[storage] SD_MMC pin setup failed");
    return false;
  }

  for (int frequencyKhz : kSdFrequenciesKhz) {
    notifyStatus("SD", "Mounting card", "", 5);
    Serial.printf("[storage] Trying SD_MMC mount at %d kHz\n", frequencyKhz);
    SD_MMC.end();
    mounted_ = SD_MMC.begin(kMountPoint, true, false, frequencyKhz, 5);
    if (mounted_) {
      const uint64_t sizeMb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
      Serial.printf("[storage] SD initialized (%llu MB) at %d kHz\n", sizeMb, frequencyKhz);
      notifyStatus("SD", "Scanning books", "EPUB converts on open", 10);
      refreshBookPaths();
      return true;
    }
  }

  Serial.println("[storage] SD init failed after retries");
  return false;
}

void StorageManager::end() {
  if (mounted_) {
    SD_MMC.end();
  }
  mounted_ = false;
  listedOnce_ = false;
  bookPaths_.clear();
}

void StorageManager::listBooks() {
  if (!mounted_ || listedOnce_) {
    return;
  }
  listedOnce_ = true;

  if (!booksDirectoryExists()) {
    Serial.println("[storage] /books directory not found");
    return;
  }

  refreshBookPaths();
  if (bookPaths_.empty()) {
    Serial.println("[storage] No readable .rsvp, .txt, or .epub books found under /books");
    return;
  }

  Serial.println("[storage] Listing /books (.rsvp/.txt/.epub pending conversion):");
  for (const String &path : bookPaths_) {
    File entry = SD_MMC.open(path);
    if (!entry || entry.isDirectory()) {
      if (entry) {
        entry.close();
      }
      continue;
    }

    Serial.printf("  %s (%lu bytes)\n", displayNameForPath(path).c_str(),
                  static_cast<unsigned long>(entry.size()));
    entry.close();
  }
}

void StorageManager::refreshBooks() {
  refreshBookPaths();
}

size_t StorageManager::bookCount() const { return bookPaths_.size(); }

String StorageManager::bookPath(size_t index) const {
  if (index >= bookPaths_.size()) {
    return "";
  }
  return bookPaths_[index];
}

String StorageManager::bookDisplayName(size_t index) const {
  const String path = bookPath(index);
  if (path.isEmpty()) {
    return "";
  }

  const String title = readRsvpDirectiveValue(path, "@title");
  if (!title.isEmpty()) {
    return title;
  }

  return normalizeDisplayText(displayNameWithoutExtension(path));
}

String StorageManager::bookAuthorName(size_t index) const {
  const String path = bookPath(index);
  if (path.isEmpty()) {
    return "";
  }

  if (hasEpubExtension(path)) {
    return epubLibraryLabel(path);
  }

  return readRsvpDirectiveValue(path, "@author");
}

bool StorageManager::ensureEpubConverted(const String &epubPath, String &rsvpPath) {
  rsvpPath = rsvpCachePathForEpub(epubPath);

  if (!RSVP_ON_DEVICE_EPUB_CONVERSION) {
    Serial.printf("[storage] EPUB conversion disabled at build time: %s\n", epubPath.c_str());
    notifyStatus("EPUB unsupported", displayNameForPath(epubPath).c_str(),
                 "Build flag is disabled", 100);
    return false;
  }

  if (!fileExistsAndHasBytes(epubPath)) {
    Serial.printf("[storage] EPUB source missing or empty: %s\n", epubPath.c_str());
    notifyStatus("Preparing book", displayNameForPath(epubPath).c_str(), "EPUB missing", 100);
    return false;
  }

  if (fileExistsAndHasBytes(rsvpPath) && EpubConverter::isCurrentCache(rsvpPath)) {
    Serial.printf("[storage] EPUB cache hit: %s -> %s\n", epubPath.c_str(), rsvpPath.c_str());
    return true;
  }

  if (fileExistsAndHasBytes(rsvpPath)) {
    Serial.printf("[storage] EPUB cache stale after converter update: %s\n", rsvpPath.c_str());
  }

  File epubFile = SD_MMC.open(epubPath);
  const size_t epubBytes = epubFile ? static_cast<size_t>(epubFile.size()) : 0;
  if (epubFile) {
    epubFile.close();
  }

  Serial.printf("[storage] Preparing EPUB conversion: source=%s output=%s size=%lu bytes\n",
                epubPath.c_str(), rsvpPath.c_str(), static_cast<unsigned long>(epubBytes));
  logHeapSnapshot("before EPUB conversion");
  notifyStatus("Preparing book", displayNameForPath(epubPath).c_str(), "Converting EPUB", 0);

  EpubConverter::Options options;
  options.maxWords = kMaxBookWords;
  options.progressCallback = handleEpubProgress;
  EpubProgressContext progressContext;
  progressContext.statusCallback = statusCallback_;
  progressContext.statusContext = statusContext_;
  progressContext.title = "Preparing book";
  progressContext.label = displayNameForPath(epubPath);
  options.progressContext = &progressContext;

  const uint32_t startedMs = millis();
  const bool converted = EpubConverter::convertIfNeeded(epubPath, rsvpPath, options);
  const uint32_t elapsedMs = millis() - startedMs;
  logHeapSnapshot("after EPUB conversion");

  if (!converted || !fileExistsAndHasBytes(rsvpPath)) {
    Serial.printf("[storage] EPUB conversion failed after %lu ms: %s\n",
                  static_cast<unsigned long>(elapsedMs), epubPath.c_str());
    notifyStatus("Preparing book", "EPUB conversion failed", "Check serial monitor", 100);
    return false;
  }

  Serial.printf("[storage] EPUB conversion ready after %lu ms: %s\n",
                static_cast<unsigned long>(elapsedMs), rsvpPath.c_str());
  notifyStatus("Preparing book", displayNameForPath(rsvpPath).c_str(), "Conversion complete",
               100);

  // Pre-build the streaming index immediately so the first read does not pay
  // the index-build cost. Failure here is non-fatal: the next load will retry.
  const String idxPath = BookIndex::idxPathForRsvp(rsvpPath);
  if (!BookIndex::isCurrentForRsvp(rsvpPath, idxPath, kIdxConverterTag)) {
    if (!buildIdxForRsvp(rsvpPath, idxPath)) {
      Serial.printf("[storage] Post-conversion idx build failed (will retry on load): %s\n",
                    rsvpPath.c_str());
    }
  }
  return true;
}

bool StorageManager::loadBookContent(size_t index, BookContent &book, String *loadedPath,
                                     size_t *loadedIndex) {
  book.clear();

  if (!mounted_) {
    Serial.println("[storage] SD not mounted, cannot load book");
    return false;
  }

  if (!booksDirectoryExists()) {
    Serial.println("[storage] /books directory not found");
    return false;
  }

  refreshBookPaths();
  if (bookPaths_.empty()) {
    Serial.println("[storage] No readable .rsvp, .txt, or .epub books found under /books");
    return false;
  }

  if (index >= bookPaths_.size()) {
    Serial.printf("[storage] Book index %u out of range\n", static_cast<unsigned int>(index));
    return false;
  }

  for (size_t offset = 0; offset < bookPaths_.size(); ++offset) {
    const size_t candidateIndex = (index + offset) % bookPaths_.size();
    String path = bookPaths_[candidateIndex];
    size_t parsedIndex = candidateIndex;

    if (hasEpubExtension(path)) {
      String rsvpPath;
      if (!ensureEpubConverted(path, rsvpPath)) {
        return false;
      }

      refreshBookPaths();
      const int convertedIndex = pathIndexIn(bookPaths_, rsvpPath);
      if (convertedIndex < 0) {
        Serial.printf("[storage] Converted RSVP not found in refreshed library: %s\n",
                      rsvpPath.c_str());
        return false;
      }

      path = rsvpPath;
      parsedIndex = static_cast<size_t>(convertedIndex);
    }

    bool loaded = false;
    if (hasRsvpExtension(path)) {
      loaded = loadRsvpAsStreaming(path, book);
    } else {
      loaded = loadTextIntoMemory(path, book);
    }

    if (loaded) {
      if (book.title.isEmpty()) {
        book.title = normalizeDisplayText(displayNameWithoutExtension(path));
      }
      const size_t loadedWordCount = book.source ? book.source->size() : 0;
      Serial.printf("[storage] Loaded %u words and %u chapters from %s\n",
                    static_cast<unsigned int>(loadedWordCount),
                    static_cast<unsigned int>(book.chapters.size()), path.c_str());
      if (loadedPath != nullptr) {
        *loadedPath = path;
      }
      if (loadedIndex != nullptr) {
        *loadedIndex = parsedIndex;
      }
      return true;
    }

    book.clear();
  }

  Serial.println("[storage] No readable book files found under /books");
  return false;
}

void StorageManager::refreshBookPaths() {
  if (!mounted_) {
    bookPaths_.clear();
    return;
  }

  notifyStatus("SD", "Reading library", "", 96);
  bookPaths_ = collectBookPaths();

  size_t rsvpCount = 0;
  size_t textCount = 0;
  size_t pendingEpubCount = 0;
  for (const String &path : bookPaths_) {
    if (hasRsvpExtension(path)) {
      ++rsvpCount;
    } else if (hasTextExtension(path)) {
      ++textCount;
    } else if (hasEpubExtension(path)) {
      ++pendingEpubCount;
    }
  }

  Serial.printf("[storage] Library scan: %u books (%u rsvp, %u txt, %u pending epub)\n",
                static_cast<unsigned int>(bookPaths_.size()),
                static_cast<unsigned int>(rsvpCount), static_cast<unsigned int>(textCount),
                static_cast<unsigned int>(pendingEpubCount));
}

namespace {

// Streams a `.txt` or `.rsvp` file through the supplied WordSink. Returns the
// number of words emitted (>0 means success).
size_t streamFileIntoSink(File &file, WordSink &sink, bool rsvpFormat) {
  String line;
  bool paragraphPending = true;
  size_t serviceCounter = 0;

  while (file.available()) {
    const char c = static_cast<char>(file.read());
    if ((++serviceCounter & 0x1FFF) == 0) {
      yield();
    }

    if (c == '\r') continue;
    if (c == '\n') {
      const bool keepReading = rsvpFormat ? processRsvpLine(line, sink, paragraphPending)
                                          : processBookLine(line, sink, paragraphPending);
      if (!keepReading) {
        if (hasBookWordLimit()) {
          Serial.printf("[storage] Reached %lu word limit, truncating book\n",
                        static_cast<unsigned long>(kMaxBookWords));
        }
        break;
      }
      line = "";
      continue;
    }
    line += c;
  }

  if (!line.isEmpty() && !reachedBookWordLimit(sink.wordCount())) {
    if (rsvpFormat) processRsvpLine(line, sink, paragraphPending);
    else processBookLine(line, sink, paragraphPending);
  }
  return sink.wordCount();
}

}  // namespace

bool StorageManager::buildIdxForRsvp(const String &rsvpPath, const String &idxPath) {
  File rsvp = SD_MMC.open(rsvpPath);
  if (!rsvp || rsvp.isDirectory()) {
    if (rsvp) rsvp.close();
    Serial.printf("[storage] Cannot open .rsvp for indexing: %s\n", rsvpPath.c_str());
    return false;
  }
  const uint32_t rsvpSize = static_cast<uint32_t>(rsvp.size());

  const String tmpPath = idxPath + ".tmp";
  BookIndex::Writer writer;
  if (!writer.open(tmpPath, kIdxConverterTag, rsvpSize)) {
    rsvp.close();
    return false;
  }

  notifyStatus("Indexing book", displayNameForPath(rsvpPath).c_str(),
               "Building word index", 30);
  Serial.printf("[storage] Building idx for %s (%lu bytes)\n", rsvpPath.c_str(),
                static_cast<unsigned long>(rsvpSize));
  logHeapSnapshot("before idx build");

  IdxBuilderSink sink(writer);
  const size_t produced = streamFileIntoSink(rsvp, sink, true /*rsvpFormat*/);
  rsvp.close();

  if (produced == 0) {
    Serial.printf("[storage] No words extracted while indexing %s\n", rsvpPath.c_str());
    writer.abort();
    return false;
  }

  // Synthesize a paragraph at index 0 if the source had none, so seek-to-start
  // works the same as before.
  // (Writer dedupes equal indices, so calling addParagraph at the start is safe.)
  // Note: we don't have a way to peek into the writer here, so always call.
  if (!writer.finalize(idxPath)) {
    Serial.printf("[storage] Failed to finalize idx: %s\n", idxPath.c_str());
    return false;
  }

  Serial.printf("[storage] Indexed %s -> %s (%u words)\n", rsvpPath.c_str(),
                idxPath.c_str(), static_cast<unsigned int>(produced));
  logHeapSnapshot("after idx build");
  return true;
}

bool StorageManager::loadRsvpAsStreaming(const String &rsvpPath, BookContent &book) {
  const String idxPath = BookIndex::idxPathForRsvp(rsvpPath);

  if (!BookIndex::isCurrentForRsvp(rsvpPath, idxPath, kIdxConverterTag)) {
    if (!buildIdxForRsvp(rsvpPath, idxPath)) {
      return false;
    }
  }

  auto streaming = std::make_shared<BookIndex::StreamingSource>();
  if (!streaming->openFromIdx(idxPath, book)) {
    return false;
  }

  if (streaming->size() == 0) {
    return false;
  }

  if (book.paragraphStarts.empty()) {
    book.paragraphStarts.push_back(0);
  }
  book.source = streaming;
  return true;
}

bool StorageManager::loadTextIntoMemory(const String &path, BookContent &book) {
  File entry = SD_MMC.open(path);
  if (!entry || entry.isDirectory()) {
    if (entry) entry.close();
    return false;
  }

  std::vector<String> words;
  InMemoryWordSink sink(book, words);
  const size_t produced = streamFileIntoSink(entry, sink, false /*rsvpFormat*/);
  entry.close();

  if (produced == 0) {
    return false;
  }
  if (book.paragraphStarts.empty()) {
    book.paragraphStarts.push_back(0);
  }
  book.source = std::make_shared<InMemoryBookSource>(std::move(words));
  return true;
}
