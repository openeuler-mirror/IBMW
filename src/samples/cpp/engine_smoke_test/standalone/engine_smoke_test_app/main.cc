// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Engine Smoke Test — comprehensive verification of R10 (topological sort,
// dependency graph, init rollback) and R15 (polymorphic init report).
//
// Test cases:
//   1. No-config lifecycle: init → async-start → shutdown (no config file)
//   2. Topological init order: verify subsystem init sequence from logs
//   3. Shutdown reverse order: verify shutdown is exact reverse of init (via timestamps)
//   4. R15 init report: verify report sections present and in correct order
//   5. Init rollback on failure: engine handles bad config gracefully
//   6. Phase transitions: verify engine phases through lifecycle
//   7. Config-based lifecycle: verify config with custom executor and console logger
//   8. Sequential lifecycles: 3 sequential create/init/start/shutdown cycles

#include <algorithm>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "engine/engine.h"

using namespace ibmw::runtime::core;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, msg)                                       \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::cout << "  FAIL: " << (msg) << " [" << __FILE__ << ":"   \
                << __LINE__ << "]" << std::endl;                     \
      ++g_fail;                                                      \
      return false;                                                  \
    }                                                                \
  } while (0)

#define TEST_CASE(name)                                              \
  std::cout << "--- Test: " << (name) << " ---" << std::endl

#define TEST_PASS(name)                                              \
  do {                                                               \
    std::cout << "  PASS: " << (name) << std::endl;                  \
    ++g_pass;                                                        \
  } while (0)

// Capture stdout+stderr at file-descriptor level.
// SimpleLogger writes to stderr; the console logger driver writes to stdout.
class OutputCapture {
 public:
  OutputCapture() {
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
    saved_stdout_ = dup(STDOUT_FILENO);
    saved_stderr_ = dup(STDERR_FILENO);
    tmp_file_ = tmpfile();
    int tmpfd = fileno(tmp_file_);
    dup2(tmpfd, STDOUT_FILENO);
    dup2(tmpfd, STDERR_FILENO);
  }
  ~OutputCapture() {
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
    dup2(saved_stdout_, STDOUT_FILENO);
    dup2(saved_stderr_, STDERR_FILENO);
    close(saved_stdout_);
    close(saved_stderr_);
    if (tmp_file_) fclose(tmp_file_);
  }
  std::string Get() {
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
    fseek(tmp_file_, 0, SEEK_END);
    long sz = ftell(tmp_file_);
    fseek(tmp_file_, 0, SEEK_SET);
    std::string result(static_cast<size_t>(sz), '\0');
    (void)fread(result.data(), 1, static_cast<size_t>(sz), tmp_file_);
    return result;
  }

 private:
  int saved_stdout_ = -1;
  int saved_stderr_ = -1;
  FILE* tmp_file_ = nullptr;
};

// Write a temporary config that enables a console logger (so all 11
// subsystem "initialized" / "stopped" messages are visible on stderr).
static const char* kTestCfgPath = "/tmp/ibmw_smoke_test_cfg.yaml";

static void WriteTestConfig() {
  std::ofstream f(kTestCfgPath);
  f << "ibmw:\n"
    << "  logging:\n"
    << "    core_level: INFO\n"
    << "    backends:\n"
    << "      - type: console\n";
  f.close();
}

// ---------------------------------------------------------------------------
// Test 1: No-config lifecycle
// ---------------------------------------------------------------------------
bool test_no_config_lifecycle() {
  TEST_CASE("No-config lifecycle (init → async-start → shutdown)");

  try {
    Engine engine;
    Engine::Options options;  // No config file.
    engine.Initialize(options);

    auto fu = engine.AsyncStart();

    // Engine should be in Running phase.
    TEST_ASSERT(engine.GetPhase() == Phase::kRunning,
                "Engine should be in Running phase after AsyncStart");

    engine.Shutdown();
    fu.wait();

    TEST_ASSERT(engine.GetPhase() == Phase::kStopped,
                "Engine should be in Stopped phase after shutdown");

    TEST_PASS("No-config lifecycle");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 2: Topological init order
//
// Capture log output and verify the "initialized" messages appear in a
// valid topological order (i.e., no subsystem initializes before its deps).
// ---------------------------------------------------------------------------
bool test_topological_init_order() {
  TEST_CASE("Topological init order verification");

  // Expected dependencies (subsystem → must appear after all deps):
  // Config:     (root)
  // Extensions: Config
  // MainLoop:   Config
  // Watchdog:   Config
  // Executors:  Config, MainLoop, Watchdog
  // Logging:    Executors
  // Memory:     Logging
  // RPC:        Logging, Executors
  // Transport:  Logging, Executors
  // Parameters: Logging
  // Modules:    Config, Extensions, Executors, Logging, Memory, RPC, Transport, Parameters

  struct DepSpec {
    std::string name;
    std::vector<std::string> deps;
  };

  std::vector<DepSpec> dep_graph = {
      {"Config", {}},
      {"Extensions", {"Config"}},
      {"MainLoop", {"Config"}},
      {"Watchdog", {"Config"}},
      {"Executors", {"Config", "MainLoop", "Watchdog"}},
      {"Logging", {"Executors"}},
      {"Memory", {"Logging"}},
      {"RPC", {"Logging", "Executors"}},
      {"Transport", {"Logging", "Executors"}},
      {"Parameters", {"Logging"}},
      {"Modules", {"Config", "Extensions", "Executors", "Logging",
                    "Memory", "RPC", "Transport", "Parameters"}},
  };

  try {
    WriteTestConfig();
    std::string logs;
    {
      OutputCapture cap;
      Engine engine;
      Engine::Options options;
      options.cfg_file_path = kTestCfgPath;
      engine.Initialize(options);
      // Shutdown while capture still active (to avoid mixing shutdown logs with test output).
      auto fu = engine.AsyncStart();
      engine.Shutdown();
      fu.wait();
      // Console logging is dispatched asynchronously through the watchdog
      // executor; capture after start/shutdown so init log messages have been
      // flushed into the file-descriptor capture.
      logs = cap.Get();
    }

    // Strip ANSI escape codes for reliable regex matching.
    std::regex ansi_re(R"(\x1b\[[0-9;]*m)");
    logs = std::regex_replace(logs, ansi_re, "");

    // Extract init order from log lines like "[Config] initialized"
    // SimpleLogger format:  "Info ... [Config] initialized  (file.cc:line)"
    // Core logger format:   "Info ... [core] [Memory] initialized (file.cc:line)"
    // We need the LAST bracketed name before "initialized".
    std::vector<std::string> init_order;
    std::regex re(R"(\[(\w+)\] initialized)");
    std::sregex_iterator it(logs.begin(), logs.end(), re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
      std::string name = (*it)[1].str();
      // Skip "core" — in core logger format, [core] [Name] initialized,
      // the regex matches [core] first but we actually want the next match.
      if (name == "core") continue;
      init_order.push_back(name);
    }

    TEST_ASSERT(init_order.size() == 11,
                "Expected 11 subsystems to initialize, got " +
                    std::to_string(init_order.size()));

    // Build position map.
    std::unordered_map<std::string, size_t> pos_map;
    for (size_t i = 0; i < init_order.size(); ++i) {
      pos_map[init_order[i]] = i;
    }

    // Verify each dependency constraint.
    for (const auto& spec : dep_graph) {
      auto it_self = pos_map.find(spec.name);
      TEST_ASSERT(it_self != pos_map.end(),
                  "Subsystem '" + spec.name + "' not found in init order");
      for (const auto& dep : spec.deps) {
        auto it_dep = pos_map.find(dep);
        TEST_ASSERT(it_dep != pos_map.end(),
                    "Dependency '" + dep + "' of '" + spec.name +
                        "' not found in init order");
        TEST_ASSERT(it_dep->second < it_self->second,
                    "'" + spec.name + "' (pos " +
                        std::to_string(it_self->second) +
                        ") initialized before its dependency '" + dep +
                        "' (pos " + std::to_string(it_dep->second) + ")");
      }
    }

    std::cout << "  Init order: ";
    for (size_t i = 0; i < init_order.size(); ++i) {
      if (i > 0) std::cout << " → ";
      std::cout << init_order[i];
    }
    std::cout << std::endl;

    TEST_PASS("Topological init order verification");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 3: Shutdown reverse order
// ---------------------------------------------------------------------------
bool test_shutdown_reverse_order() {
  TEST_CASE("Shutdown reverse order verification");

  try {
    WriteTestConfig();
    std::string logs;
    {
      OutputCapture cap;
      Engine engine;
      Engine::Options options;
      options.cfg_file_path = kTestCfgPath;
      engine.Initialize(options);
      auto fu = engine.AsyncStart();
      engine.Shutdown();
      fu.wait();
      logs = cap.Get();
    }
    // OutputCapture destroyed — stdout/stderr restored for TEST_ASSERT output.

    // Strip ANSI escape codes.
    std::regex ansi_re(R"(\x1b\[[0-9;]*m)");
    logs = std::regex_replace(logs, ansi_re, "");

    // Because stdout (core logger) and stderr (SimpleLogger) have different
    // buffering, log line ORDER in the capture may not reflect actual execution
    // order. We must use timestamps to reconstruct the true order.
    //
    // Log formats:
    //   SimpleLogger: "Info YYYY-MM-DD HH:MM:SS.UUUUUU [Name] <action>  (file:line)"
    //   Core logger:  "Info YYYY-MM-DD HH:MM:SS.UUUUUU [core] [Name] <action> (file:line)"
    struct TimedEvent {
      std::string timestamp;
      std::string name;
    };

    auto extract_timed_events = [&](const std::string& action) {
      std::vector<TimedEvent> events;
      // Split logs into lines and process each line.
      std::istringstream stream(logs);
      std::string line;
      while (std::getline(stream, line)) {
        // Check if this line contains the action keyword.
        if (line.find("] " + action) == std::string::npos) continue;

        // Extract timestamp.
        std::regex ts_re(R"((\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+))");
        std::smatch ts_match;
        if (!std::regex_search(line, ts_match, ts_re)) continue;
        std::string ts = ts_match[1].str();

        // Extract the subsystem name: last [Name] before the action word.
        // Use findall approach: find all [Word] in the line, take the last
        // one before the action.
        size_t action_pos = line.find("] " + action);
        // The bracket before action_pos contains the name.
        // Find the '[' that starts this bracket.
        size_t bracket_end = action_pos;  // position of ']'
        size_t bracket_start = line.rfind('[', bracket_end);
        if (bracket_start == std::string::npos) continue;
        std::string name = line.substr(bracket_start + 1,
                                        bracket_end - bracket_start - 1);
        if (name.empty() || name == "core") continue;
        events.push_back({ts, name});
      }
      // Sort by timestamp.
      std::sort(events.begin(), events.end(),
                [](const TimedEvent& a, const TimedEvent& b) {
                  return a.timestamp < b.timestamp;
                });
      return events;
    };

    auto init_events = extract_timed_events("initialized");
    auto shutdown_events = extract_timed_events("stopped");

    TEST_ASSERT(init_events.size() == 11,
                "Expected 11 init events, got " +
                    std::to_string(init_events.size()));
    TEST_ASSERT(shutdown_events.size() == 11,
                "Expected 11 shutdown events, got " +
                    std::to_string(shutdown_events.size()));

    // Build init order from timestamp-sorted events.
    std::vector<std::string> init_order;
    for (const auto& e : init_events) init_order.push_back(e.name);

    std::vector<std::string> shutdown_order;
    for (const auto& e : shutdown_events) shutdown_order.push_back(e.name);

    // Shutdown should be exact reverse of init.
    std::vector<std::string> expected_shutdown(init_order.rbegin(),
                                               init_order.rend());

    for (size_t i = 0; i < expected_shutdown.size(); ++i) {
      TEST_ASSERT(shutdown_order[i] == expected_shutdown[i],
                  "Shutdown position " + std::to_string(i) + ": expected '" +
                      expected_shutdown[i] + "', got '" + shutdown_order[i] +
                      "'");
    }

    std::cout << "  Shutdown order: ";
    for (size_t i = 0; i < shutdown_order.size(); ++i) {
      if (i > 0) std::cout << " -> ";
      std::cout << shutdown_order[i];
    }
    std::cout << std::endl;

    TEST_PASS("Shutdown reverse order verification");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 4: R15 initialization report sections
//
// The engine calls GenInitializationReport() internally during StartImpl(),
// before any subsystem Start() is called. We capture the output and verify
// the report sections are present and in correct order.
// ---------------------------------------------------------------------------
bool test_r15_init_report() {
  TEST_CASE("R15 initialization report (polymorphic generation)");

  try {
    WriteTestConfig();
    std::string logs;
    {
      OutputCapture cap;
      Engine engine;
      Engine::Options options;
      options.cfg_file_path = kTestCfgPath;
      engine.Initialize(options);
      auto fu = engine.AsyncStart();
      engine.Shutdown();
      fu.wait();
      logs = cap.Get();
    }
    // OutputCapture destroyed.

    // Strip ANSI escape codes.
    std::regex ansi_re(R"(\x1b\[[0-9;]*m)");
    logs = std::regex_replace(logs, ansi_re, "");

    // Expected sections (from subsystems that return non-empty reports):
    std::vector<std::string> expected_sections = {
        "System",         // Always first (hardcoded in engine)
        "Configuration",  // ConfigSubsystem
        "Extensions",     // ExtensionBus
        "Executors",      // ExecutorBus
        "Logging",        // LoggingSubsystem
        "RPC",            // ServiceDispatcher (has default 'local' driver)
        "Transport",      // TransportBus (has default 'local' driver)
        "Modules",        // ModuleBus (always reports, even if empty)
    };

    for (const auto& section : expected_sections) {
      std::string marker = "--- " + section + " ---";
      TEST_ASSERT(logs.find(marker) != std::string::npos,
                  "Report missing expected section '" + section + "'");
    }

    // Verify section order: each section must appear after the previous one.
    size_t prev_pos = 0;
    for (const auto& section : expected_sections) {
      std::string marker = "--- " + section + " ---";
      size_t pos = logs.find(marker);
      TEST_ASSERT(pos >= prev_pos,
                  "Section '" + section +
                      "' appears before previous section in report");
      prev_pos = pos;
    }

    // Verify System section contains key fields.
    TEST_ASSERT(logs.find("Version:") != std::string::npos,
                "Report missing 'Version:' in System section");
    TEST_ASSERT(logs.find("PID:") != std::string::npos,
                "Report missing 'PID:' in System section");
    TEST_ASSERT(logs.find("Executable:") != std::string::npos,
                "Report missing 'Executable:' in System section");

    // Verify that Memory and Parameters sections are NOT present
    // (they have empty reports).
    TEST_ASSERT(logs.find("--- Memory ---") == std::string::npos,
                "Report should NOT contain 'Memory' section (empty report)");
    TEST_ASSERT(logs.find("--- Parameters ---") == std::string::npos,
                "Report should NOT contain 'Parameters' section (empty report)");

    TEST_PASS("R15 initialization report");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 5: Init rollback on failure
//
// Create a YAML config that references a nonexistent package .so file.
// The engine should fail during Module initialization and roll back all
// previously-initialized subsystems cleanly.
// ---------------------------------------------------------------------------
bool test_init_rollback() {
  TEST_CASE("Init rollback on module load failure");

  // Write a temporary config that references a nonexistent .so.
  const char* tmp_cfg = "/tmp/ibmw_smoke_test_rollback.yaml";
  {
    std::ofstream f(tmp_cfg);
    f << "ibmw:\n"
      << "  logging:\n"
      << "    core_level: INFO\n"
      << "    backends:\n"
      << "      - type: console\n"
      << "  modules:\n"
      << "    packages:\n"
      << "      - path: ./nonexistent_fake_pkg_12345.so\n"
      << "        enable_modules: [FakeModule]\n"
      << "    modules:\n"
      << "      - name: FakeModule\n";
    f.close();
  }

  try {
    bool caught_exception = false;
    std::string logs;
    Phase phase_after_rollback = Phase::kConfigure;
    {
      OutputCapture cap;
      Engine engine;
      Engine::Options options;
      options.cfg_file_path = tmp_cfg;

      try {
        engine.Initialize(options);
      } catch (const std::exception&) {
        caught_exception = true;
        phase_after_rollback = engine.GetPhase();
      }
      logs = cap.Get();
    }
    // OutputCapture destroyed — stdout/stderr restored for TEST_ASSERT output.

    TEST_ASSERT(caught_exception,
                "Expected exception from loading nonexistent .so");

    TEST_ASSERT(phase_after_rollback == Phase::kStopped,
                "Engine should be Stopped after init rollback, got phase " +
                    std::to_string(static_cast<int>(phase_after_rollback)));

    // Strip ANSI escape codes.
    std::regex ansi_re(R"(\x1b\[[0-9;]*m)");
    logs = std::regex_replace(logs, ansi_re, "");

    // Verify rollback log message.
    TEST_ASSERT(logs.find("rolling back") != std::string::npos ||
                    logs.find("Rolling back") != std::string::npos,
                "Expected 'rolling back' message in logs");

    // Verify that initialized subsystems were shut down during rollback.
    // Count "stopped" messages — should be > 0.
    size_t stop_count = 0;
    size_t pos = 0;
    while ((pos = logs.find("] stopped", pos)) != std::string::npos) {
      ++stop_count;
      pos += 9;
    }
    TEST_ASSERT(stop_count > 0,
                "Expected at least one 'stopped' message during rollback");

    std::cout << "  Rollback shut down " << stop_count
              << " subsystem(s)" << std::endl;

    // Clean up.
    std::remove(tmp_cfg);

    TEST_PASS("Init rollback on module load failure");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: unexpected exception: " << e.what() << std::endl;
    std::remove(tmp_cfg);
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 6: Phase transitions
// ---------------------------------------------------------------------------
bool test_phase_transitions() {
  TEST_CASE("Phase transitions (Configure → Initialize → Start → Running → Stopping → Stopped)");

  try {
    Engine engine;

    // After construction, phase should be Configure.
    TEST_ASSERT(engine.GetPhase() == Phase::kConfigure,
                "Initial phase should be Configure");

    Engine::Options options;
    engine.Initialize(options);

    // After init, phase should be Initialize (stays there until Start).
    // Actually it stays at kInitialize after Initialize() returns.
    Phase p_after_init = engine.GetPhase();
    TEST_ASSERT(p_after_init == Phase::kInitialize,
                "Phase after Initialize should be kInitialize, got " +
                    std::to_string(static_cast<int>(p_after_init)));

    auto fu = engine.AsyncStart();

    TEST_ASSERT(engine.GetPhase() == Phase::kRunning,
                "Phase after AsyncStart should be Running");

    engine.Shutdown();
    fu.wait();

    TEST_ASSERT(engine.GetPhase() == Phase::kStopped,
                "Phase after shutdown should be Stopped");

    TEST_PASS("Phase transitions");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 7: Config-based lifecycle with real YAML
// ---------------------------------------------------------------------------
bool test_config_lifecycle() {
  TEST_CASE("Config-based lifecycle with YAML");

  // Write a minimal but valid config.
  const char* tmp_cfg = "/tmp/ibmw_smoke_test_config.yaml";
  {
    std::ofstream f(tmp_cfg);
    f << "ibmw:\n"
      << "  logging:\n"
      << "    core_level: INFO\n"
      << "    backends:\n"
      << "      - type: console\n"
      << "  scheduling:\n"
      << "    executors:\n"
      << "      - name: test_pool\n"
      << "        type: thread_pool\n"
      << "        options:\n"
      << "          thread_num: 1\n";
    f.close();
  }

  try {
    std::string logs;
    {
      OutputCapture cap;
      Engine engine;
      Engine::Options options;
      options.cfg_file_path = tmp_cfg;
      engine.Initialize(options);

      auto fu = engine.AsyncStart();

      // Verify engine is running with our config.
      TEST_ASSERT(engine.GetPhase() == Phase::kRunning,
                  "Engine should be Running with config");

      engine.Shutdown();
      fu.wait();

      TEST_ASSERT(engine.GetPhase() == Phase::kStopped,
                  "Engine should be Stopped");
      logs = cap.Get();
    }

    // Strip ANSI escape codes.
    std::regex ansi_re(R"(\x1b\[[0-9;]*m)");
    logs = std::regex_replace(logs, ansi_re, "");

    // Verify the initialization report (emitted by StartImpl) mentions our config.
    TEST_ASSERT(logs.find("test_pool") != std::string::npos,
                "Report should contain custom executor 'test_pool'");
    TEST_ASSERT(logs.find("console") != std::string::npos,
                "Report should mention 'console' log driver");

    std::remove(tmp_cfg);

    TEST_PASS("Config-based lifecycle with YAML");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    std::remove(tmp_cfg);
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Test 8: Multiple sequential engine lifecycles (stress test)
//
// Verifies that creating, initializing, starting, and destroying multiple
// Engine instances in sequence works without resource leaks or deadlocks.
// ---------------------------------------------------------------------------
bool test_sequential_lifecycles() {
  TEST_CASE("Sequential engine lifecycles (3 iterations)");

  try {
    for (int i = 0; i < 3; ++i) {
      Engine engine;
      Engine::Options options;
      engine.Initialize(options);

      auto fu = engine.AsyncStart();

      TEST_ASSERT(engine.GetPhase() == Phase::kRunning,
                  "Iteration " + std::to_string(i) +
                      ": engine should be Running");

      // Brief delay to simulate real usage.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      engine.Shutdown();
      fu.wait();

      TEST_ASSERT(engine.GetPhase() == Phase::kStopped,
                  "Iteration " + std::to_string(i) +
                      ": engine should be Stopped");
    }

    TEST_PASS("Sequential engine lifecycles");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
  std::cout << "========================================" << std::endl;
  std::cout << "IBMW Engine Smoke Test (R10 + R15)" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << std::endl;

  test_no_config_lifecycle();
  test_topological_init_order();
  test_shutdown_reverse_order();
  test_r15_init_report();
  test_init_rollback();
  test_phase_transitions();
  test_config_lifecycle();
  test_sequential_lifecycles();

  std::cout << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Results: " << g_pass << " passed, " << g_fail << " failed"
            << std::endl;
  std::cout << "========================================" << std::endl;

  return g_fail > 0 ? 1 : 0;
}
