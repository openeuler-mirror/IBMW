// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Replaces the global operator new so tests can make the allocations of one
// thread fail on demand (see test::FailAllocations).

#include <cstddef>
#include <cstdlib>
#include <stdlib.h>
#include <new>

#include "test_util.h"

namespace {

thread_local int t_fail_allocations = 0;

void* Allocate(std::size_t size) {
  if (t_fail_allocations > 0) {
    --t_fail_allocations;
    throw std::bad_alloc();
  }
  void* p = std::malloc(size != 0 ? size : 1);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* AllocateAligned(std::size_t size, std::align_val_t align) {
  if (t_fail_allocations > 0) {
    --t_fail_allocations;
    throw std::bad_alloc();
  }
  const auto a = static_cast<std::size_t>(align);
  void* p = nullptr;
  if (::posix_memalign(&p, a < sizeof(void*) ? sizeof(void*) : a, size != 0 ? size : 1) != 0) throw std::bad_alloc();
  return p;
}

void* AllocateNoThrow(std::size_t size, std::align_val_t align = std::align_val_t{0}) noexcept {
  try {
    return align == std::align_val_t{0} ? Allocate(size) : AllocateAligned(size, align);
  } catch (...) {
    return nullptr;
  }
}

}  // namespace

namespace ibmw::selfdev::shm::test {

void FailAllocations(int n) noexcept { t_fail_allocations = n; }

}  // namespace ibmw::selfdev::shm::test

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// The aligned and nothrow forms fail on demand too (over-aligned types and
// std::nothrow callers would otherwise bypass the injection).
void* operator new(std::size_t size, std::align_val_t a) { return AllocateAligned(size, a); }
void* operator new[](std::size_t size, std::align_val_t a) { return AllocateAligned(size, a); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return AllocateNoThrow(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return AllocateNoThrow(size); }
void* operator new(std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
  return AllocateNoThrow(size, a);
}
void* operator new[](std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
  return AllocateNoThrow(size, a);
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
