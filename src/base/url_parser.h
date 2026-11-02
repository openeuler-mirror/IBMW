// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ibmw::common::util {

/**
 * @brief Url structure
 */
template <class StringType = std::string_view>
  requires(std::is_same_v<StringType, std::string_view> ||
           std::is_same_v<StringType, std::string>)
struct Url {
  StringType protocol;
  StringType host;
  StringType service;  // port
  StringType path;
  StringType query;
  StringType fragment;
};

/**
 * @brief Parse a url
 * @note [protocol://][host][:service][path][?query][#fragment]
 */
template <class StringType = std::string_view>
std::optional<Url<StringType>> ParseUrl(std::string_view url_str) {
  // Same split as the regex
  //   ^(([^:/?#]+)://)?([^/?#]*)([^?#]*)(\?([^#]*))?(#(.*))?$
  // without building a std::regex per call or recursing per character.
  Url<StringType> url;
  std::string_view rest = url_str;

  if (const size_t scheme_end = rest.find("://");
      scheme_end != std::string_view::npos && scheme_end > 0 &&
      rest.substr(0, scheme_end).find_first_of(":/?#") == std::string_view::npos) {
    url.protocol = StringType(rest.substr(0, scheme_end));
    rest.remove_prefix(scheme_end + 3);
  }

  const std::string_view auth = rest.substr(0, rest.find_first_of("/?#"));
  rest.remove_prefix(auth.size());
  if (const size_t pos = auth.find(':'); pos != std::string_view::npos) {
    url.host = StringType(auth.substr(0, pos));
    url.service = StringType(auth.substr(pos + 1));
  } else {
    url.host = StringType(auth);
  }

  const std::string_view path = rest.substr(0, rest.find_first_of("?#"));
  url.path = StringType(path);
  rest.remove_prefix(path.size());

  if (!rest.empty() && rest.front() == '?') {
    const std::string_view query = rest.substr(1, rest.find('#') - 1);
    url.query = StringType(query);
    rest.remove_prefix(query.size() + 1);
  }

  if (!rest.empty()) {  // starts with '#'
    const std::string_view fragment = rest.substr(1);
    // ECMAScript '.' does not match line terminators, so the regex rejected them.
    if (fragment.find_first_of("\r\n") != std::string_view::npos) return std::nullopt;
    url.fragment = StringType(fragment);
  }

  return std::optional<Url<StringType>>{std::move(url)};
}

/**
 * @brief Join a url
 * @note [protocol://][host][:service][path][?query][#fragment]
 */
template <class StringType = std::string_view>
std::string JoinUrl(const Url<StringType>& url) {
  std::stringstream ss;

  if (!url.protocol.empty()) ss << url.protocol << "://";
  if (!url.host.empty()) ss << url.host;
  if (!url.service.empty()) ss << ":" << url.service;
  if (!url.path.empty()) {
    if (url.path[0] != '/') ss << '/';
    ss << url.path;
  }
  if (!url.query.empty()) ss << '?' << url.query;
  if (!url.fragment.empty()) ss << '#' << url.fragment;

  return ss.str();
}

}  // namespace ibmw::common::util
