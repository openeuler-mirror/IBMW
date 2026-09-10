// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "convert_payload_manifest.h"
#include "record_playback_ext/storage/bag_reader_factory.h"

namespace {

using ibmw::examples::cpp::convert_smoke_test::CompareContent;
using ibmw::examples::cpp::convert_smoke_test::MessageContent;
using ibmw::examples::cpp::convert_smoke_test::ReadManifest;
using ibmw::examples::cpp::convert_smoke_test::WriteManifest;
using ibmw::extensions::record_playback_extension::BagMessage;
using ibmw::extensions::record_playback_extension::CreateBagReader;
using ibmw::extensions::record_playback_extension::ReaderOpenOptions;

std::vector<MessageContent> ReadIbmw(
    const std::filesystem::path& path,
    std::string_view storage_id) {
  auto reader = CreateBagReader(
      path, ReaderOpenOptions{.expected_storage_id = std::string(storage_id)});
  std::vector<MessageContent> result;
  reader->ReadMessages({}, [&](BagMessage&& message) {
    if (message.payload == nullptr) {
      throw std::runtime_error("IBMW message payload is null");
    }
    result.emplace_back(MessageContent{
        .topic = message.channel.topic.topic,
        .timestamp = message.log_time,
        .sequence = message.sequence,
        .payload = message.payload->JoinToString()});
    return true;
  });
  return result;
}

int Run(int argc, char** argv) {
  if (argc != 5) {
    throw std::runtime_error(
        "usage: validate_convert_payload dump|compare BAG STORAGE MANIFEST");
  }
  const auto messages = ReadIbmw(argv[2], argv[3]);
  if (std::string_view(argv[1]) == "dump") {
    WriteManifest(argv[4], messages);
  } else if (std::string_view(argv[1]) == "compare") {
    CompareContent(ReadManifest(argv[4]), messages, true);
  } else {
    throw std::runtime_error("operation must be dump or compare");
  }
  std::cout << "PASS: IBMW/" << argv[3]
            << " content validation (5 ordered std_msgs/String messages)\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "FAIL: IBMW payload validation: " << error.what() << '\n';
    return 1;
  }
}
