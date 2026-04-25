#pragma once

#include <Arduino.h>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

// Abstract source of book words. Implementations can keep the words in DRAM
// (small books) or stream them on demand from the SD card (large books).
class BookSource {
 public:
  virtual ~BookSource() = default;

  // Total number of readable words in the book.
  virtual size_t size() const = 0;

  // Returns the i-th word as a UTF-8 String. Returns empty String if the index
  // is out of range or if a transient SD read error occurred.
  virtual String at(size_t index) const = 0;

  bool empty() const { return size() == 0; }
};

// Backing store that holds every word in DRAM. Suitable for small files (e.g.
// .txt books or the built-in demo word list).
class InMemoryBookSource : public BookSource {
 public:
  InMemoryBookSource() = default;
  explicit InMemoryBookSource(std::vector<String> words) : words_(std::move(words)) {}

  size_t size() const override { return words_.size(); }
  String at(size_t index) const override {
    if (index >= words_.size()) {
      return String();
    }
    return words_[index];
  }

  std::vector<String> &words() { return words_; }
  const std::vector<String> &words() const { return words_; }

 private:
  std::vector<String> words_;
};

using BookSourcePtr = std::shared_ptr<BookSource>;
