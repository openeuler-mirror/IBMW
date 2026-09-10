// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_storage/storage_options.hpp>

#include "convert_payload_manifest.h"

namespace {

using ibmw::examples::cpp::convert_smoke_test::CompareContent;
using ibmw::examples::cpp::convert_smoke_test::MessageContent;
using ibmw::examples::cpp::convert_smoke_test::ReadManifest;

std::vector<MessageContent> ReadRosbag2(
    const std::filesystem::path& path,
    std::string_view storage_id) {
  rosbag2_cpp::Reader reader;
  rosbag2_storage::StorageOptions options;
  options.uri = std::filesystem::absolute(path).string();
  options.storage_id = std::string(storage_id);
  reader.open(options);
  std::vector<MessageContent> result;
  while (reader.has_next()) {
    const auto message = reader.read_next();
    if (message == nullptr || message->serialized_data == nullptr ||
        message->serialized_data->buffer == nullptr) {
      throw std::runtime_error("rosbag2 message payload is null");
    }
    result.emplace_back(MessageContent{
        .topic = message->topic_name,
        .timestamp = static_cast<uint64_t>(message->time_stamp),
        .payload = std::string(
            reinterpret_cast<const char*>(message->serialized_data->buffer),
            message->serialized_data->buffer_length)});
  }
  reader.close();
  return result;
}

int Run(int argc, char** argv) {
  if (argc != 4) {
    throw std::runtime_error(
        "usage: validate_rosbag2_payload BAG STORAGE MANIFEST");
  }
  CompareContent(
      ReadManifest(argv[3]), ReadRosbag2(argv[1], argv[2]), false);
  std::cout << "PASS: rosbag2/" << argv[2]
            << " content validation (5 ordered std_msgs/String messages)\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "FAIL: rosbag2 payload validation: " << error.what() << '\n';
    return 1;
  }
}
