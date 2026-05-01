#pragma once

#include <Arduino.h>
#include <FS.h>
#include <cstdint>
#include <utility>
#include <vector>

#include "reader/BookContent.h"
#include "reader/BookSource.h"

namespace BookIndex {

constexpr uint32_t kMagic = 0x58444952u;
constexpr uint32_t kVersion = 1u;

String idxPathForRsvp(const String &rsvpPath);
bool isCurrentForRsvp(const String &rsvpPath, const String &idxPath);

class Writer {
 public:
  Writer() = default;
  ~Writer();

  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;

  bool open(const String &tmpPath, uint32_t rsvpFileSize);
  void setTitle(const String &title);
  void setAuthor(const String &author);
  bool addWord(const String &word);
  void addParagraph();
  void addChapter(const String &title);
  bool finalize(const String &finalPath);
  void abort();
  size_t wordCount() const { return wordCount_; }

 private:
  bool growArrays();

  File file_;
  String tmpPath_;
  uint32_t rsvpFileSize_ = 0;
  uint32_t wordDataSize_ = 0;
  size_t wordCount_ = 0;
  size_t offsetsCapacity_ = 0;
  uint32_t *offsets_ = nullptr;
  uint16_t *lengths_ = nullptr;
  String title_;
  String author_;
  std::vector<size_t> paragraphStarts_;
  std::vector<std::pair<size_t, String>> chapters_;
};

class StreamingSource : public BookSource {
 public:
  StreamingSource() = default;
  ~StreamingSource() override;

  StreamingSource(const StreamingSource &) = delete;
  StreamingSource &operator=(const StreamingSource &) = delete;

  bool openFromIdx(const String &idxPath, BookContent &book);
  void close();

  size_t size() const override { return wordCount_; }
  String at(size_t index) const override;

 private:
  mutable File file_;
  uint32_t *offsets_ = nullptr;
  uint16_t *lengths_ = nullptr;
  size_t wordCount_ = 0;

  static constexpr size_t kCacheSize = 64;
  struct CacheEntry {
    size_t index = static_cast<size_t>(-1);
    String word;
    uint32_t lastUsed = 0;
  };
  mutable CacheEntry cache_[kCacheSize];
  mutable uint32_t cacheTick_ = 0;
};

}  // namespace BookIndex
