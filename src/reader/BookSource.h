#pragma once

#include <Arduino.h>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

class BookSource {
 public:
  virtual ~BookSource() = default;
  virtual size_t size() const = 0;
  virtual String at(size_t index) const = 0;
  bool empty() const { return size() == 0; }
};

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

 private:
  std::vector<String> words_;
};

using BookSourcePtr = std::shared_ptr<BookSource>;
