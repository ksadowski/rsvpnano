#include "storage/StorageManager.h"

#include <SD_MMC.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <driver/sdmmc_types.h>
#include <utility>

#include "board/BoardConfig.h"

namespace {

constexpr const char *kMountPoint = "/sdcard";
constexpr const char *kBooksPath = "/books";
constexpr size_t kMaxBookWords = 4096;
constexpr size_t kMaxChapterTitleChars = 64;
constexpr int kSdFrequenciesKhz[] = {
    SDMMC_FREQ_DEFAULT,
    10000,
    SDMMC_FREQ_PROBING,
};

bool isWordBoundary(char c) { return c <= ' '; }

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

bool hasSupportedBookExtension(const String &path) {
  return hasTextExtension(path) || hasRsvpExtension(path);
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
  }
  return name;
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
      if (hasSupportedBookExtension(path)) {
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

void pushCleanWord(String token, std::vector<String> &words) {
  token.trim();

  if (token.length() >= 3 && static_cast<uint8_t>(token[0]) == 0xEF &&
      static_cast<uint8_t>(token[1]) == 0xBB && static_cast<uint8_t>(token[2]) == 0xBF) {
    token.remove(0, 3);
  }

  while (!token.isEmpty() && isTrimmableEdgeChar(token[0])) {
    token.remove(0, 1);
  }

  while (!token.isEmpty() && isTrimmableEdgeChar(token[token.length() - 1])) {
    token.remove(token.length() - 1, 1);
  }

  bool hasAlphaNumeric = false;
  for (size_t i = 0; i < token.length(); ++i) {
    if (std::isalnum(static_cast<unsigned char>(token[i])) != 0) {
      hasAlphaNumeric = true;
      break;
    }
  }

  if (!token.isEmpty() && hasAlphaNumeric) {
    words.push_back(token);
  }
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
  String trimmed = stripBom(line);
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

void addChapterMarker(BookContent &book, const String &title) {
  if (title.isEmpty()) {
    return;
  }

  ChapterMarker marker;
  marker.title = title;
  marker.wordIndex = book.words.size();

  if (!book.chapters.empty() && book.chapters.back().wordIndex == marker.wordIndex) {
    book.chapters.back() = marker;
    return;
  }

  book.chapters.push_back(marker);
}

String directiveValue(const String &line, const char *directive) {
  String value = line.substring(std::strlen(directive));
  value.trim();
  if (!value.isEmpty() && (value[0] == ':' || value[0] == '-' || value[0] == '.')) {
    value.remove(0, 1);
    value.trim();
  }
  return value;
}

bool appendLineWords(const String &line, std::vector<String> &words) {
  String currentWord;

  for (size_t i = 0; i < line.length(); ++i) {
    const char c = line[i];
    if (isWordBoundary(c)) {
      if (!currentWord.isEmpty()) {
        pushCleanWord(currentWord, words);
        currentWord = "";
        if (words.size() >= kMaxBookWords) {
          return false;
        }
      }
      continue;
    }

    currentWord += c;
  }

  if (!currentWord.isEmpty() && words.size() < kMaxBookWords) {
    pushCleanWord(currentWord, words);
  }

  return words.size() < kMaxBookWords;
}

bool processBookLine(const String &line, BookContent &book) {
  String chapterTitle;
  if (chapterTitleFromLine(line, chapterTitle)) {
    addChapterMarker(book, chapterTitle);
  }

  return appendLineWords(line, book.words);
}

bool processRsvpLine(const String &line, BookContent &book) {
  String trimmed = stripBom(line);
  if (trimmed.isEmpty()) {
    return true;
  }

  if (trimmed.startsWith("@@")) {
    trimmed.remove(0, 1);
    return appendLineWords(trimmed, book.words);
  }

  if (trimmed.startsWith("@")) {
    String lowered = trimmed;
    lowered.toLowerCase();
    if (prefixHasBoundary(lowered, "@chapter")) {
      String title = directiveValue(trimmed, "@chapter");
      if (title.isEmpty()) {
        title = "Chapter";
      }
      addChapterMarker(book, title);
      return true;
    }
    if (prefixHasBoundary(lowered, "@title")) {
      book.title = directiveValue(trimmed, "@title");
      return true;
    }
    return true;
  }

  return appendLineWords(line, book.words);
}

String readRsvpTitle(const String &path) {
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
    if (prefixHasBoundary(lowered, "@title")) {
      file.close();
      return directiveValue(trimmed, "@title");
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

bool StorageManager::begin() {
  mounted_ = false;
  listedOnce_ = false;
  bookPaths_.clear();

  if (!SD_MMC.setPins(BoardConfig::PIN_SD_CLK, BoardConfig::PIN_SD_CMD, BoardConfig::PIN_SD_D0)) {
    Serial.println("[storage] SD_MMC pin setup failed");
    return false;
  }

  for (int frequencyKhz : kSdFrequenciesKhz) {
    Serial.printf("[storage] Trying SD_MMC mount at %d kHz\n", frequencyKhz);
    SD_MMC.end();
    mounted_ = SD_MMC.begin(kMountPoint, true, false, frequencyKhz, 5);
    if (mounted_) {
      const uint64_t sizeMb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
      Serial.printf("[storage] SD initialized (%llu MB) at %d kHz\n", sizeMb, frequencyKhz);
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
    Serial.println("[storage] No .rsvp or .txt books found under /books");
    return;
  }

  Serial.println("[storage] Listing /books (.rsvp/.txt):");
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

void StorageManager::refreshBooks() { refreshBookPaths(); }

bool StorageManager::loadFirstBookWords(std::vector<String> &words, String *loadedPath) {
  return loadBookWords(0, words, loadedPath);
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

  const String title = readRsvpTitle(path);
  if (!title.isEmpty()) {
    return title;
  }

  return displayNameWithoutExtension(path);
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
    Serial.println("[storage] No .rsvp or .txt books found under /books");
    return false;
  }

  if (index >= bookPaths_.size()) {
    Serial.printf("[storage] Book index %u out of range\n", static_cast<unsigned int>(index));
    return false;
  }

  for (size_t offset = 0; offset < bookPaths_.size(); ++offset) {
    const size_t candidateIndex = (index + offset) % bookPaths_.size();
    const String &path = bookPaths_[candidateIndex];
    File entry = SD_MMC.open(path);
    if (!entry || entry.isDirectory()) {
      if (entry) {
        entry.close();
      }
      continue;
    }

    if (parseFile(entry, book, hasRsvpExtension(path))) {
      Serial.printf("[storage] Loaded %u words and %u chapters from %s\n",
                    static_cast<unsigned int>(book.words.size()),
                    static_cast<unsigned int>(book.chapters.size()), path.c_str());
      if (loadedPath != nullptr) {
        *loadedPath = path;
      }
      if (loadedIndex != nullptr) {
        *loadedIndex = candidateIndex;
      }
      entry.close();
      return true;
    }

    book.clear();
    entry.close();
  }

  Serial.println("[storage] No readable book files found under /books");
  return false;
}

bool StorageManager::loadBookWords(size_t index, std::vector<String> &words, String *loadedPath,
                                   size_t *loadedIndex) {
  BookContent book;
  if (!loadBookContent(index, book, loadedPath, loadedIndex)) {
    words.clear();
    return false;
  }

  words = std::move(book.words);
  return true;
}

void StorageManager::refreshBookPaths() {
  if (!mounted_) {
    bookPaths_.clear();
    return;
  }

  bookPaths_ = collectBookPaths();
}

bool StorageManager::parseFile(File &file, BookContent &book, bool rsvpFormat) {
  book.clear();
  String line;

  while (file.available()) {
    const char c = static_cast<char>(file.read());

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      const bool keepReading =
          rsvpFormat ? processRsvpLine(line, book) : processBookLine(line, book);
      if (!keepReading) {
        Serial.printf("[storage] Reached %u word limit, truncating book\n",
                      static_cast<unsigned int>(kMaxBookWords));
        break;
      }
      line = "";
      continue;
    }

    line += c;
  }

  if (!line.isEmpty() && book.words.size() < kMaxBookWords) {
    if (rsvpFormat) {
      processRsvpLine(line, book);
    } else {
      processBookLine(line, book);
    }
  }

  return !book.words.empty();
}
