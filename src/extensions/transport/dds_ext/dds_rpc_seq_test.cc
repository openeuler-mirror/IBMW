// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "dds_ext/dds_rpc_driver.h"

namespace ibmw::extensions::dds_extension {
namespace {

dds_rpc_rtps::GUID_t Guid(uint8_t seed) {
  dds_rpc_rtps::GUID_t guid = dds_rpc_rtps::GUID_t::unknown();
  for (size_t i = 0; i < 12; ++i) guid.guidPrefix.value[i] = seed + i;
  for (size_t i = 0; i < 4; ++i) guid.entityId.value[i] = seed + 12 + i;
  return guid;
}

dds_rpc_rtps::SampleIdentity Related(const dds_rpc_rtps::GUID_t& guid, int32_t high, uint32_t low) {
  dds_rpc_rtps::SampleIdentity identity;
  identity.writer_guid() = guid;
  identity.sequence_number(dds_rpc_rtps::SequenceNumber_t(high, low));
  return identity;
}

// TRANS-048: a reply only matches a pending call by the reader GUID and the
// full 64-bit id; the same low word with another high word is not ours.
TEST(DdsRpcReplySeqTest, MatchesGuidAndFullId) {
  const auto ours = Guid(1);
  EXPECT_EQ(DdsRpcReplySeq(Related(ours, 0, 7), ours), 7u);
  EXPECT_EQ(DdsRpcReplySeq(Related(ours, 0, UINT32_MAX), ours), UINT32_MAX);
  EXPECT_EQ(DdsRpcReplySeq(Related(ours, 1, 7), ours), std::nullopt);
  EXPECT_EQ(DdsRpcReplySeq(Related(Guid(2), 0, 7), ours), std::nullopt);
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
