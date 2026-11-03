// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "grpc_ext/server/recv_dump.h"

namespace ibmw::extensions::grpc_extension::server {
namespace {

// EXT-010: same layout as before (hex row, then printable row), limited to
// the first bytes.
TEST(RecvDumpTest, HexAndPrintableRows) {
  const uint8_t data[] = {'P', 'R', 0x00, 0xff, ' '};
  EXPECT_EQ(FormatRecvDump(data, sizeof(data)), "50 52 00 ff 20 \nP  R  .  .     ");
}

TEST(RecvDumpTest, LongPayloadIsTruncated) {
  const std::vector<uint8_t> data(1000, 'a');
  const std::string dump = FormatRecvDump(data.data(), data.size(), 4);
  EXPECT_EQ(dump, "61 61 61 61 \na  a  a  a  \n... 996 more bytes");
}

TEST(RecvDumpTest, EmptyPayload) {
  EXPECT_EQ(FormatRecvDump(nullptr, 0), "\n");
}

}  // namespace
}  // namespace ibmw::extensions::grpc_extension::server
