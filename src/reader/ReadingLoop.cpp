#include "reader/ReadingLoop.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace {

constexpr const char *kDemoWords[] = {
    "This",   "is",      "the",      "minimal", "RSVP",   "demo",
    "reader", "running", "on",       "the",     "Waveshare", "AMOLED",
    "board",  "with",    "one",      "word",    "at",     "a",
    "time.",
};

constexpr size_t kDemoWordCount = sizeof(kDemoWords) / sizeof(kDemoWords[0]);
constexpr uint16_t kMinWpm = 100;
constexpr uint16_t kMaxWpm = 1000;
constexpr uint16_t kWpmStep = 25;
constexpr uint8_t kLongWordAfterChars = 7;
constexpr uint8_t kLongWordPercentPerChar = 8;
constexpr uint8_t kLongWordMaxPercent = 72;
constexpr uint8_t kCommaPausePercent = 65;
constexpr uint8_t kClausePausePercent = 85;
constexpr uint8_t kSentencePausePercent = 120;
constexpr uint8_t kMaxCatchUpWords = 4;

bool isWordCharacter(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool isIgnoredTrailingChar(char c) {
  switch (c) {
    case '"':
    case '\'':
    case ')':
    case ']':
    case '}':
      return true;
    default:
      return false;
  }
}

int readableCharacterCount(const String &word) {
  int count = 0;
  for (size_t i = 0; i < word.length(); ++i) {
    if (isWordCharacter(word[i])) {
      ++count;
    }
  }
  return count;
}

char trailingRhythmChar(const String &word) {
  for (int i = static_cast<int>(word.length()) - 1; i >= 0; --i) {
    const char c = word[static_cast<size_t>(i)];
    if (isIgnoredTrailingChar(c)) {
      continue;
    }
    return c;
  }
  return '\0';
}

bool looksLikeAbbreviation(const String &word) {
  String lowered = word;
  lowered.toLowerCase();

  constexpr const char *kKnownAbbreviations[] = {
      "mr.", "mrs.", "ms.", "dr.", "prof.", "sr.", "jr.", "st.", "vs.",
  };

  for (const char *abbreviation : kKnownAbbreviations) {
    if (lowered == abbreviation) {
      return true;
    }
  }

  return lowered.endsWith(".") && readableCharacterCount(lowered) <= 2;
}

uint32_t percentOf(uint32_t value, uint8_t percent) {
  return (value * percent) / 100;
}

uint32_t durationForWord(const String &word, uint32_t baseIntervalMs) {
  if (word.isEmpty() || baseIntervalMs == 0) {
    return baseIntervalMs;
  }

  uint32_t durationMs = baseIntervalMs;
  const int readableLength = readableCharacterCount(word);
  if (readableLength > kLongWordAfterChars) {
    const int extraChars = readableLength - kLongWordAfterChars;
    const uint8_t extraPercent = static_cast<uint8_t>(
        std::min(static_cast<int>(kLongWordMaxPercent),
                 extraChars * static_cast<int>(kLongWordPercentPerChar)));
    durationMs += percentOf(baseIntervalMs, extraPercent);
  }

  switch (trailingRhythmChar(word)) {
    case ',':
      durationMs += percentOf(baseIntervalMs, kCommaPausePercent);
      break;
    case ';':
    case ':':
      durationMs += percentOf(baseIntervalMs, kClausePausePercent);
      break;
    case '.':
      if (!looksLikeAbbreviation(word)) {
        durationMs += percentOf(baseIntervalMs, kSentencePausePercent);
      }
      break;
    case '!':
    case '?':
      durationMs += percentOf(baseIntervalMs, kSentencePausePercent);
      break;
    default:
      break;
  }

  return durationMs;
}

}  // namespace

void ReadingLoop::begin(uint32_t nowMs) {
  currentIndex_ = 0;
  lastAdvanceMs_ = nowMs;
  setCurrentWordFromIndex();
}

void ReadingLoop::setWords(std::vector<String> words, uint32_t nowMs) {
  loadedWords_ = std::move(words);
  currentIndex_ = 0;
  lastAdvanceMs_ = nowMs;
  setCurrentWordFromIndex();
}

void ReadingLoop::start(uint32_t nowMs) { lastAdvanceMs_ = nowMs; }

bool ReadingLoop::update(uint32_t nowMs) {
  bool changed = false;

  for (uint8_t catchUp = 0; catchUp < kMaxCatchUpWords; ++catchUp) {
    const uint32_t durationMs = currentWordDurationMs();
    if (durationMs == 0 || nowMs - lastAdvanceMs_ < durationMs) {
      break;
    }

    lastAdvanceMs_ += durationMs;
    if (!advance(1)) {
      break;
    }
    changed = true;
  }

  return changed;
}

const String &ReadingLoop::currentWord() const { return currentWord_; }

size_t ReadingLoop::currentIndex() const { return currentIndex_; }

uint16_t ReadingLoop::wpm() const { return wpm_; }

uint32_t ReadingLoop::wordIntervalMs() const { return 60000UL / wpm_; }

uint32_t ReadingLoop::currentWordDurationMs() const {
  return durationForWord(currentWord_, wordIntervalMs());
}

void ReadingLoop::scrub(int steps) {
  seekRelative(currentIndex_, steps);
}

void ReadingLoop::seekTo(size_t wordIndex) {
  const size_t count = wordCount();
  if (count == 0) {
    currentWord_ = "";
    return;
  }

  if (wordIndex >= count) {
    wordIndex = count - 1;
  }

  currentIndex_ = wordIndex;
  setCurrentWordFromIndex();
}

void ReadingLoop::seekRelative(size_t baseIndex, int steps) {
  const size_t count = wordCount();
  if (count == 0) {
    return;
  }

  if (baseIndex >= count) {
    baseIndex = count - 1;
  }

  int nextIndex = static_cast<int>(baseIndex) + steps;
  if (usingLoadedBook()) {
    if (nextIndex < 0) {
      nextIndex = 0;
    }
    if (nextIndex >= static_cast<int>(count)) {
      nextIndex = static_cast<int>(count) - 1;
    }
  } else {
    nextIndex %= static_cast<int>(count);
    if (nextIndex < 0) {
      nextIndex += static_cast<int>(count);
    }
  }

  currentIndex_ = static_cast<size_t>(nextIndex);
  setCurrentWordFromIndex();
}

void ReadingLoop::adjustWpm(int delta) {
  if (delta == 0) {
    return;
  }

  int nextWpm = static_cast<int>(wpm_);
  nextWpm += (delta > 0) ? kWpmStep : -static_cast<int>(kWpmStep);
  if (nextWpm < static_cast<int>(kMinWpm)) {
    nextWpm = kMinWpm;
  }
  if (nextWpm > static_cast<int>(kMaxWpm)) {
    nextWpm = kMaxWpm;
  }
  wpm_ = static_cast<uint16_t>(nextWpm);
}

void ReadingLoop::setWpm(uint16_t wpm) {
  if (wpm < kMinWpm) {
    wpm = kMinWpm;
  }
  if (wpm > kMaxWpm) {
    wpm = kMaxWpm;
  }
  wpm_ = wpm;
}

bool ReadingLoop::advance(size_t steps) {
  const size_t count = wordCount();
  if (count == 0) {
    currentWord_ = "";
    return false;
  }

  const size_t previousIndex = currentIndex_;
  if (usingLoadedBook()) {
    const size_t maxIndex = count - 1;
    if (currentIndex_ < maxIndex) {
      const size_t remaining = maxIndex - currentIndex_;
      currentIndex_ += (steps < remaining) ? steps : remaining;
    }
  } else {
    currentIndex_ = (currentIndex_ + steps) % count;
  }

  if (currentIndex_ == previousIndex) {
    return false;
  }

  setCurrentWordFromIndex();
  return true;
}

void ReadingLoop::setCurrentWordFromIndex() {
  if (wordCount() == 0) {
    currentWord_ = "";
    return;
  }

  currentWord_ = wordAt(currentIndex_);
}

size_t ReadingLoop::wordCount() const {
  if (!loadedWords_.empty()) {
    return loadedWords_.size();
  }
  return kDemoWordCount;
}

String ReadingLoop::wordAt(size_t index) const {
  if (!loadedWords_.empty()) {
    return loadedWords_[index];
  }
  return String(kDemoWords[index]);
}

bool ReadingLoop::usingLoadedBook() const { return !loadedWords_.empty(); }
