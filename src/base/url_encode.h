// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

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
    } else if (str[i] == '%') {
      if (i + 2 < str.length()) {
        ret_str += static_cast<char>((FromHex(str[i + 1]) << 4) | FromHex(str[i + 2]));
        i += 2;
      } else {
        break;
      }
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
    if (str[i] == '%') {
      if (i + 2 < str.length()) {
        ret_str += static_cast<char>((FromHex(str[i + 1]) << 4) | FromHex(str[i + 2]));
        i += 2;
      } else {
        break;
      }
    } else {
      ret_str += str[i];
    }
  }
  return ret_str;
}

}  // namespace ibmw::common::util
