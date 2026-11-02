// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Replaces the global operator new so tests can make the allocations of one
// thread fail on demand (see test::FailAllocations).

#include <cstddef>
#include <cstdlib>
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
