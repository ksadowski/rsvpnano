#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

#include "reader/BookContent.h"

class StorageManager {
 public:
  using StatusCallback = void (*)(void *context, const char *title, const char *line1,
                                  const char *line2, int progressPercent);

  void setStatusCallback(StatusCallback callback, void *context);
  bool begin();
  void end();
  void listBooks();
  void refreshBooks();
  bool loadBookContent(size_t index, BookContent &book, String *loadedPath = nullptr,
                       size_t *loadedIndex = nullptr);
  size_t bookCount() const;
  String bookPath(size_t index) const;
  String bookDisplayName(size_t index) const;
  String bookAuthorName(size_t index) const;

 private:
  bool ensureEpubConverted(const String &epubPath, String &rsvpPath);
  void refreshBookPaths();
  void notifyStatus(const char *title, const char *line1 = "", const char *line2 = "",
                    int progressPercent = -1);

  // Streams a `.rsvp` file through the parser into a sidecar `.idx` file when
  // the existing one is missing or stale, then opens a streaming book source
  // backed by it. This is what keeps memory usage bounded for very large
  // books (no per-word String stays in DRAM).
  bool buildIdxForRsvp(const String &rsvpPath, const String &idxPath);
  bool loadRsvpAsStreaming(const String &rsvpPath, BookContent &book);
  bool loadTextIntoMemory(const String &path, BookContent &book);

  bool mounted_ = false;
  bool listedOnce_ = false;
  StatusCallback statusCallback_ = nullptr;
  void *statusContext_ = nullptr;
  std::vector<String> bookPaths_;
};
