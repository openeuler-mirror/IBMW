// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>
#include <std_msgs/msg/string.hpp>

namespace ibmw::examples::cpp::convert_smoke_test {

struct MessageContent {
  std::string topic;
  uint64_t timestamp = 0;
  uint32_t sequence = 0;
  std::string payload;
};

inline std::string DecodeRosString(std::string_view payload) {
  rclcpp::SerializedMessage serialized(payload.size());
  auto& native = serialized.get_rcl_serialized_message();
  std::memcpy(native.buffer, payload.data(), payload.size());
  native.buffer_length = payload.size();
  std_msgs::msg::String message;
  rclcpp::Serialization<std_msgs::msg::String> serialization;
  serialization.deserialize_message(&serialized, &message);
  return message.data;
}

inline void ValidateFixture(const std::vector<MessageContent>& messages) {
  if (messages.size() != 5) {
    throw std::runtime_error(
        "expected 5 messages, got " + std::to_string(messages.size()));
  }
  for (size_t index = 0; index < messages.size(); ++index) {
    const auto& message = messages[index];
    if (message.topic != "/sequence_probe") {
      throw std::runtime_error("unexpected topic at message " +
                               std::to_string(index));
    }
    if (DecodeRosString(message.payload) != "probe") {
      throw std::runtime_error("decoded String.data mismatch at message " +
                               std::to_string(index));
    }
    if (index != 0 && message.timestamp <= messages[index - 1].timestamp) {
      throw std::runtime_error("message timestamps are not strictly ordered");
    }
  }
}

inline std::string HexEncode(std::string_view value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const unsigned char byte : value) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

inline std::string HexDecode(std::string_view value) {
  if (value.size() % 2 != 0) {
    throw std::runtime_error("manifest payload has an odd hex length");
  }
  std::string result;
  result.reserve(value.size() / 2);
  for (size_t index = 0; index < value.size(); index += 2) {
    unsigned byte = 0;
    std::istringstream input(std::string(value.substr(index, 2)));
    input >> std::hex >> byte;
    if (!input || byte > 255) {
      throw std::runtime_error("manifest payload contains invalid hex");
    }
    result.push_back(static_cast<char>(byte));
  }
  return result;
}

inline void WriteManifest(
    const std::filesystem::path& path,
    const std::vector<MessageContent>& messages) {
  ValidateFixture(messages);
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write reference manifest");
  }
  for (const auto& message : messages) {
    output << message.topic << '\t' << message.timestamp << '\t'
           << message.sequence << '\t' << HexEncode(message.payload) << '\n';
  }
  if (!output.good()) {
    throw std::runtime_error("failed to write reference manifest");
  }
}

inline std::vector<MessageContent> ReadManifest(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read reference manifest");
  }
  std::vector<MessageContent> result;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    MessageContent message;
    std::string encoded_payload;
    if (!std::getline(fields, message.topic, '\t') ||
        !(fields >> message.timestamp) || fields.get() != '\t' ||
        !(fields >> message.sequence) || fields.get() != '\t' ||
        !std::getline(fields, encoded_payload)) {
      throw std::runtime_error("reference manifest contains an invalid row");
    }
    message.payload = HexDecode(encoded_payload);
    result.emplace_back(std::move(message));
  }
  ValidateFixture(result);
  return result;
}

inline void CompareContent(
    const std::vector<MessageContent>& expected,
    const std::vector<MessageContent>& actual,
    bool compare_sequence) {
  ValidateFixture(actual);
  if (expected.size() != actual.size()) {
    throw std::runtime_error("candidate message count changed");
  }
  for (size_t index = 0; index < expected.size(); ++index) {
    if (actual[index].topic != expected[index].topic ||
        actual[index].timestamp != expected[index].timestamp ||
        actual[index].payload != expected[index].payload) {
      throw std::runtime_error(
          "topic, timestamp, order, or payload changed at message " +
          std::to_string(index));
    }
    if (compare_sequence &&
        actual[index].sequence != expected[index].sequence) {
      throw std::runtime_error(
          "IBMW message sequence changed at message " +
          std::to_string(index));
    }
  }
}

}  // namespace ibmw::examples::cpp::convert_smoke_test
