// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace ibmw::util {

/**
 * @brief Move-only, SBO-optimized type-erased callable.
 *
 * Unlike ibmw::util::Function<Ops>, this wrapper accepts full C++ signatures
 * (references, STL types, coroutine return types, etc.) and does not require
 * a C-style ops struct.  It is intended for internal hot-path use such as
 * filter chains, where avoiding the overhead of std::function matters.
 *
 * Supports both const and mutable callables (e.g. mutable lambdas that
 * move-capture handles).  The stored callable is always invoked through a
 * non-const pointer to support the mutable case.
 *
 * @tparam Signature  Function signature, e.g. void(int&, std::string&&)
 * @tparam BufSize    Inline buffer size in bytes (default 128).
 *                    Closures that fit are stored inline; larger ones
 *                    fall back to a heap allocation.
 */
template <typename Signature, std::size_t BufSize = 128>
class InlineFunction;

template <typename R, typename... Args, std::size_t BufSize>
class InlineFunction<R(Args...), BufSize> {
  // ---- vtable ---------------------------------------------------------------
  struct VTable {
    R (*invoke)(void*, Args...);          // mutable invoke to support mutable lambdas
    void (*move)(void* src, void* dst);   // relocate src -> dst (placement-new + destroy src)
    void (*destroy)(void*);               // destroy the stored object
  };

  // ---- small-object storage -------------------------------------------------
  static_assert(BufSize >= sizeof(void*), "BufSize too small");
  alignas(std::max_align_t) mutable unsigned char buf_[BufSize];
  const VTable* vt_ = nullptr;
  bool on_heap_ = false;

  // ---- per-type vtable generator --------------------------------------------
  template <typename F, bool Heap>
  static const VTable* vtable_for() noexcept {
    static constexpr VTable vt = {
        // invoke (non-const to support mutable lambdas)
        .invoke = [](void* obj, Args... args) -> R {
          if constexpr (Heap) {
            return (**static_cast<F**>(obj))(std::forward<Args>(args)...);
          } else {
            return (*static_cast<F*>(obj))(std::forward<Args>(args)...);
          }
        },
        // move (relocate src -> dst, destroy src)
        .move = [](void* src, void* dst) {
          if constexpr (Heap) {
            // Just move the pointer
            new (dst) F*(std::exchange(*static_cast<F**>(src), nullptr));
          } else {
            new (dst) F(std::move(*static_cast<F*>(src)));
            static_cast<F*>(src)->~F();
          }
        },
        // destroy
        .destroy = [](void* obj) {
          if constexpr (Heap) {
            delete *static_cast<F**>(obj);
          } else {
            static_cast<F*>(obj)->~F();
          }
        }};
    return &vt;
  }

  void reset() noexcept {
    if (vt_) {
      vt_->destroy(&buf_);
      vt_ = nullptr;
      on_heap_ = false;
    }
  }

 public:
  InlineFunction() = default;
  InlineFunction(std::nullptr_t) noexcept {}

  ~InlineFunction() { reset(); }

  // Move-only
  InlineFunction(const InlineFunction&) = delete;
  InlineFunction& operator=(const InlineFunction&) = delete;

  InlineFunction(InlineFunction&& other) noexcept
      : vt_(std::exchange(other.vt_, nullptr)),
        on_heap_(std::exchange(other.on_heap_, false)) {
    if (vt_) {
      vt_->move(&other.buf_, &buf_);
    }
  }

  InlineFunction& operator=(InlineFunction&& other) noexcept {
    if (this != &other) {
      reset();
      vt_ = std::exchange(other.vt_, nullptr);
      on_heap_ = std::exchange(other.on_heap_, false);
      if (vt_) {
        vt_->move(&other.buf_, &buf_);
      }
    }
    return *this;
  }

  InlineFunction& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  // Construct from any callable
  template <typename F,
            typename = std::enable_if_t<
                std::is_invocable_r_v<R, std::decay_t<F>, Args...> &&
                !std::is_same_v<std::decay_t<F>, InlineFunction>>>
  InlineFunction(F&& f) {  // NOLINT(google-explicit-constructor)
    using Decayed = std::decay_t<F>;

    if constexpr (sizeof(Decayed) <= BufSize &&
                  alignof(Decayed) <= alignof(std::max_align_t) &&
                  std::is_nothrow_move_constructible_v<Decayed>) {
      // SBO path -- store inline
      vt_ = vtable_for<Decayed, false>();
      on_heap_ = false;
      new (&buf_) Decayed(std::forward<F>(f));
    } else {
      // Heap path
      vt_ = vtable_for<Decayed, true>();
      on_heap_ = true;
      new (&buf_) Decayed*(new Decayed(std::forward<F>(f)));
    }
  }

  template <typename F,
            typename = std::enable_if_t<
                std::is_invocable_r_v<R, std::decay_t<F>, Args...> &&
                !std::is_same_v<std::decay_t<F>, InlineFunction>>>
  InlineFunction& operator=(F&& f) {
    reset();
    new (this) InlineFunction(std::forward<F>(f));
    return *this;
  }

  explicit operator bool() const noexcept { return vt_ != nullptr; }

  R operator()(Args... args) const {
    assert(vt_ && "Calling empty InlineFunction");
    return vt_->invoke(const_cast<void*>(static_cast<const void*>(&buf_)),
                       std::forward<Args>(args)...);
  }
};

}  // namespace ibmw::util
