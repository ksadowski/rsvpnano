#include "storage/BookIndex.h"

#ifdef BOARD_LILYGO_TDISPLAY_S3_PRO
#include <SD.h>
#include <SPI.h>
#define STORAGE_FS SD
#else
#include <SD_MMC.h>
#define STORAGE_FS SD_MMC
#endif
#include <esp_heap_caps.h>

namespace BookIndex {

namespace {

static constexpr size_t kInitialCapacity = 2048;
static constexpr size_t kMaxChapterTitleBytes = 64;

bool writeU8(File &f, uint8_t v) { return f.write(&v, 1) == 1; }
bool writeU16(File &f, uint16_t v) {
  return f.write(reinterpret_cast<const uint8_t *>(&v), 2) == 2;
}
bool writeU32(File &f, uint32_t v) {
  return f.write(reinterpret_cast<const uint8_t *>(&v), 4) == 4;
}
bool writeStr8(File &f, const String &s) {
  const uint8_t len = static_cast<uint8_t>(s.length() > 255 ? 255 : s.length());
  if (!writeU8(f, len)) return false;
  if (len == 0) return true;
  return f.write(reinterpret_cast<const uint8_t *>(s.c_str()), len) == len;
}

bool readU8(File &f, uint8_t &v) { return f.read(&v, 1) == 1; }
bool readU16(File &f, uint16_t &v) {
  return f.read(reinterpret_cast<uint8_t *>(&v), 2) == 2;
}
bool readU32(File &f, uint32_t &v) {
  return f.read(reinterpret_cast<uint8_t *>(&v), 4) == 4;
}
bool readStr8(File &f, String &s) {
  uint8_t len;
  if (!readU8(f, len)) return false;
  if (len == 0) {
    s = "";
    return true;
  }
  char buf[256];
  if (static_cast<size_t>(f.read(reinterpret_cast<uint8_t *>(buf), len)) != len) return false;
  buf[len] = '\0';
  s = String(buf);
  return true;
}

}  // namespace

String idxPathForRsvp(const String &rsvpPath) { return rsvpPath + ".idx"; }

bool isCurrentForRsvp(const String &rsvpPath, const String &idxPath) {
  File rsvp = STORAGE_FS.open(rsvpPath);
  if (!rsvp || rsvp.isDirectory()) {
    if (rsvp) rsvp.close();
    return false;
  }
  const uint32_t rsvpSize = static_cast<uint32_t>(rsvp.size());
  rsvp.close();

  File idx = STORAGE_FS.open(idxPath);
  if (!idx || idx.isDirectory() || idx.size() < 12) {
    if (idx) idx.close();
    return false;
  }

  idx.seek(static_cast<uint32_t>(idx.size()) - 12);
  uint32_t metaOffset, magic, version;
  if (!readU32(idx, metaOffset) || !readU32(idx, magic) || !readU32(idx, version)) {
    idx.close();
    return false;
  }
  if (magic != kMagic || version != kVersion) {
    idx.close();
    return false;
  }

  idx.seek(metaOffset);
  uint32_t wordCount, chapterCount, paragraphCount, storedRsvpSize;
  if (!readU32(idx, wordCount) || !readU32(idx, chapterCount) ||
      !readU32(idx, paragraphCount) || !readU32(idx, storedRsvpSize)) {
    idx.close();
    return false;
  }
  idx.close();
  return storedRsvpSize == rsvpSize;
}

// ---- Writer ----

Writer::~Writer() { abort(); }

bool Writer::open(const String &tmpPath, uint32_t rsvpFileSize) {
  tmpPath_ = tmpPath;
  rsvpFileSize_ = rsvpFileSize;
  wordDataSize_ = 0;
  wordCount_ = 0;
  offsetsCapacity_ = 0;
  offsets_ = nullptr;
  lengths_ = nullptr;
  title_ = "";
  author_ = "";
  paragraphStarts_.clear();
  chapters_.clear();

  if (!growArrays()) {
    Serial.println("[bookidx] Failed to allocate PSRAM for index table");
    return false;
  }

  STORAGE_FS.remove(tmpPath);
  file_ = STORAGE_FS.open(tmpPath, FILE_WRITE);
  if (!file_) {
    Serial.printf("[bookidx] Failed to open tmp: %s\n", tmpPath.c_str());
    heap_caps_free(offsets_);
    heap_caps_free(lengths_);
    offsets_ = nullptr;
    lengths_ = nullptr;
    offsetsCapacity_ = 0;
    return false;
  }
  return true;
}

void Writer::setTitle(const String &title) { title_ = title; }
void Writer::setAuthor(const String &author) { author_ = author; }

bool Writer::addWord(const String &word) {
  if (word.isEmpty()) return true;
  if (wordCount_ >= offsetsCapacity_ && !growArrays()) {
    Serial.println("[bookidx] PSRAM grow failed");
    return false;
  }
  offsets_[wordCount_] = wordDataSize_;
  const uint16_t len =
      static_cast<uint16_t>(word.length() > 65535u ? 65535u : word.length());
  lengths_[wordCount_] = len;
  if (static_cast<size_t>(
          file_.write(reinterpret_cast<const uint8_t *>(word.c_str()), len)) != len) {
    Serial.println("[bookidx] SD write error");
    return false;
  }
  wordDataSize_ += len;
  ++wordCount_;
  return true;
}

void Writer::addParagraph() { paragraphStarts_.push_back(wordCount_); }

void Writer::addChapter(const String &title) {
  chapters_.push_back({wordCount_, title});
}

bool Writer::finalize(const String &finalPath) {
  if (!file_) return false;

  const uint32_t metaOffset = wordDataSize_;

  bool ok =
      writeU32(file_, static_cast<uint32_t>(wordCount_)) &&
      writeU32(file_, static_cast<uint32_t>(chapters_.size())) &&
      writeU32(file_, static_cast<uint32_t>(paragraphStarts_.size())) &&
      writeU32(file_, rsvpFileSize_) &&
      writeStr8(file_, title_) &&
      writeStr8(file_, author_);

  for (size_t i = 0; i < wordCount_ && ok; ++i) ok = writeU32(file_, offsets_[i]);
  for (size_t i = 0; i < wordCount_ && ok; ++i) ok = writeU16(file_, lengths_[i]);

  for (const auto &ch : chapters_) {
    if (!ok) break;
    String t = ch.second;
    if (t.length() > kMaxChapterTitleBytes) t = t.substring(0, kMaxChapterTitleBytes);
    ok = writeU32(file_, static_cast<uint32_t>(ch.first)) && writeStr8(file_, t);
  }

  for (size_t p : paragraphStarts_) {
    if (!ok) break;
    ok = writeU32(file_, static_cast<uint32_t>(p));
  }

  ok = ok && writeU32(file_, metaOffset) && writeU32(file_, kMagic) &&
       writeU32(file_, kVersion);

  file_.close();
  if (offsets_) { heap_caps_free(offsets_); offsets_ = nullptr; }
  if (lengths_) { heap_caps_free(lengths_); lengths_ = nullptr; }
  offsetsCapacity_ = 0;

  if (!ok) {
    STORAGE_FS.remove(tmpPath_);
    tmpPath_ = "";
    return false;
  }

  STORAGE_FS.remove(finalPath);
  if (!STORAGE_FS.rename(tmpPath_.c_str(), finalPath.c_str())) {
    Serial.printf("[bookidx] rename failed: %s -> %s\n", tmpPath_.c_str(), finalPath.c_str());
    STORAGE_FS.remove(tmpPath_);
    tmpPath_ = "";
    return false;
  }
  tmpPath_ = "";
  return true;
}

void Writer::abort() {
  if (file_) file_.close();
  if (!tmpPath_.isEmpty()) {
    STORAGE_FS.remove(tmpPath_);
    tmpPath_ = "";
  }
  if (offsets_) { heap_caps_free(offsets_); offsets_ = nullptr; }
  if (lengths_) { heap_caps_free(lengths_); lengths_ = nullptr; }
  offsetsCapacity_ = 0;
  wordCount_ = 0;
}

bool Writer::growArrays() {
  const size_t newCap = offsetsCapacity_ == 0 ? kInitialCapacity : offsetsCapacity_ * 2;
  void *newOffsets = heap_caps_realloc(offsets_, newCap * sizeof(uint32_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!newOffsets) return false;
  offsets_ = static_cast<uint32_t *>(newOffsets);
  void *newLengths = heap_caps_realloc(lengths_, newCap * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!newLengths) return false;
  lengths_ = static_cast<uint16_t *>(newLengths);
  offsetsCapacity_ = newCap;
  return true;
}

// ---- StreamingSource ----

StreamingSource::~StreamingSource() { close(); }

bool StreamingSource::openFromIdx(const String &idxPath, BookContent &book) {
  file_ = STORAGE_FS.open(idxPath);
  if (!file_ || file_.isDirectory()) {
    if (file_) file_.close();
    return false;
  }
  const uint32_t fileSize = static_cast<uint32_t>(file_.size());
  if (fileSize < 12) {
    file_.close();
    return false;
  }

  file_.seek(fileSize - 12);
  uint32_t metaOffset, magic, version;
  if (!readU32(file_, metaOffset) || !readU32(file_, magic) || !readU32(file_, version) ||
      magic != kMagic || version != kVersion) {
    file_.close();
    return false;
  }

  file_.seek(metaOffset);
  uint32_t wordCount, chapterCount, paragraphCount, rsvpSize;
  if (!readU32(file_, wordCount) || !readU32(file_, chapterCount) ||
      !readU32(file_, paragraphCount) || !readU32(file_, rsvpSize)) {
    file_.close();
    return false;
  }

  String title, author;
  if (!readStr8(file_, title) || !readStr8(file_, author)) {
    file_.close();
    return false;
  }
  book.title = title;
  book.author = author;

  wordCount_ = static_cast<size_t>(wordCount);
  offsets_ = static_cast<uint32_t *>(
      heap_caps_malloc(wordCount_ * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  lengths_ = static_cast<uint16_t *>(
      heap_caps_malloc(wordCount_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!offsets_ || !lengths_) {
    Serial.println("[bookidx] PSRAM alloc failed for streaming source");
    close();
    return false;
  }

  for (size_t i = 0; i < wordCount_; ++i) {
    if (!readU32(file_, offsets_[i])) { close(); return false; }
  }
  for (size_t i = 0; i < wordCount_; ++i) {
    if (!readU16(file_, lengths_[i])) { close(); return false; }
  }

  book.chapters.clear();
  for (uint32_t i = 0; i < chapterCount; ++i) {
    uint32_t wordIndex;
    String t;
    if (!readU32(file_, wordIndex) || !readStr8(file_, t)) { close(); return false; }
    ChapterMarker marker;
    marker.wordIndex = static_cast<size_t>(wordIndex);
    marker.title = t;
    book.chapters.push_back(marker);
  }

  book.paragraphStarts.clear();
  for (uint32_t i = 0; i < paragraphCount; ++i) {
    uint32_t p;
    if (!readU32(file_, p)) { close(); return false; }
    book.paragraphStarts.push_back(static_cast<size_t>(p));
  }

  for (auto &e : cache_) {
    e.index = static_cast<size_t>(-1);
    e.word = "";
    e.lastUsed = 0;
  }
  cacheTick_ = 0;

  Serial.printf("[bookidx] Streaming source ready: %u words, %u chapters\n",
                static_cast<unsigned int>(wordCount_),
                static_cast<unsigned int>(chapterCount));
  return true;
}

void StreamingSource::close() {
  if (file_) file_.close();
  if (offsets_) { heap_caps_free(offsets_); offsets_ = nullptr; }
  if (lengths_) { heap_caps_free(lengths_); lengths_ = nullptr; }
  wordCount_ = 0;
}

String StreamingSource::at(size_t index) const {
  if (index >= wordCount_ || !file_) return String();

  ++cacheTick_;
  for (auto &e : cache_) {
    if (e.index == index) {
      e.lastUsed = cacheTick_;
      return e.word;
    }
  }

  const uint16_t len = lengths_[index];
  if (len == 0) return String();

  file_.seek(offsets_[index]);
  uint8_t buf[512];
  const uint16_t readLen = len > 511 ? 511 : len;
  if (static_cast<size_t>(file_.read(buf, readLen)) != readLen) return String();
  buf[readLen] = '\0';
  const String result(reinterpret_cast<const char *>(buf));

  CacheEntry *lru = &cache_[0];
  for (size_t i = 1; i < kCacheSize; ++i) {
    if (cache_[i].lastUsed < lru->lastUsed) lru = &cache_[i];
  }
  lru->index = index;
  lru->word = result;
  lru->lastUsed = cacheTick_;

  return result;
}

}  // namespace BookIndex
