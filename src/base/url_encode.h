// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ibmw::common::util {

inline unsigned char ToHex(unsigned char x, bool up) {
  return x > 9 ? x + (up ? 'A' : 'a') - 10 : (x + '0');
}

inline unsigned char FromHex(unsigned char x) {
  if (x >= '0' && x <= '9') {
    return x - '0';
  }
  if (x >= 'A' && x <= 'F') {
    return x - 'A' + 10;
  }
  if (x >= 'a' && x <= 'f') {
    return x - 'a' + 10;
  }
  return 0;
}

inline bool IsHexDigit(unsigned char x) {
  return (x >= '0' && x <= '9') || ((x | 0x20) >= 'a' && (x | 0x20) <= 'f');
}

// Decodes the "%XY" escape starting at str[i]; false if str[i + 1] and
// str[i + 2] are not two hex digits.
inline bool DecodePercentEscape(std::string_view str, size_t i, char& out) {
  if (i + 2 >= str.length()) return false;
  const auto high = static_cast<unsigned char>(str[i + 1]);
  const auto low = static_cast<unsigned char>(str[i + 2]);
  if (!IsHexDigit(high) || !IsHexDigit(low)) return false;
  out = static_cast<char>((FromHex(high) << 4) | FromHex(low));
  return true;
}

inline std::string UrlEncode(std::string_view str, bool up = true) {
  std::string ret_str;
  ret_str.reserve(str.length() * 3);

  for (unsigned char c : str) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      ret_str += static_cast<char>(c);
    } else if (c == ' ') {
      ret_str += '+';
    } else {
      ret_str += '%';
      ret_str += static_cast<char>(ToHex(c >> 4, up));
      ret_str += static_cast<char>(ToHex(c & 0xF, up));
    }
  }
  return ret_str;
}

inline std::string UrlDecode(std::string_view str) {
  std::string ret_str;
  ret_str.reserve(str.length());

  for (size_t i = 0; i < str.length(); ++i) {
    if (str[i] == '+') {
      ret_str += ' ';
    } else if (char decoded; str[i] == '%' && DecodePercentEscape(str, i, decoded)) {
      ret_str += decoded;
      i += 2;
    } else {
      ret_str += str[i];
    }
  }

  return ret_str;
}

inline std::string HttpHeaderEncode(std::string_view str, bool up = true) {
  std::string ret_str;
  ret_str.reserve(str.length() * 3);

  for (unsigned char c : str) {
    if (std::isalnum(c) || c == '-') {
      ret_str += static_cast<char>(c);
    } else {
      ret_str += '%';
      ret_str += static_cast<char>(ToHex(c >> 4, up));
      ret_str += static_cast<char>(ToHex(c & 0xF, up));
    }
  }
  return ret_str;
}

inline std::string HttpHeaderDecode(std::string_view str) {
  std::string ret_str;
  ret_str.reserve(str.length());

  for (size_t i = 0; i < str.length(); ++i) {
    if (char decoded; str[i] == '%' && DecodePercentEscape(str, i, decoded)) {
      ret_str += decoded;
      i += 2;
    } else {
      ret_str += str[i];
    }
  }
  return ret_str;
}

}  // namespace ibmw::common::util
