#pragma once

#include <Arduino.h>
#include <vector>

struct ChapterMarker {
  String title;
  size_t wordIndex = 0;
};

struct BookContent {
  String title;
  std::vector<String> words;
  std::vector<ChapterMarker> chapters;

  void clear() {
    title = "";
    words.clear();
    chapters.clear();
  }
};
