// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <random>
#include <regex>
#include <string>
#include <vector>

#include "base/url_parser.h"

namespace ibmw::common::util {
namespace {

// The regex implementation ParseUrl replaced (API-042), kept as the reference.
std::optional<Url<std::string>> RegexParseUrl(std::string_view url_str) {
  static const std::regex url_regex(
      R"(^(([^:\/?#]+)://)?(([^\/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?)", std::regex::ECMAScript);
  std::match_results<std::string_view::const_iterator> m;
  if (!std::regex_match(url_str.begin(), url_str.end(), m, url_regex)) return std::nullopt;
  Url<std::string> url;
  if (m[2].matched) url.protocol = std::string(m[2].first, m[2].second);
  if (m[4].matched) {
    std::string auth(m[4].first, m[4].second);
    const size_t pos = auth.find_first_of(':');
    if (pos != std::string::npos) {
      url.host = auth.substr(0, pos);
      url.service = auth.substr(pos + 1);
    } else {
      url.host = auth;
    }
  }
  if (m[5].matched) url.path = std::string(m[5].first, m[5].second);
  if (m[7].matched) url.query = std::string(m[7].first, m[7].second);
  if (m[9].matched) url.fragment = std::string(m[9].first, m[9].second);
  return url;
}

void ExpectSameAsRegex(const std::string& input) {
  SCOPED_TRACE(testing::PrintToString(input));
  const auto expected = RegexParseUrl(input);
  const auto actual = ParseUrl<std::string>(input);
  const auto view = ParseUrl<std::string_view>(input);
  ASSERT_EQ(actual.has_value(), expected.has_value());
  ASSERT_EQ(view.has_value(), expected.has_value());
  if (!expected) return;
  for (const auto& [a, e, v] : {std::tuple{actual->protocol, expected->protocol, view->protocol},
                                {actual->host, expected->host, view->host},
                                {actual->service, expected->service, view->service},
                                {actual->path, expected->path, view->path},
                                {actual->query, expected->query, view->query},
                                {actual->fragment, expected->fragment, view->fragment}}) {
    EXPECT_EQ(a, e);
    EXPECT_EQ(std::string(v), e);
  }
}

TEST(UrlParserTest, MatchesRegexSplit) {
  for (const char* input :
       {"", "http://", "http://host", "http://host:8080/a/b?x=1&y=2#frag", "host:8080",
        "localhost:1883", "tcp://127.0.0.1:1883", "/path/only", "?q", "#f", "a?b?c#d#e",
        "://x", "ht:tp://x", "a/b://c", "http://h:p:q/x", "http:/x", "mqtt://[::1]:1883/t",
        "x#a\nb", "x#a\rb", "x?a\nb#c", "a\nb://c/d", "http://h/p#", "http://h/p?", "::"}) {
    ExpectSameAsRegex(input);
  }
}

TEST(UrlParserTest, MatchesRegexOnRandomInput) {
  std::mt19937 rng(42);
  const std::string alphabet = "ab:/?#.\n\r1";
  for (int i = 0; i < 20000; ++i) {
    std::string input(rng() % 12, ' ');
    for (auto& c : input) c = alphabet[rng() % alphabet.size()];
    ExpectSameAsRegex(input);
    if (HasFatalFailure()) return;
  }
}

// The regex recursed per character and could overflow the stack.
TEST(UrlParserTest, LongInputIsParsed) {
  const std::string path(1 << 20, 'p');
  const auto url = ParseUrl<std::string_view>("http://h/" + path);
  ASSERT_TRUE(url);
  EXPECT_EQ(url->path.size(), path.size() + 1);
}

}  // namespace
}  // namespace ibmw::common::util
