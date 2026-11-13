// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <vector>

#include "c/util/string.h"

namespace ibmw::util {

// The key and key/value string view arrays a context hands out through its C
// ops. The owner updates them on every change of its meta data, so reading
// never writes and concurrent readers share one stable array.
class MetaViews {
 public:
  MetaViews() noexcept = default;
  // Views point into the owner's map: the owner rebuilds them for a copy.
  MetaViews(const MetaViews&) = delete;
  MetaViews& operator=(const MetaViews&) = delete;

  // A new entry. The map must keep key and value objects at a stable address
  // (node-based maps do).
  template <class String>
  void Add(const String& key, const String& value) {
    keys_.PushBack(View(key));
    key_vals_.PushBack(View(key));
    key_vals_.PushBack(View(value));
  }

  // The value of an existing entry was assigned.
  template <class String>
  void Update(const String& key, const String& value) noexcept {
    for (size_t i = 0; i < keys_.size; ++i) {
      if (keys_.data[i].str == key.data()) {
        key_vals_.data[2 * i + 1] = View(value);
        return;
      }
    }
  }

  void Clear() noexcept {
    keys_.size = 0;
    key_vals_.size = 0;
  }

  ibmw_string_view_array_t Keys() const noexcept { return {.str_array = keys_.data, .len = keys_.size}; }
  ibmw_string_view_array_t KeyVals() const noexcept {
    return {.str_array = key_vals_.data, .len = key_vals_.size};
  }

 private:
  template <class String>
  static ibmw_string_view_t View(const String& s) noexcept {
    return {s.data(), s.size()};
  }

  // Inline storage for the first N views, the heap after that.
  template <size_t N>
  struct Array {
    void PushBack(ibmw_string_view_t view) {
      if (data == inline_data && size < N) [[likely]] {
        inline_data[size++] = view;
        return;
      }
      PushBackToHeap(view);
    }

    [[gnu::noinline]] void PushBackToHeap(ibmw_string_view_t view) {
      if (data == inline_data) {
        heap.reserve(4 * N);
        heap.assign(inline_data, inline_data + size);
      }
      heap.resize(size);  // Clear() only resets size
      heap.push_back(view);
      data = heap.data();
      size = heap.size();
    }

    ibmw_string_view_t inline_data[N];
    ibmw_string_view_t* data = inline_data;
    size_t size = 0;
    std::vector<ibmw_string_view_t> heap;
  };

  static constexpr size_t kInlineEntries = 4;
  Array<kInlineEntries> keys_;
  Array<2 * kInlineEntries> key_vals_;
};

}  // namespace ibmw::util
