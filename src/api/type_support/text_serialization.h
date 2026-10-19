// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once

#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "nlohmann/json.hpp"
#include "c/util/type_support_base.h"

namespace ibmw::util::text {
using Json = nlohmann::json;

inline void Object(const Json& value, std::initializer_list<const char*> fields) {
  if (!value.is_object()) throw std::invalid_argument("Message must be an object.");
  for (auto it = value.begin(); it != value.end(); ++it) {
    bool found = false;
    for (const auto* field : fields) if (it.key() == field) found = true;
    if (!found) throw std::invalid_argument("Unknown message field: " + it.key());
  }
}

template <typename T, typename = void> struct HasCodec : std::false_type {};
template <typename T> struct HasCodec<T, std::void_t<
    decltype(IbmwFromJson(std::declval<const Json&>(), std::declval<T&>())),
    decltype(IbmwToJson(std::declval<const T&>()))>> : std::true_type {};

template <typename T> void Read(const Json& value, T& output, std::span<const size_t> bounds = {});
template <typename T> Json Write(const T& value);

template <typename T> struct Sequence : std::false_type {};
template <typename T, typename A> struct Sequence<std::vector<T, A>> : std::true_type {};
template <typename T> struct Array : std::false_type {};
template <typename T, size_t N> struct Array<std::array<T, N>> : std::true_type {};

template <typename T> void Read(const Json& value, T& output, std::span<const size_t> bounds) {
  if constexpr (std::is_same_v<T, bool>) {
    if (!value.is_boolean()) throw std::invalid_argument("Expected a boolean.");
    output = value.get<bool>();
  } else if constexpr (std::is_integral_v<T>) {
    if (!value.is_number_integer()) throw std::invalid_argument("Expected an integer.");
    if (value.is_number_unsigned()) {
      const auto number = value.get<uint64_t>();
      if (number > static_cast<uint64_t>(std::numeric_limits<T>::max()))
        throw std::out_of_range("Integer exceeds field range.");
    } else {
      const auto number = value.get<int64_t>();
      if constexpr (std::is_unsigned_v<T>) {
        if (number < 0 || static_cast<uint64_t>(number) > std::numeric_limits<T>::max())
          throw std::out_of_range("Integer exceeds unsigned field range.");
      } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) {
        throw std::out_of_range("Integer exceeds field range.");
      }
    }
    output = value.get<T>();
  } else if constexpr (std::is_floating_point_v<T>) {
    if (!value.is_number()) throw std::invalid_argument("Expected a number.");
    const auto number = value.get<long double>();
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<T>::max())
      throw std::out_of_range("Number exceeds field range.");
    output = static_cast<T>(number);
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (!value.is_string()) throw std::invalid_argument("Expected a string.");
    if (!bounds.empty() && bounds.front() && value.get_ref<const std::string&>().size() > bounds.front())
      throw std::out_of_range("String exceeds its bound.");
    output = value.get<std::string>();
  } else if constexpr (Sequence<T>::value || Array<T>::value) {
    if (!value.is_array()) throw std::invalid_argument("Expected an array.");
    if (!bounds.empty() && bounds.front() && value.size() > bounds.front())
      throw std::out_of_range("Field exceeds its bound.");
    if constexpr (Sequence<T>::value) output.resize(value.size());
    else if (value.size() != output.size()) throw std::invalid_argument("Incorrect fixed array length.");
    const auto element_bounds = bounds.empty() ? bounds : bounds.subspan(1);
    for (size_t i = 0; i < value.size(); ++i) {
      typename T::value_type element{};
      Read(value[i], element, element_bounds);
      output[i] = std::move(element);
    }
  } else if constexpr (HasCodec<T>::value) {
    IbmwFromJson(value, output);
  } else {
    throw std::invalid_argument("This message field has no text conversion support.");
  }
}

template <typename T> Json Write(const T& value) {
  if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (Sequence<T>::value || Array<T>::value) {
    auto result = Json::array();
    for (size_t i = 0; i < value.size(); ++i) result.push_back(Write<typename T::value_type>(value[i]));
    return result;
  } else if constexpr (HasCodec<T>::value) {
    return IbmwToJson(value);
  } else {
    throw std::invalid_argument("This message field has no text conversion support.");
  }
}

template <typename T> void Field(const Json& value, const char* name, T& output,
                                std::initializer_list<size_t> bounds) {
  if (!value.contains(name)) return;  // Keep the generated message's default.
  try {
    Read(value.at(name), output, {bounds.begin(), bounds.size()});
  } catch (const std::exception& error) {
    throw std::invalid_argument(std::string(name) + ": " + error.what());
  }
}

template <typename T> void Field(const Json& value, const char* name, T& output, size_t bound = 0) {
  Field(value, name, output, {bound});
}

inline Json Parse(ibmw_buffer_array_view_t buffers) {
  if (!buffers.data || !buffers.len) throw std::invalid_argument("Missing message buffers.");
  std::string text;
  for (size_t i = 0; i < buffers.len; ++i) {
    if (buffers.data[i].len && !buffers.data[i].data)
      throw std::invalid_argument("Missing message buffer data.");
    if (buffers.data[i].len) text.append(static_cast<const char*>(buffers.data[i].data), buffers.data[i].len);
  }
  return Json::parse(text);
}
inline void RequireFiniteNumbers(const Json& value) {
  if (value.is_number_float() && !std::isfinite(value.get<Json::number_float_t>()))
    throw std::invalid_argument("Cannot represent non-finite numbers as JSON.");
  if (value.is_structured()) {
    for (const auto& child : value) RequireFiniteNumbers(child);
  }
}

inline bool Store(const Json& value, const ibmw_buffer_array_allocator_t* allocator,
                  ibmw_buffer_array_t* buffers) {
  if (!allocator || !allocator->allocate || !buffers) return false;
  // dump() silently replaces NaN and infinities with null. Validate the entire
  // message, including custom codecs and nested arrays, before writing bytes.
  RequireFiniteNumbers(value);
  const auto text = value.dump();
  auto buffer = allocator->allocate(allocator->ctx, buffers, text.size());
  if (!buffer.data || buffer.len < text.size() || !buffers->data || !buffers->len) return false;
  std::memcpy(buffer.data, text.data(), text.size());
  buffers->data[buffers->len - 1].len = text.size();
  return true;
}
}  // namespace ibmw::util::text
