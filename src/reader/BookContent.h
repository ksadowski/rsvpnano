#pragma once

#include <Arduino.h>
#include <vector>

#include "reader/BookSource.h"

struct ChapterMarker {
  String title;
  size_t wordIndex = 0;
};

struct BookContent {
  String title;
  String author;
  // Backing source for words. May be an InMemoryBookSource (small books, demo)
  // or a streaming source backed by a `.rsvp.idx` file. Held as shared_ptr so
  // App / ReadingLoop can share the same source without copying.
  BookSourcePtr source;
  std::vector<ChapterMarker> chapters;
  std::vector<size_t> paragraphStarts;

  void clear() {
    title = "";
    author = "";
    source.reset();
    chapters.clear();
    paragraphStarts.clear();
  }
};
