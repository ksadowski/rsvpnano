#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

namespace utf8 {

constexpr uint32_t kReplacement = 0xFFFDu;

// Encodes a Unicode code point into 1..4 UTF-8 bytes appended to `out`.
// Returns the number of bytes appended. Code points outside the valid Unicode
// range produce U+FFFD (3 bytes).
inline size_t encode(uint32_t cp, String &out) {
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    cp = kReplacement;
  }

  if (cp < 0x80u) {
    out += static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800u) {
    out += static_cast<char>(0xC0u | (cp >> 6));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp < 0x10000u) {
    out += static_cast<char>(0xE0u | (cp >> 12));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
    return 3;
  }
  out += static_cast<char>(0xF0u | (cp >> 18));
  out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
  out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
  out += static_cast<char>(0x80u | (cp & 0x3Fu));
  return 4;
}

inline String encode(uint32_t cp) {
  String out;
  out.reserve(4);
  encode(cp, out);
  return out;
}

// Returns the number of UTF-8 bytes for the sequence starting at `lead`,
// or 1 for invalid lead bytes (caller should treat as a single replacement byte).
inline size_t sequenceLength(uint8_t lead) {
  if (lead < 0x80u) return 1;
  if ((lead & 0xE0u) == 0xC0u) return 2;
  if ((lead & 0xF0u) == 0xE0u) return 3;
  if ((lead & 0xF8u) == 0xF0u) return 4;
  return 1;
}

// Decodes the next code point from `data[i..length)`, advancing `i` past it.
// Returns U+FFFD on malformed bytes (advancing past a single invalid byte).
inline uint32_t next(const uint8_t *data, size_t length, size_t &i) {
  if (i >= length) {
    return kReplacement;
  }
  const uint8_t lead = data[i];
  if (lead < 0x80u) {
    ++i;
    return lead;
  }

  size_t needed = 0;
  uint32_t cp = 0;
  uint32_t minCp = 0;
  if ((lead & 0xE0u) == 0xC0u) {
    needed = 1;
    cp = lead & 0x1Fu;
    minCp = 0x80u;
  } else if ((lead & 0xF0u) == 0xE0u) {
    needed = 2;
    cp = lead & 0x0Fu;
    minCp = 0x800u;
  } else if ((lead & 0xF8u) == 0xF0u) {
    needed = 3;
    cp = lead & 0x07u;
    minCp = 0x10000u;
  } else {
    ++i;
    return kReplacement;
  }

  if (i + 1 + needed > length) {
    ++i;
    return kReplacement;
  }

  for (size_t k = 0; k < needed; ++k) {
    const uint8_t cont = data[i + 1 + k];
    if ((cont & 0xC0u) != 0x80u) {
      ++i;
      return kReplacement;
    }
    cp = (cp << 6) | (cont & 0x3Fu);
  }

  if (cp < minCp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    ++i;
    return kReplacement;
  }

  i += 1 + needed;
  return cp;
}

inline uint32_t next(const String &s, size_t &i) {
  return next(reinterpret_cast<const uint8_t *>(s.c_str()), s.length(), i);
}

// Counts the number of code points in `s`. Malformed bytes are counted as one
// code point each.
inline size_t codepointCount(const String &s) {
  size_t i = 0;
  size_t count = 0;
  const size_t length = s.length();
  const uint8_t *data = reinterpret_cast<const uint8_t *>(s.c_str());
  while (i < length) {
    next(data, length, i);
    ++count;
  }
  return count;
}

}  // namespace utf8
