#pragma once

#include <Arduino.h>
#include <FS.h>
#include <cstdint>
#include <vector>

#include "reader/BookContent.h"

// Sidecar index format for streaming `.rsvp` book reading.
//
// Goal: avoid loading every word of a large book into DRAM. Instead, for each
// `<book>.rsvp` we write a sidecar `<book>.rsvp.idx` that contains the cleaned
// word bytes (concatenated) plus a small metadata trailer with chapter /
// paragraph markers and a per-word offset+length table. At runtime we keep the
// offset table in PSRAM and read individual words from the file on demand.
//
// File layout:
//   [0 .. wordDataEnd)        Concatenated cleaned UTF-8 word bytes.
//   [wordDataEnd .. metaEnd)  Metadata block (see BookIndexMetadata writer).
//   [metaEnd-12 .. metaEnd)   Footer: u32 metadataOffset, u32 magic, u32 ver.
//
// The metadata block stores: converter tag, source file size, title/author,
// chapter table, paragraph table, word offset table, word length table.

namespace BookIndex {

constexpr uint32_t kMagic = 0x58444952u;  // "RIDX" little-endian
constexpr uint32_t kVersion = 1u;

struct LoadedHeader {
  String converterTag;
  uint32_t rsvpSize = 0;
  uint32_t wordCount = 0;
  uint32_t chapterCount = 0;
  uint32_t paragraphCount = 0;
  String title;
  String author;
};

// Returns true iff `idxPath` exists, parses successfully, and matches the
// `.rsvp` file size and converter tag found at `rsvpPath`.
bool isCurrentForRsvp(const String &rsvpPath, const String &idxPath,
                      const String &converterTag);

// Default sidecar path for a given .rsvp file ("<rsvp>.idx").
String idxPathForRsvp(const String &rsvpPath);

// Streaming writer that builds the sidecar `.idx` file. The caller is expected
// to drive it: open() → setTitle/setAuthor → repeated addWord/addParagraph/
// addChapter → finalize(). On failure use abort(). The writer streams word
// bytes straight to disk and only keeps offset+length tables in PSRAM, so it
// works for arbitrarily large books on the device.
class Writer {
 public:
  Writer() = default;
  ~Writer();

  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;

  // Opens `tmpPath` for writing. The eventual final path is supplied to
  // finalize(); we write to tmp first and rename on success.
  bool open(const String &tmpPath, const String &converterTag, uint32_t rsvpFileSize);

  void setTitle(const String &title);
  void setAuthor(const String &author);

  // Appends a cleaned UTF-8 word. Empty words are ignored. Returns false if a
  // PSRAM/SD allocation fails.
  bool addWord(const String &word);

  // Records a paragraph break at the current word index.
  void addParagraph();

  // Records a chapter break at the current word index with the given title.
  void addChapter(const String &title);

  size_t wordCount() const;

  // Writes metadata + footer, closes file, atomically renames tmp → finalPath.
  bool finalize(const String &finalPath);

  // Aborts, closes, and removes tmp.
  void abort();

 private:
  struct Impl;
  Impl *impl_ = nullptr;
};

// Streaming source backed by a `<book>.rsvp.idx` file. Holds the file open and
// keeps the offset table in PSRAM. Word fetches are O(1) seeks plus a tiny
// in-RAM LRU cache so context views don't constantly re-seek.
class StreamingSource : public BookSource {
 public:
  StreamingSource() = default;
  ~StreamingSource() override;

  StreamingSource(const StreamingSource &) = delete;
  StreamingSource &operator=(const StreamingSource &) = delete;

  // Opens the index file and reads the metadata block plus offset tables. On
  // success the returned source owns the file handle and the offset table in
  // PSRAM. Chapter and paragraph metadata are written into `book` for
  // bookkeeping (the source itself does not need them).
  bool openFromIdx(const String &idxPath, BookContent &book);

  void close();

  size_t size() const override { return wordCount_; }
  String at(size_t index) const override;

 private:
  String path_;
  mutable File file_;
  uint32_t *offsets_ = nullptr;  // length wordCount_
  uint16_t *lengths_ = nullptr;  // length wordCount_
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
