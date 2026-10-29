// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ibmw::selfdev::shm::test {

// Every test binary uses a private service root (mkdtemp) exported through
// IBMW_SHM_ROOT so child processes join the same discovery directory.
const std::string& TestRoot();

// Unique per call; keeps tests in one process from seeing each other.
std::string UniqueUrl(const std::string& base);

bool WaitFor(const std::function<bool()>& pred,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

// Test payload: header + deterministic bytes derived from `seq`.
void FillPattern(void* payload, uint32_t size, uint64_t seq);
// Returns the sequence stored in the payload or ~0 when the pattern is broken.
uint64_t CheckPattern(const void* payload, uint32_t size);

struct Peer {
  pid_t pid = -1;
  int out = -1;  // read end of the child's stdout
};
// Starts ibmw_selfdev_shm_peer with `args`; extra env as "K=V".
Peer SpawnPeer(const std::vector<std::string>& args, const std::vector<std::string>& env = {});
// Reads one line from the child's stdout ("" on timeout/EOF).
std::string ReadLine(Peer& p, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));
// Waits for exit; returns the exit code, 128+signal, or -1 on timeout (then kills).
int WaitPeer(Peer& p, std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
void KillPeer(Peer& p, int sig);

// Voluntary + involuntary context switches of the thread named `comm`.
// Returns -1 when no such thread exists.
int64_t ThreadSwitches(const std::string& comm);

}  // namespace ibmw::selfdev::shm::test
