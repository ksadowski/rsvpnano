#include "storage/BookIndex.h"

#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr size_t kFooterBytes = 12;  // u32 metadataOffset, u32 magic, u32 version
constexpr size_t kMaxCachedWordBytes = 256;

bool readU16(File &f, uint16_t &out) {
  uint8_t buf[2];
  if (f.read(buf, 2) != 2) return false;
  out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
  return true;
}

bool readU32(File &f, uint32_t &out) {
  uint8_t buf[4];
  if (f.read(buf, 4) != 4) return false;
  out = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
        (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
  return true;
}

void writeU16(File &f, uint16_t v) {
  uint8_t buf[2] = {static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 8) & 0xFFu)};
  f.write(buf, 2);
}

void writeU32(File &f, uint32_t v) {
  uint8_t buf[4] = {static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 8) & 0xFFu),
                    static_cast<uint8_t>((v >> 16) & 0xFFu),
                    static_cast<uint8_t>((v >> 24) & 0xFFu)};
  f.write(buf, 4);
}

bool readBytesIntoString(File &f, size_t length, String &out) {
  out = "";
  if (length == 0) return true;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const int byte = f.read();
    if (byte < 0) return false;
    out += static_cast<char>(byte);
  }
  return true;
}

uint32_t fileSizeOf(const String &path) {
  File f = SD_MMC.open(path);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return 0;
  }
  const uint32_t size = static_cast<uint32_t>(f.size());
  f.close();
  return size;
}

}  // namespace

namespace BookIndex {

String idxPathForRsvp(const String &rsvpPath) { return rsvpPath + ".idx"; }

// ---------------- Reader ----------------

bool readMetadataBlock(File &f, LoadedHeader &header,
                       std::vector<ChapterMarker> &chapters,
                       std::vector<size_t> &paragraphs, uint32_t &metadataOffset,
                       uint32_t **outOffsets, uint16_t **outLengths) {
  const uint32_t totalSize = static_cast<uint32_t>(f.size());
  if (totalSize < kFooterBytes) return false;

  if (!f.seek(totalSize - kFooterBytes)) return false;

  uint32_t metaOffset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  if (!readU32(f, metaOffset) || !readU32(f, magic) || !readU32(f, version)) {
    return false;
  }
  if (magic != kMagic || version != kVersion) {
    return false;
  }
  if (metaOffset >= totalSize - kFooterBytes) {
    return false;
  }

  metadataOffset = metaOffset;
  if (!f.seek(metaOffset)) return false;

  uint16_t converterLen = 0;
  if (!readU16(f, converterLen)) return false;
  if (!readBytesIntoString(f, converterLen, header.converterTag)) return false;

  if (!readU32(f, header.rsvpSize)) return false;

  uint16_t titleLen = 0;
  uint16_t authorLen = 0;
  if (!readU16(f, titleLen)) return false;
  if (!readBytesIntoString(f, titleLen, header.title)) return false;
  if (!readU16(f, authorLen)) return false;
  if (!readBytesIntoString(f, authorLen, header.author)) return false;

  if (!readU32(f, header.wordCount)) return false;
  if (!readU32(f, header.chapterCount)) return false;
  if (!readU32(f, header.paragraphCount)) return false;

  chapters.clear();
  chapters.reserve(header.chapterCount);
  for (uint32_t i = 0; i < header.chapterCount; ++i) {
    ChapterMarker marker;
    uint32_t wordIndex = 0;
    uint16_t tl = 0;
    if (!readU32(f, wordIndex) || !readU16(f, tl)) return false;
    if (!readBytesIntoString(f, tl, marker.title)) return false;
    marker.wordIndex = wordIndex;
    chapters.push_back(marker);
  }

  paragraphs.clear();
  paragraphs.reserve(header.paragraphCount);
  for (uint32_t i = 0; i < header.paragraphCount; ++i) {
    uint32_t wordIndex = 0;
    if (!readU32(f, wordIndex)) return false;
    paragraphs.push_back(wordIndex);
  }

  if (outOffsets == nullptr || outLengths == nullptr) {
    return true;
  }

  // Allocate offset+length tables in PSRAM.
  const size_t offsetsBytes = static_cast<size_t>(header.wordCount) * sizeof(uint32_t);
  const size_t lengthsBytes = static_cast<size_t>(header.wordCount) * sizeof(uint16_t);

  *outOffsets = static_cast<uint32_t *>(heap_caps_malloc(offsetsBytes, MALLOC_CAP_SPIRAM));
  if (*outOffsets == nullptr) {
    *outOffsets =
        static_cast<uint32_t *>(heap_caps_malloc(offsetsBytes, MALLOC_CAP_8BIT));
  }
  *outLengths = static_cast<uint16_t *>(heap_caps_malloc(lengthsBytes, MALLOC_CAP_SPIRAM));
  if (*outLengths == nullptr) {
    *outLengths =
        static_cast<uint16_t *>(heap_caps_malloc(lengthsBytes, MALLOC_CAP_8BIT));
  }
  if (*outOffsets == nullptr || *outLengths == nullptr) {
    if (*outOffsets) {
      heap_caps_free(*outOffsets);
      *outOffsets = nullptr;
    }
    if (*outLengths) {
      heap_caps_free(*outLengths);
      *outLengths = nullptr;
    }
    return false;
  }

  // Read offsets.
  for (uint32_t i = 0; i < header.wordCount; ++i) {
    uint32_t v = 0;
    if (!readU32(f, v)) return false;
    (*outOffsets)[i] = v;
  }
  // Read lengths.
  for (uint32_t i = 0; i < header.wordCount; ++i) {
    uint16_t v = 0;
    if (!readU16(f, v)) return false;
    (*outLengths)[i] = v;
  }
  return true;
}

bool isCurrentForRsvp(const String &rsvpPath, const String &idxPath,
                      const String &converterTag) {
  File f = SD_MMC.open(idxPath);
  if (!f || f.isDirectory() || f.size() < kFooterBytes) {
    if (f) f.close();
    return false;
  }

  LoadedHeader header;
  std::vector<ChapterMarker> chapters;
  std::vector<size_t> paragraphs;
  uint32_t metaOffset = 0;
  const bool ok = readMetadataBlock(f, header, chapters, paragraphs, metaOffset, nullptr, nullptr);
  f.close();
  if (!ok) return false;

  const uint32_t actualRsvpSize = fileSizeOf(rsvpPath);
  return header.converterTag == converterTag && header.rsvpSize == actualRsvpSize;
}

// ---------------- StreamingSource ----------------

StreamingSource::~StreamingSource() { close(); }

void StreamingSource::close() {
  if (file_) {
    file_.close();
  }
  if (offsets_) {
    heap_caps_free(offsets_);
    offsets_ = nullptr;
  }
  if (lengths_) {
    heap_caps_free(lengths_);
    lengths_ = nullptr;
  }
  wordCount_ = 0;
  for (size_t i = 0; i < kCacheSize; ++i) {
    cache_[i].index = static_cast<size_t>(-1);
    cache_[i].word = "";
    cache_[i].lastUsed = 0;
  }
  cacheTick_ = 0;
}

bool StreamingSource::openFromIdx(const String &idxPath, BookContent &book) {
  close();

  File f = SD_MMC.open(idxPath);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    Serial.printf("[bookidx] Could not open idx: %s\n", idxPath.c_str());
    return false;
  }

  LoadedHeader header;
  std::vector<ChapterMarker> chapters;
  std::vector<size_t> paragraphs;
  uint32_t metaOffset = 0;
  uint32_t *offsets = nullptr;
  uint16_t *lengths = nullptr;
  if (!readMetadataBlock(f, header, chapters, paragraphs, metaOffset, &offsets, &lengths)) {
    f.close();
    if (offsets) heap_caps_free(offsets);
    if (lengths) heap_caps_free(lengths);
    Serial.printf("[bookidx] Failed to parse idx: %s\n", idxPath.c_str());
    return false;
  }

  path_ = idxPath;
  file_ = f;  // keep file open for word reads
  offsets_ = offsets;
  lengths_ = lengths;
  wordCount_ = header.wordCount;

  if (book.title.isEmpty()) book.title = header.title;
  if (book.author.isEmpty()) book.author = header.author;
  book.chapters = std::move(chapters);
  book.paragraphStarts = std::move(paragraphs);

  Serial.printf("[bookidx] Opened %s: words=%u chapters=%u paragraphs=%u meta=%u\n",
                idxPath.c_str(), static_cast<unsigned>(wordCount_),
                static_cast<unsigned>(book.chapters.size()),
                static_cast<unsigned>(book.paragraphStarts.size()),
                static_cast<unsigned>(metaOffset));
  return true;
}

String StreamingSource::at(size_t index) const {
  if (index >= wordCount_ || offsets_ == nullptr || lengths_ == nullptr || !file_) {
    return String();
  }

  // LRU lookup
  for (size_t i = 0; i < kCacheSize; ++i) {
    if (cache_[i].index == index) {
      cache_[i].lastUsed = ++cacheTick_;
      return cache_[i].word;
    }
  }

  const uint32_t offset = offsets_[index];
  const uint16_t length = lengths_[index];
  if (length == 0) {
    return String();
  }

  if (!file_.seek(offset)) {
    return String();
  }

  String out;
  out.reserve(length);
  // Read in a stack buffer to avoid Arduino String per-char concat overhead.
  uint8_t buf[128];
  size_t remaining = length;
  while (remaining > 0) {
    const size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    const int got = file_.read(buf, chunk);
    if (got <= 0) break;
    for (int i = 0; i < got; ++i) {
      out += static_cast<char>(buf[i]);
    }
    remaining -= static_cast<size_t>(got);
  }

  // Insert into LRU (evict oldest).
  if (length <= kMaxCachedWordBytes) {
    size_t victim = 0;
    uint32_t victimAge = cache_[0].lastUsed;
    for (size_t i = 1; i < kCacheSize; ++i) {
      if (cache_[i].index == static_cast<size_t>(-1)) {
        victim = i;
        break;
      }
      if (cache_[i].lastUsed < victimAge) {
        victimAge = cache_[i].lastUsed;
        victim = i;
      }
    }
    cache_[victim].index = index;
    cache_[victim].word = out;
    cache_[victim].lastUsed = ++cacheTick_;
  }

  return out;
}

// ---------------- Writer ----------------

struct ChapterRec {
  uint32_t wordIndex = 0;
  String title;
};

struct Writer::Impl {
  File file;
  String outPath;
  String converterTag;
  uint32_t rsvpFileSize = 0;
  uint32_t bytesWritten = 0;
  size_t wordCount = 0;
  String title;
  String author;
  std::vector<ChapterRec> chapters;
  std::vector<uint32_t> paragraphs;
  uint32_t *offsets = nullptr;
  uint16_t *lengths = nullptr;
  size_t capacity = 0;

  bool ensureCapacity(size_t need) {
    if (need <= capacity) return true;
    size_t newCap = capacity == 0 ? 4096 : capacity * 2;
    while (newCap < need) newCap *= 2;

    uint32_t *newOffsets = static_cast<uint32_t *>(
        heap_caps_realloc(offsets, newCap * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    if (newOffsets == nullptr) {
      newOffsets = static_cast<uint32_t *>(
          heap_caps_realloc(offsets, newCap * sizeof(uint32_t), MALLOC_CAP_8BIT));
    }
    if (newOffsets == nullptr) return false;
    offsets = newOffsets;

    uint16_t *newLengths = static_cast<uint16_t *>(
        heap_caps_realloc(lengths, newCap * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (newLengths == nullptr) {
      newLengths = static_cast<uint16_t *>(
          heap_caps_realloc(lengths, newCap * sizeof(uint16_t), MALLOC_CAP_8BIT));
    }
    if (newLengths == nullptr) return false;
    lengths = newLengths;

    capacity = newCap;
    return true;
  }

  void freeArrays() {
    if (offsets) {
      heap_caps_free(offsets);
      offsets = nullptr;
    }
    if (lengths) {
      heap_caps_free(lengths);
      lengths = nullptr;
    }
    capacity = 0;
  }
};

Writer::~Writer() {
  if (impl_) {
    abort();
    delete impl_;
    impl_ = nullptr;
  }
}

bool Writer::open(const String &tmpPath, const String &converterTag, uint32_t rsvpFileSize) {
  if (impl_) {
    abort();
    delete impl_;
    impl_ = nullptr;
  }
  impl_ = new (std::nothrow) Impl();
  if (impl_ == nullptr) return false;

  impl_->outPath = tmpPath;
  impl_->converterTag = converterTag;
  impl_->rsvpFileSize = rsvpFileSize;
  SD_MMC.remove(tmpPath);
  impl_->file = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!impl_->file) {
    Serial.printf("[bookidx] Could not open writer: %s\n", tmpPath.c_str());
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  impl_->bytesWritten = 0;
  impl_->wordCount = 0;
  return true;
}

void Writer::setTitle(const String &title) {
  if (impl_) impl_->title = title;
}
void Writer::setAuthor(const String &author) {
  if (impl_) impl_->author = author;
}

bool Writer::addWord(const String &word) {
  if (impl_ == nullptr || word.isEmpty()) return impl_ != nullptr;
  String trimmed = word;
  if (trimmed.length() > 0xFFFFu) {
    trimmed = trimmed.substring(0, 0xFFFFu);
  }
  if (!impl_->ensureCapacity(impl_->wordCount + 1)) return false;
  impl_->offsets[impl_->wordCount] = impl_->bytesWritten;
  impl_->lengths[impl_->wordCount] = static_cast<uint16_t>(trimmed.length());
  impl_->file.write(reinterpret_cast<const uint8_t *>(trimmed.c_str()), trimmed.length());
  impl_->bytesWritten += static_cast<uint32_t>(trimmed.length());
  ++impl_->wordCount;
  return true;
}

void Writer::addParagraph() {
  if (impl_ == nullptr) return;
  const uint32_t at = static_cast<uint32_t>(impl_->wordCount);
  if (!impl_->paragraphs.empty() && impl_->paragraphs.back() == at) return;
  impl_->paragraphs.push_back(at);
}

void Writer::addChapter(const String &title) {
  if (impl_ == nullptr || title.isEmpty()) return;
  ChapterRec rec;
  rec.title = title;
  rec.wordIndex = static_cast<uint32_t>(impl_->wordCount);
  if (!impl_->chapters.empty() && impl_->chapters.back().wordIndex == rec.wordIndex) {
    impl_->chapters.back() = rec;
    return;
  }
  impl_->chapters.push_back(rec);
}

size_t Writer::wordCount() const { return impl_ ? impl_->wordCount : 0; }

bool Writer::finalize(const String &finalPath) {
  if (impl_ == nullptr || !impl_->file) return false;
  const uint32_t metadataOffset = impl_->bytesWritten;
  File &f = impl_->file;

  writeU16(f, static_cast<uint16_t>(impl_->converterTag.length()));
  f.write(reinterpret_cast<const uint8_t *>(impl_->converterTag.c_str()),
          impl_->converterTag.length());

  writeU32(f, impl_->rsvpFileSize);

  writeU16(f, static_cast<uint16_t>(impl_->title.length()));
  f.write(reinterpret_cast<const uint8_t *>(impl_->title.c_str()), impl_->title.length());

  writeU16(f, static_cast<uint16_t>(impl_->author.length()));
  f.write(reinterpret_cast<const uint8_t *>(impl_->author.c_str()), impl_->author.length());

  writeU32(f, static_cast<uint32_t>(impl_->wordCount));
  writeU32(f, static_cast<uint32_t>(impl_->chapters.size()));
  writeU32(f, static_cast<uint32_t>(impl_->paragraphs.size()));

  for (const ChapterRec &c : impl_->chapters) {
    writeU32(f, c.wordIndex);
    writeU16(f, static_cast<uint16_t>(c.title.length()));
    f.write(reinterpret_cast<const uint8_t *>(c.title.c_str()), c.title.length());
  }
  for (uint32_t p : impl_->paragraphs) {
    writeU32(f, p);
  }

  // Offsets and lengths tables.
  uint8_t buf[256];
  size_t bufFill = 0;
  for (size_t i = 0; i < impl_->wordCount; ++i) {
    const uint32_t v = impl_->offsets[i];
    buf[bufFill++] = static_cast<uint8_t>(v & 0xFFu);
    buf[bufFill++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[bufFill++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[bufFill++] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    if (bufFill + 4 > sizeof(buf)) {
      f.write(buf, bufFill);
      bufFill = 0;
    }
  }
  if (bufFill > 0) {
    f.write(buf, bufFill);
    bufFill = 0;
  }

  for (size_t i = 0; i < impl_->wordCount; ++i) {
    const uint16_t v = impl_->lengths[i];
    buf[bufFill++] = static_cast<uint8_t>(v & 0xFFu);
    buf[bufFill++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    if (bufFill + 2 > sizeof(buf)) {
      f.write(buf, bufFill);
      bufFill = 0;
    }
  }
  if (bufFill > 0) {
    f.write(buf, bufFill);
  }

  writeU32(f, metadataOffset);
  writeU32(f, kMagic);
  writeU32(f, kVersion);

  f.flush();
  f.close();

  SD_MMC.remove(finalPath);
  if (!SD_MMC.rename(impl_->outPath, finalPath)) {
    Serial.printf("[bookidx] Could not rename %s -> %s\n", impl_->outPath.c_str(),
                  finalPath.c_str());
    SD_MMC.remove(impl_->outPath);
    impl_->freeArrays();
    delete impl_;
    impl_ = nullptr;
    return false;
  }

  impl_->freeArrays();
  delete impl_;
  impl_ = nullptr;
  return true;
}

void Writer::abort() {
  if (impl_ == nullptr) return;
  if (impl_->file) {
    impl_->file.close();
  }
  if (!impl_->outPath.isEmpty()) {
    SD_MMC.remove(impl_->outPath);
  }
  impl_->freeArrays();
  impl_->chapters.clear();
  impl_->paragraphs.clear();
  impl_->title = "";
  impl_->author = "";
  impl_->bytesWritten = 0;
  impl_->wordCount = 0;
}

}  // namespace BookIndex
