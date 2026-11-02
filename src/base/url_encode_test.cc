// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>

#include "base/url_encode.h"

namespace ibmw::common::util {
namespace {

using namespace std::string_literals;

TEST(UrlEncodeTest, RoundTrip) {
  const std::string raw = "a b/c?d=e&f\x01\xff~";
  EXPECT_EQ(UrlDecode(UrlEncode(raw)), raw);
  EXPECT_EQ(UrlDecode(UrlEncode(raw, false)), raw);
  EXPECT_EQ(HttpHeaderDecode(HttpHeaderEncode(raw)), raw);
  EXPECT_EQ(UrlDecode("%41%6a%00"), "Aj\0"s);
}

// API-041: a '%' that does not start a valid escape is kept literally instead
// of decoding to NUL or dropping the rest of the input.
TEST(UrlEncodeTest, InvalidEscapesAreKeptLiterally) {
  for (auto decode : {&UrlDecode, &HttpHeaderDecode}) {
    EXPECT_EQ(decode("a%zzb"), "a%zzb");
    EXPECT_EQ(decode("a%4gb"), "a%4gb");
    EXPECT_EQ(decode("%g1%41"), "%g1A");
    EXPECT_EQ(decode("ab%4"), "ab%4");
    EXPECT_EQ(decode("ab%"), "ab%");
    EXPECT_EQ(decode("%%41"), "%A");
  }
}

}  // namespace
}  // namespace ibmw::common::util
