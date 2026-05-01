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
