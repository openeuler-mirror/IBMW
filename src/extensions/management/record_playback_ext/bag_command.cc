// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/bag_command.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "base/exception.h"
#include "engine/engine.h"
#include "engine/transport/serialized_transport_driver.h"
#include "engine/type_support_pkg_loader.h"
#include "nlohmann/json.hpp"
#include "record_playback_ext/capture/cached_transport_subscription_source.h"
#include "record_playback_ext/capture/runtime_tap_source.h"
#include "record_playback_ext/capture/transport_subscription_source.h"
#include "record_playback_ext/coordinator/bag_capture_planner.h"
#include "record_playback_ext/coordinator/dynamic_capture_controller.h"
#include "record_playback_ext/coordinator/recorder_coordinator.h"
#include "record_playback_ext/coordinator/record_topic_selector.h"
#include "record_playback_ext/converter/bag_converter.h"
#include "record_playback_ext/discovery/bag_topic_graph.h"
#include "record_playback_ext/player/player.h"
#include "record_playback_ext/runtime/runtime_registry.h"
#include "record_playback_ext/runtime/runtime_tap_stream.h"
#include "record_playback_ext/storage/bag_reader_factory.h"
#include "record_playback_ext/storage/mcap_bag_reader.h"
#include "record_playback_ext/storage/mcap_storage.h"
#include "record_playback_ext/storage/storage_factory.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_snapshot_requested{false};
std::atomic<bool> g_pause_toggle_requested{false};

void BagSignalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_stop_requested = true;
  } else if (signal == SIGUSR1) {
    g_snapshot_requested = true;
  } else if (signal == SIGUSR2) {
    g_pause_toggle_requested = true;
  }
}

struct SignalGuard {
  struct sigaction old_int {};
  struct sigaction old_term {};
  struct sigaction old_usr1 {};
  struct sigaction old_usr2 {};

  SignalGuard() {
    struct sigaction action {};
    action.sa_handler = BagSignalHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);
    sigaction(SIGUSR1, &action, &old_usr1);
    sigaction(SIGUSR2, &action, &old_usr2);
  }

  ~SignalGuard() {
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
    sigaction(SIGUSR1, &old_usr1, nullptr);
    sigaction(SIGUSR2, &old_usr2, nullptr);
  }
};

struct RecordCliOptions {
  std::vector<std::string> topics;
  std::filesystem::path output;
  bool help_requested = false;
  bool all = false;
  std::string regex;
  std::string exclude_regex;
  bool include_unpublished_topics = false;
  bool include_hidden_topics = false;
  bool ignore_leaf_topics = false;
  bool no_discovery = false;
  uint32_t polling_interval_ms = 100;
  std::string storage_id = "mcap";
  std::string serialization_format = "auto";
  uint64_t max_bag_size = 0;
  double max_bag_duration_seconds = 0.0;
  size_t max_cache_size = 100 * 1024 * 1024;
  std::string compression_mode = "none";
  std::string compression_format;
  size_t compression_queue_size = 1;
  size_t compression_threads = 1;
  bool snapshot_mode = false;
  std::string storage_preset_profile;
  std::filesystem::path storage_config_file;
  bool start_paused = false;
  bool use_sim_time = false;
  std::string log_level = "info";
};

void PrintBagUsage() {
  std::cout
      << "Usage:\n"
         "  ibmw bag record <topic> [<topic> ...] [options]\n"
         "  ibmw bag record -a [options]\n"
         "  ibmw bag info <bag_directory>\n"
         "  ibmw bag play <bag_directory>\n"
         "  ibmw bag convert <input_bag> -o <output_bag> [options]\n\n"
         "Record selection:\n"
         "  -a, --all                         Record all eligible topics\n"
         "  -e, --regex <regex>               Include matching topics\n"
         "  -x, --exclude <regex>             Exclude matching topics\n"
         "  --include-hidden-topics           Include hidden topics\n"
         "  --include-unpublished-topics      Include subscriber-only topics\n"
         "  --ignore-leaf-topics              Ignore topics without publishers\n"
         "  --no-discovery                    Use only the initial graph snapshot\n"
         "  -p, --polling-interval <ms>       Discovery polling period\n"
         "  -o, --output <bag_directory>      Output directory\n"
         "  -s, --storage <id>                mcap or sqlite3\n"
         "  -f, --serialization-format <fmt>  auto/cdr/json/ros2\n"
         "  -b, --max-bag-size <bytes>        Split size, zero disables\n"
         "  -d, --max-bag-duration <seconds>  Split duration, zero disables\n"
         "  --max-cache-size <bytes>          Bounded queue/cache size\n"
         "  --compression-mode <mode>         none/file/message\n"
         "  --compression-format <format>     zstd or lz4\n"
         "  --compression-queue-size <count>  Compression queue limit\n"
         "  --compression-threads <count>     Zero uses CPU count\n"
         "  --snapshot-mode                   Buffer until SIGUSR1\n"
         "  --storage-preset-profile <name>   Storage preset\n"
         "  --storage-config-file <path>      Storage YAML configuration\n"
         "  --start-paused                    Resume/pause with SIGUSR2\n"
         "  --use-sim-time                    Gate on /clock timestamps\n"
         "  --log-level <level>               debug/info/warn/error/fatal\n\n"
         "Info:\n"
         "  -s, --storage <id>                Require a storage plugin ID\n\n"
         "Play:\n"
         "  -s, --storage <id>                Require a storage plugin ID\n"
         "  --read-ahead-queue-size <count>   Bounded prefetch queue\n"
         "  -r, --rate <factor>               Playback rate greater than zero\n"
         "  --topics <topic> [<topic> ...]    Play selected topics\n"
         "  -l, --loop                        Repeat playback until stopped\n"
         "  -m, --remap <from:=to>            Remap a topic\n"
         "  -d, --delay <seconds>             Delay before each playback loop\n"
         "  --disable-keyboard-controls       Disable space pause/resume\n"
         "  -p, --start-paused                Start in paused state\n"
         "  --start-offset <seconds>          Skip from the bag start\n"
         "  --config-file <path>              Required playback transport configuration\n"
         "  --storage-config-file <path>      Reader configuration YAML\n"
         "  --wait-for-all-acked <seconds>    Wait for reliable acknowledgments\n"
         "  --log-level <level>               debug/info/warn/error/fatal\n\n"
         "Convert:\n"
         "  -i, --input <bag>                 Input bag (or positional)\n"
         "  -o, --output <bag_directory>      Output bag directory\n"
         "  --input-format <format>           auto, ibmw, or rosbag2\n"
         "  --output-format <format>          ibmw or rosbag2\n"
         "  -s, --storage <id>                Output storage: mcap or sqlite3\n"
         "  --input-storage <id>              Require input storage plugin ID\n"
         "  -b, --max-bag-size <bytes>        Output split size, zero disables\n"
         "  -d, --max-bag-duration <seconds>  Output split duration\n"
         "  --compression-mode <mode>         none/file/message (MCAP only)\n"
         "  --compression-format <format>     zstd or lz4\n"
         "  --compression-queue-size <count>  Compression queue limit\n"
         "  --compression-threads <count>     Zero uses CPU count\n"
         "  --storage-preset-profile <name>   Output storage preset\n"
         "  --input-storage-config-file <p>   Reader configuration YAML\n"
         "  --storage-config-file <path>      Writer configuration YAML\n";
}

struct InfoCliOptions {
  std::filesystem::path bag_path;
  std::string storage_id;
  bool help_requested = false;
};

struct PlayCliOptions {
  std::filesystem::path bag_path;
  std::string storage_id;
  size_t read_ahead_queue_size = 1000;
  double rate = 1.0;
  std::vector<std::string> topics;
  bool loop = false;
  std::map<std::string, std::string> remaps;
  double delay_seconds = 0.0;
  bool disable_keyboard_controls = false;
  bool start_paused = false;
  double start_offset_seconds = 0.0;
  std::string log_level = "info";
  std::filesystem::path config_file;
  std::filesystem::path storage_config_file;
  double wait_for_all_acked_seconds = -1.0;
  bool help_requested = false;
};

struct ConvertCliOptions {
  std::filesystem::path input;
  std::filesystem::path output;
  std::string input_format = "auto";
  std::string output_format = "ibmw";
  std::string input_storage_id;
  std::string storage_id = "mcap";
  uint64_t max_bag_size = 0;
  double max_bag_duration_seconds = 0.0;
  std::string compression_mode = "none";
  std::string compression_format;
  size_t compression_queue_size = 1;
  size_t compression_threads = 1;
  std::string storage_preset_profile;
  std::filesystem::path input_storage_config_file;
  std::filesystem::path storage_config_file;
  bool help_requested = false;
};

uint64_t ParseDurationNanoseconds(double seconds, std::string_view option);

uint32_t CliLogLevel(std::string_view level) {
  if (level == "debug") {
    return ibmw::common::util::kLogLevelDebug;
  }
  if (level == "info") {
    return ibmw::common::util::kLogLevelInfo;
  }
  if (level == "warn") {
    return ibmw::common::util::kLogLevelWarn;
  }
  if (level == "error") {
    return ibmw::common::util::kLogLevelError;
  }
  return ibmw::common::util::kLogLevelFatal;
}

class SimpleLoggerLevelGuard {
 public:
  explicit SimpleLoggerLevelGuard(std::string_view level)
      : previous_(ibmw::common::util::SimpleLogger::GetLogLevel()) {
    ibmw::common::util::SimpleLogger::SetLogLevel(CliLogLevel(level));
  }

  ~SimpleLoggerLevelGuard() {
    ibmw::common::util::SimpleLogger::SetLogLevel(previous_);
  }

 private:
  uint32_t previous_;
};

std::filesystem::path DefaultOutputPath() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
  localtime_r(&time, &local_time);
  std::ostringstream name;
  name << "ibmwbag_" << std::put_time(&local_time, "%Y%m%d_%H%M%S");
  return name.str();
}

RecordCliOptions ParseRecordOptions(int argc, const char* const* argv) {
  RecordCliOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-o" || argument == "--output") {
      MW_ASSERT(index + 1 < argc, "{} requires a path.", argument);
      options.output = argv[++index];
    } else if (argument.starts_with("--output=")) {
      options.output = argument.substr(std::string("--output=").size());
    } else if (argument == "-a" || argument == "--all") {
      options.all = true;
    } else if (argument == "-e" || argument == "--regex") {
      MW_ASSERT(index + 1 < argc, "{} requires a regular expression.", argument);
      options.regex = argv[++index];
    } else if (argument.starts_with("--regex=")) {
      options.regex = argument.substr(std::string("--regex=").size());
    } else if (argument == "-x" || argument == "--exclude") {
      MW_ASSERT(index + 1 < argc, "{} requires a regular expression.", argument);
      options.exclude_regex = argv[++index];
    } else if (argument.starts_with("--exclude=")) {
      options.exclude_regex =
          argument.substr(std::string("--exclude=").size());
    } else if (argument == "--include-unpublished-topics") {
      options.include_unpublished_topics = true;
    } else if (argument == "--include-hidden-topics") {
      options.include_hidden_topics = true;
    } else if (argument == "--ignore-leaf-topics") {
      options.ignore_leaf_topics = true;
    } else if (argument == "--no-discovery") {
      options.no_discovery = true;
    } else if (argument == "-p" || argument == "--polling-interval") {
      MW_ASSERT(index + 1 < argc, "{} requires milliseconds.", argument);
      options.polling_interval_ms =
          static_cast<uint32_t>(std::stoul(argv[++index]));
    } else if (argument.starts_with("--polling-interval=")) {
      options.polling_interval_ms = static_cast<uint32_t>(std::stoul(
          argument.substr(std::string("--polling-interval=").size())));
    } else if (argument == "-s" || argument == "--storage") {
      MW_ASSERT(index + 1 < argc, "{} requires a storage ID.", argument);
      options.storage_id = argv[++index];
    } else if (argument.starts_with("--storage=")) {
      options.storage_id = argument.substr(std::string("--storage=").size());
    } else if (argument == "-f" || argument == "--serialization-format") {
      MW_ASSERT(index + 1 < argc, "{} requires a format.", argument);
      options.serialization_format = argv[++index];
    } else if (argument.starts_with("--serialization-format=")) {
      options.serialization_format = argument.substr(
          std::string("--serialization-format=").size());
    } else if (argument == "-b" || argument == "--max-bag-size") {
      MW_ASSERT(index + 1 < argc, "{} requires bytes.", argument);
      options.max_bag_size = std::stoull(argv[++index]);
    } else if (argument.starts_with("--max-bag-size=")) {
      options.max_bag_size = std::stoull(
          argument.substr(std::string("--max-bag-size=").size()));
    } else if (argument == "-d" || argument == "--max-bag-duration") {
      MW_ASSERT(index + 1 < argc, "{} requires seconds.", argument);
      options.max_bag_duration_seconds = std::stod(argv[++index]);
    } else if (argument.starts_with("--max-bag-duration=")) {
      options.max_bag_duration_seconds = std::stod(
          argument.substr(std::string("--max-bag-duration=").size()));
    } else if (argument == "--max-cache-size") {
      MW_ASSERT(index + 1 < argc, "{} requires bytes.", argument);
      options.max_cache_size = static_cast<size_t>(std::stoull(argv[++index]));
    } else if (argument.starts_with("--max-cache-size=")) {
      options.max_cache_size = static_cast<size_t>(std::stoull(
          argument.substr(std::string("--max-cache-size=").size())));
    } else if (argument == "--compression-mode") {
      MW_ASSERT(index + 1 < argc, "{} requires a mode.", argument);
      options.compression_mode = argv[++index];
    } else if (argument.starts_with("--compression-mode=")) {
      options.compression_mode = argument.substr(
          std::string("--compression-mode=").size());
    } else if (argument == "--compression-format") {
      MW_ASSERT(index + 1 < argc, "{} requires a format.", argument);
      options.compression_format = argv[++index];
    } else if (argument.starts_with("--compression-format=")) {
      options.compression_format = argument.substr(
          std::string("--compression-format=").size());
    } else if (argument == "--compression-queue-size") {
      MW_ASSERT(index + 1 < argc, "{} requires a count.", argument);
      options.compression_queue_size =
          static_cast<size_t>(std::stoull(argv[++index]));
    } else if (argument.starts_with("--compression-queue-size=")) {
      options.compression_queue_size = static_cast<size_t>(std::stoull(
          argument.substr(
              std::string("--compression-queue-size=").size())));
    } else if (argument == "--compression-threads") {
      MW_ASSERT(index + 1 < argc, "{} requires a count.", argument);
      options.compression_threads =
          static_cast<size_t>(std::stoull(argv[++index]));
    } else if (argument.starts_with("--compression-threads=")) {
      options.compression_threads = static_cast<size_t>(std::stoull(
          argument.substr(std::string("--compression-threads=").size())));
    } else if (argument == "--snapshot-mode") {
      options.snapshot_mode = true;
    } else if (argument == "--storage-preset-profile") {
      MW_ASSERT(index + 1 < argc, "{} requires a profile.", argument);
      options.storage_preset_profile = argv[++index];
    } else if (argument.starts_with("--storage-preset-profile=")) {
      options.storage_preset_profile = argument.substr(
          std::string("--storage-preset-profile=").size());
    } else if (argument == "--storage-config-file") {
      MW_ASSERT(index + 1 < argc, "{} requires a path.", argument);
      options.storage_config_file = argv[++index];
    } else if (argument.starts_with("--storage-config-file=")) {
      options.storage_config_file = argument.substr(
          std::string("--storage-config-file=").size());
    } else if (argument == "--start-paused") {
      options.start_paused = true;
    } else if (argument == "--use-sim-time") {
      options.use_sim_time = true;
    } else if (argument == "--log-level") {
      MW_ASSERT(index + 1 < argc, "{} requires a level.", argument);
      options.log_level = argv[++index];
    } else if (argument.starts_with("--log-level=")) {
      options.log_level =
          argument.substr(std::string("--log-level=").size());
    } else if (argument == "-h" || argument == "--help") {
      PrintBagUsage();
      options.help_requested = true;
      return options;
    } else if (!argument.empty() && argument.front() == '-') {
      MW_ASSERT(false, "Unsupported bag record option '{}'.", argument);
    } else {
      options.topics.emplace_back(argument);
    }
  }
  MW_ASSERT(
      !options.topics.empty() || options.all || !options.regex.empty(),
      "bag record requires topics, --all, or --regex.");
  MW_ASSERT(
      options.polling_interval_ms > 0,
      "Polling interval must be greater than zero.");
  MW_ASSERT(
      options.storage_id == "mcap" || options.storage_id == "sqlite3",
      "Storage must be mcap or sqlite3.");
  MW_ASSERT(
      !options.serialization_format.empty(),
      "Serialization format cannot be empty.");
  MW_ASSERT(
      options.max_bag_duration_seconds >= 0.0,
      "Max bag duration cannot be negative.");
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_mode == "file" ||
          options.compression_mode == "message",
      "Unsupported compression mode '{}'.", options.compression_mode);
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_format == "zstd" ||
          options.compression_format == "lz4",
      "Compression mode requires zstd or lz4 format.");
  MW_ASSERT(
      options.storage_id == "mcap" || options.compression_mode == "none",
      "Compression is only supported by MCAP storage.");
  MW_ASSERT(
      options.compression_queue_size > 0,
      "Compression queue size must be greater than zero.");
  MW_ASSERT(
      !options.snapshot_mode || options.max_cache_size > 0,
      "Snapshot mode requires a non-zero max cache size.");
  static const std::set<std::string> log_levels{
      "debug", "info", "warn", "error", "fatal"};
  MW_ASSERT(log_levels.contains(options.log_level), "Invalid log level.");
  MW_ASSERT(
      options.storage_config_file.empty() ||
          std::filesystem::is_regular_file(options.storage_config_file),
      "Storage config file does not exist.");
  if (options.output.empty()) {
    options.output = DefaultOutputPath();
  }
  return options;
}

InfoCliOptions ParseInfoOptions(int argc, const char* const* argv) {
  InfoCliOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-s" || argument == "--storage") {
      MW_ASSERT(index + 1 < argc, "{} requires a storage ID.", argument);
      options.storage_id = argv[++index];
    } else if (argument.starts_with("--storage=")) {
      options.storage_id =
          argument.substr(std::string("--storage=").size());
    } else if (argument == "-h" || argument == "--help") {
      PrintBagUsage();
      options.help_requested = true;
      return options;
    } else if (!argument.empty() && argument.front() == '-') {
      MW_ASSERT(false, "Unsupported bag info option '{}'.", argument);
    } else {
      MW_ASSERT(options.bag_path.empty(), "bag info accepts one bag path.");
      options.bag_path = argument;
    }
  }
  MW_ASSERT(!options.bag_path.empty(), "bag info requires a bag path.");
  return options;
}

double ParseDouble(std::string_view value, std::string_view option) {
  size_t consumed = 0;
  double result = 0.0;
  try {
    result = std::stod(std::string(value), &consumed);
  } catch (const std::invalid_argument&) {
    MW_ASSERT(false, "{} requires a numeric value.", option);
  } catch (const std::out_of_range&) {
    MW_ASSERT(false, "{} value is outside the supported range.", option);
  }
  MW_ASSERT(
      consumed == value.size() && std::isfinite(result),
      "{} requires a finite numeric value.", option);
  return result;
}

uint64_t ParseDurationNanoseconds(double seconds, std::string_view option) {
  MW_ASSERT(
      std::isfinite(seconds) && seconds >= 0.0,
      "{} must be finite and non-negative.", option);
  const long double nanoseconds = static_cast<long double>(seconds) * 1.0e9L;
  MW_ASSERT(
      nanoseconds <=
          static_cast<long double>(std::numeric_limits<uint64_t>::max()),
      "{} is too large.", option);
  return static_cast<uint64_t>(nanoseconds);
}

size_t ParseSize(std::string_view value, std::string_view option) {
  size_t consumed = 0;
  const auto result = std::stoull(std::string(value), &consumed);
  MW_ASSERT(
      consumed == value.size() && result <= std::numeric_limits<size_t>::max(),
      "{} requires a valid size.", option);
  return static_cast<size_t>(result);
}

void ParseRemap(std::string_view value, PlayCliOptions& options) {
  const auto separator = value.find(":=");
  MW_ASSERT(
      separator != std::string_view::npos && separator != 0 &&
          separator + 2 < value.size(),
      "Remap '{}' must use from:=to syntax.", value);
  const std::string from(value.substr(0, separator));
  const std::string to(value.substr(separator + 2));
  MW_ASSERT(
      options.remaps.emplace(from, to).second,
      "Topic '{}' is remapped more than once.", from);
}

PlayCliOptions ParsePlayOptions(int argc, const char* const* argv) {
  PlayCliOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-s" || argument == "--storage") {
      MW_ASSERT(index + 1 < argc, "{} requires a storage ID.", argument);
      options.storage_id = argv[++index];
    } else if (argument.starts_with("--storage=")) {
      options.storage_id =
          argument.substr(std::string("--storage=").size());
    } else if (argument == "--read-ahead-queue-size") {
      MW_ASSERT(index + 1 < argc, "{} requires a count.", argument);
      options.read_ahead_queue_size = ParseSize(argv[++index], argument);
    } else if (argument.starts_with("--read-ahead-queue-size=")) {
      options.read_ahead_queue_size = ParseSize(
          argument.substr(std::string("--read-ahead-queue-size=").size()),
          "--read-ahead-queue-size");
    } else if (argument == "-r" || argument == "--rate") {
      MW_ASSERT(index + 1 < argc, "{} requires a factor.", argument);
      options.rate = ParseDouble(argv[++index], argument);
    } else if (argument.starts_with("--rate=")) {
      options.rate = ParseDouble(
          argument.substr(std::string("--rate=").size()), "--rate");
    } else if (argument == "--topics") {
      MW_ASSERT(!options.bag_path.empty(), "Place the bag path before --topics.");
      const size_t previous_size = options.topics.size();
      while (index + 1 < argc && argv[index + 1][0] != '-') {
        options.topics.emplace_back(argv[++index]);
      }
      MW_ASSERT(
          options.topics.size() != previous_size,
          "--topics requires at least one topic.");
    } else if (argument == "-l" || argument == "--loop") {
      options.loop = true;
    } else if (argument == "-m" || argument == "--remap") {
      MW_ASSERT(index + 1 < argc, "{} requires from:=to.", argument);
      ParseRemap(argv[++index], options);
    } else if (argument.starts_with("--remap=")) {
      ParseRemap(
          argument.substr(std::string("--remap=").size()), options);
    } else if (argument == "-d" || argument == "--delay") {
      MW_ASSERT(index + 1 < argc, "{} requires seconds.", argument);
      options.delay_seconds = ParseDouble(argv[++index], argument);
    } else if (argument.starts_with("--delay=")) {
      options.delay_seconds = ParseDouble(
          argument.substr(std::string("--delay=").size()), "--delay");
    } else if (argument == "--disable-keyboard-controls") {
      options.disable_keyboard_controls = true;
    } else if (argument == "-p" || argument == "--start-paused") {
      options.start_paused = true;
    } else if (argument == "--start-offset") {
      MW_ASSERT(index + 1 < argc, "{} requires seconds.", argument);
      options.start_offset_seconds = ParseDouble(argv[++index], argument);
    } else if (argument.starts_with("--start-offset=")) {
      options.start_offset_seconds = ParseDouble(
          argument.substr(std::string("--start-offset=").size()),
          "--start-offset");
    } else if (argument == "--config-file") {
      MW_ASSERT(index + 1 < argc, "{} requires a path.", argument);
      options.config_file = argv[++index];
    } else if (argument.starts_with("--config-file=")) {
      options.config_file =
          argument.substr(std::string("--config-file=").size());
    } else if (argument == "--storage-config-file") {
      MW_ASSERT(index + 1 < argc, "{} requires a path.", argument);
      options.storage_config_file = argv[++index];
    } else if (argument.starts_with("--storage-config-file=")) {
      options.storage_config_file = argument.substr(
          std::string("--storage-config-file=").size());
    } else if (argument == "--wait-for-all-acked") {
      MW_ASSERT(index + 1 < argc, "{} requires seconds.", argument);
      options.wait_for_all_acked_seconds = ParseDouble(argv[++index], argument);
    } else if (argument.starts_with("--wait-for-all-acked=")) {
      options.wait_for_all_acked_seconds = ParseDouble(
          argument.substr(std::string("--wait-for-all-acked=").size()),
          "--wait-for-all-acked");
    } else if (argument == "--log-level") {
      MW_ASSERT(index + 1 < argc, "{} requires a level.", argument);
      options.log_level = argv[++index];
    } else if (argument.starts_with("--log-level=")) {
      options.log_level =
          argument.substr(std::string("--log-level=").size());
    } else if (argument == "-h" || argument == "--help") {
      PrintBagUsage();
      options.help_requested = true;
      return options;
    } else if (!argument.empty() && argument.front() == '-') {
      MW_ASSERT(false, "Unsupported bag play option '{}'.", argument);
    } else {
      MW_ASSERT(options.bag_path.empty(), "bag play accepts one bag path.");
      options.bag_path = argument;
    }
  }
  MW_ASSERT(!options.bag_path.empty(), "bag play requires a bag path.");
  MW_ASSERT(options.read_ahead_queue_size > 0, "Read-ahead queue cannot be zero.");
  MW_ASSERT(
      std::isfinite(options.rate) && options.rate > 0.0 &&
          options.rate <= 1.0e9,
      "Playback rate must be finite, greater than zero, and no greater than 1e9.");
  MW_ASSERT(
      std::isfinite(options.delay_seconds) && options.delay_seconds >= 0.0,
      "Playback delay must be finite and non-negative.");
  MW_ASSERT(
      std::isfinite(options.start_offset_seconds) &&
          options.start_offset_seconds >= 0.0,
      "Playback start offset must be finite and non-negative.");
  MW_ASSERT(
      options.wait_for_all_acked_seconds >= -1.0,
      "Acknowledgment timeout cannot be less than -1.");
  static const std::set<std::string> log_levels{
      "debug", "info", "warn", "error", "fatal"};
  MW_ASSERT(
      log_levels.contains(options.log_level),
      "Unsupported log level '{}'.", options.log_level);
  MW_ASSERT(
      !options.config_file.empty(), "bag play requires --config-file.");
  MW_ASSERT(
      std::filesystem::is_regular_file(options.config_file),
      "Player config file does not exist.");
  MW_ASSERT(
      options.storage_config_file.empty() ||
          std::filesystem::is_regular_file(options.storage_config_file),
      "Storage config file does not exist.");
  return options;
}

ConvertCliOptions ParseConvertOptions(int argc, const char* const* argv) {
  ConvertCliOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-i" || argument == "--input") {
      MW_ASSERT(index + 1 < argc, "{} requires a bag path.", argument);
      MW_ASSERT(options.input.empty(), "bag convert accepts one input bag.");
      options.input = argv[++index];
    } else if (argument.starts_with("--input=")) {
      MW_ASSERT(options.input.empty(), "bag convert accepts one input bag.");
      options.input = argument.substr(std::string("--input=").size());
    } else if (argument == "-o" || argument == "--output") {
      MW_ASSERT(index + 1 < argc, "{} requires a bag path.", argument);
      MW_ASSERT(options.output.empty(), "bag convert accepts one output bag.");
      options.output = argv[++index];
    } else if (argument.starts_with("--output=")) {
      MW_ASSERT(options.output.empty(), "bag convert accepts one output bag.");
      options.output = argument.substr(std::string("--output=").size());
    } else if (argument == "--input-format") {
      MW_ASSERT(index + 1 < argc, "{} requires a format.", argument);
      options.input_format = argv[++index];
    } else if (argument.starts_with("--input-format=")) {
      options.input_format =
          argument.substr(std::string("--input-format=").size());
    } else if (argument == "--output-format") {
      MW_ASSERT(index + 1 < argc, "{} requires a format.", argument);
      options.output_format = argv[++index];
    } else if (argument.starts_with("--output-format=")) {
      options.output_format =
          argument.substr(std::string("--output-format=").size());
    } else if (argument == "-s" || argument == "--storage") {
      MW_ASSERT(index + 1 < argc, "{} requires a storage ID.", argument);
      options.storage_id = argv[++index];
    } else if (argument.starts_with("--storage=")) {
      options.storage_id = argument.substr(std::string("--storage=").size());
    } else if (argument == "--input-storage") {
      MW_ASSERT(index + 1 < argc, "{} requires a storage ID.", argument);
      options.input_storage_id = argv[++index];
    } else if (argument.starts_with("--input-storage=")) {
      options.input_storage_id = argument.substr(
          std::string("--input-storage=").size());
    } else if (argument == "-b" || argument == "--max-bag-size") {
      MW_ASSERT(index + 1 < argc, "{} requires bytes.", argument);
      options.max_bag_size = std::stoull(argv[++index]);
    } else if (argument.starts_with("--max-bag-size=")) {
      options.max_bag_size = std::stoull(
          argument.substr(std::string("--max-bag-size=").size()));
    } else if (argument == "-d" || argument == "--max-bag-duration") {
      MW_ASSERT(index + 1 < argc, "{} requires seconds.", argument);
      options.max_bag_duration_seconds = ParseDouble(argv[++index], argument);
    } else if (argument.starts_with("--max-bag-duration=")) {
      options.max_bag_duration_seconds = ParseDouble(
          argument.substr(std::string("--max-bag-duration=").size()),
          "--max-bag-duration");
    } else if (argument == "--compression-mode") {
      MW_ASSERT(index + 1 < argc, "{} requires a mode.", argument);
      options.compression_mode = argv[++index];
    } else if (argument.starts_with("--compression-mode=")) {
      options.compression_mode = argument.substr(
          std::string("--compression-mode=").size());
    } else if (argument == "--compression-format") {
      MW_ASSERT(index + 1 < argc, "{} requires a format.", argument);
      options.compression_format = argv[++index];
    } else if (argument.starts_with("--compression-format=")) {
      options.compression_format = argument.substr(
          std::string("--compression-format=").size());
    } else if (argument == "--compression-queue-size") {
      MW_ASSERT(index + 1 < argc, "{} requires a count.", argument);
      options.compression_queue_size = ParseSize(argv[++index], argument);
    } else if (argument.starts_with("--compression-queue-size=")) {
      options.compression_queue_size = ParseSize(
          argument.substr(std::string("--compression-queue-size=").size()),
          "--compression-queue-size");
    } else if (argument == "--compression-threads") {
      MW_ASSERT(index + 1 < argc, "{} requires a count.", argument);
      options.compression_threads = ParseSize(argv[++index], argument);
    } else if (argument.starts_with("--compression-threads=")) {
      options.compression_threads = ParseSize(
          argument.substr(std::string("--compression-threads=").size()),
          "--compression-threads");
    } else if (argument == "--storage-preset-profile") {
      MW_ASSERT(index + 1 < argc, "{} requires a profile.", argument);
      options.storage_preset_profile = argv[++index];
    } else if (argument.starts_with("--storage-preset-profile=")) {
      options.storage_preset_profile = argument.substr(
          std::string("--storage-preset-profile=").size());
    } else if (argument == "--input-storage-config-file") {
      MW_ASSERT(index + 1 < argc, "{} requires a path.", argument);
      options.input_storage_config_file = argv[++index];
    } else if (argument.starts_with("--input-storage-config-file=")) {
      options.input_storage_config_file = argument.substr(
          std::string("--input-storage-config-file=").size());
    } else if (argument == "--storage-config-file") {
      MW_ASSERT(index + 1 < argc, "{} requires a path.", argument);
      options.storage_config_file = argv[++index];
    } else if (argument.starts_with("--storage-config-file=")) {
      options.storage_config_file = argument.substr(
          std::string("--storage-config-file=").size());
    } else if (argument == "-h" || argument == "--help") {
      PrintBagUsage();
      options.help_requested = true;
      return options;
    } else if (!argument.empty() && argument.front() == '-') {
      MW_ASSERT(false, "Unsupported bag convert option '{}'.", argument);
    } else {
      MW_ASSERT(options.input.empty(), "bag convert accepts one input bag.");
      options.input = argument;
    }
  }

  MW_ASSERT(!options.input.empty(), "bag convert requires an input bag.");
  MW_ASSERT(!options.output.empty(), "bag convert requires --output.");
  MW_ASSERT(
      options.storage_id == "mcap" || options.storage_id == "sqlite3",
      "Output storage must be mcap or sqlite3.");
  MW_ASSERT(
      options.input_format == "auto" || options.input_format == "ibmw" ||
          options.input_format == "rosbag2",
      "Input format must be auto, ibmw, or rosbag2.");
  MW_ASSERT(
      options.output_format == "ibmw" || options.output_format == "rosbag2",
      "Output format must be ibmw or rosbag2.");
  MW_ASSERT(
      options.input_storage_id.empty() ||
          options.input_storage_id == "mcap" ||
          options.input_storage_id == "sqlite3",
      "Input storage must be mcap or sqlite3.");
  MW_ASSERT(
      std::isfinite(options.max_bag_duration_seconds) &&
          options.max_bag_duration_seconds >= 0.0,
      "Max bag duration must be finite and non-negative.");
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_mode == "file" ||
          options.compression_mode == "message",
      "Unsupported compression mode '{}'.", options.compression_mode);
  MW_ASSERT(
      options.compression_mode == "none" || options.storage_id == "mcap",
      "Compression is only supported for MCAP output.");
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_format == "zstd" ||
          options.compression_format == "lz4",
      "Compression requires zstd or lz4.");
  MW_ASSERT(
      options.compression_mode != "none" || options.compression_format.empty(),
      "Compression format requires a compression mode.");
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_queue_size > 0,
      "Compression queue size cannot be zero.");
  MW_ASSERT(
      options.input_storage_config_file.empty() ||
          std::filesystem::is_regular_file(options.input_storage_config_file),
      "Input storage config file does not exist.");
  MW_ASSERT(
      options.storage_config_file.empty() ||
          std::filesystem::is_regular_file(options.storage_config_file),
      "Output storage config file does not exist.");
  return options;
}

uint32_t ReadClockUint32(const char* data, bool little_endian) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  if (little_endian) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
  }
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

std::optional<uint64_t> ExtractSimTime(
    const ibmw::transport::SerializedMessage& message) {
  if (message.payload == nullptr) {
    return std::nullopt;
  }
  const std::string payload = message.payload->JoinToString();
  if (message.topic.serialization_type == "json") {
    try {
      const auto root = nlohmann::json::parse(payload);
      const auto& clock = root.contains("clock") ? root.at("clock") : root;
      if (clock.contains("nanoseconds")) {
        return clock.at("nanoseconds").get<uint64_t>();
      }
      if (clock.contains("timestamp")) {
        return clock.at("timestamp").get<uint64_t>();
      }
      const int64_t seconds = clock.at("sec").get<int64_t>();
      const uint64_t nanoseconds = clock.at("nanosec").get<uint64_t>();
      if (seconds < 0 || nanoseconds >= 1000000000ull) {
        return std::nullopt;
      }
      return static_cast<uint64_t>(seconds) * 1000000000ull + nanoseconds;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  if ((message.topic.serialization_type == "cdr" ||
       message.topic.serialization_type == "ros2") &&
      payload.size() >= 12) {
    const uint16_t encapsulation =
        (static_cast<uint16_t>(static_cast<unsigned char>(payload[0])) << 8) |
        static_cast<unsigned char>(payload[1]);
    const bool little_endian = encapsulation == 0x0001 ||
                               encapsulation == 0x0003;
    const int32_t seconds = static_cast<int32_t>(
        ReadClockUint32(payload.data() + 4, little_endian));
    const uint32_t nanoseconds =
        ReadClockUint32(payload.data() + 8, little_endian);
    if (seconds < 0 || nanoseconds >= 1000000000u) {
      return std::nullopt;
    }
    return static_cast<uint64_t>(seconds) * 1000000000ull + nanoseconds;
  }
  return std::nullopt;
}

struct RecorderRuntimeSpec {
  std::map<std::string, TransportCaptureCapability> direct_drivers;
  std::set<std::string> type_support_packages;
};

nlohmann::json CanonicalYamlValue(const YAML::Node& node) {
  if (!node || node.IsNull()) {
    return nullptr;
  }
  if (node.IsScalar()) {
    return node.as<std::string>();
  }
  if (node.IsSequence()) {
    auto result = nlohmann::json::array();
    for (const auto& item : node) {
      result.push_back(CanonicalYamlValue(item));
    }
    return result;
  }
  MW_ASSERT(node.IsMap(), "Unsupported YAML node in Recorder options.");
  auto result = nlohmann::json::object();
  for (const auto& item : node) {
    result[item.first.as<std::string>()] = CanonicalYamlValue(item.second);
  }
  return result;
}

YAML::Node ParseOptionsYaml(std::string_view yaml) {
  if (yaml.empty()) {
    return YAML::Node(YAML::NodeType::Map);
  }
  auto result = YAML::Load(std::string(yaml));
  if (!result || result.IsNull()) {
    return YAML::Node(YAML::NodeType::Map);
  }
  MW_ASSERT(result.IsMap(), "Recorder connection options must be a YAML map.");
  return result;
}

std::string CanonicalDriverOptions(std::string_view yaml) {
  return CanonicalYamlValue(ParseOptionsYaml(yaml)).dump();
}

std::string CanonicalExtensionOptions(
    const TransportCaptureCapability& capability) {
  auto options = ParseOptionsYaml(capability.extension_options_yaml);
  if (!capability.provider_instance_option.empty()) {
    options.remove(capability.provider_instance_option);
  }
  if (!capability.provider_clear_option.empty()) {
    options.remove(capability.provider_clear_option);
  }
  return CanonicalYamlValue(options).dump();
}

bool CompatibleRecorderConnection(
    const TransportCaptureCapability& lhs,
    const TransportCaptureCapability& rhs) {
  return lhs.mode == rhs.mode && lhs.priority == rhs.priority &&
         lhs.extension_name == rhs.extension_name &&
         lhs.extension_path == rhs.extension_path &&
         lhs.provider_instance_option == rhs.provider_instance_option &&
         lhs.provider_clear_option == rhs.provider_clear_option &&
         lhs.provider_strip_leading_slash_option ==
             rhs.provider_strip_leading_slash_option &&
         CanonicalExtensionOptions(lhs) == CanonicalExtensionOptions(rhs) &&
         CanonicalDriverOptions(lhs.driver_options_yaml) ==
             CanonicalDriverOptions(rhs.driver_options_yaml);
}

RecorderRuntimeSpec BuildRecorderRuntimeSpec(
    const BagTopicGraph& graph,
    const std::vector<std::string>& topics,
    bool include_cached_serialized = true) {
  RecorderRuntimeSpec spec;
  std::set<std::string> incompatible_direct_drivers;
  for (const auto& topic : topics) {
    for (const auto& advertisement : graph.Find(topic)) {
      for (const auto& [driver, capability] :
           advertisement.capture_capabilities) {
        if (capability.mode != "native_serialized" &&
            (!include_cached_serialized ||
             capability.mode != "cached_serialized")) {
          continue;
        }
        if (capability.abi_version !=
            runtime::core::transport::SerializedTransportCapabilityV1::
                kAbiVersion) {
          continue;
        }
        if (incompatible_direct_drivers.contains(driver)) {
          continue;
        }
        const auto [found, inserted] =
            spec.direct_drivers.emplace(driver, capability);
        if (!inserted &&
            !CompatibleRecorderConnection(found->second, capability)) {
          spec.direct_drivers.erase(found);
          incompatible_direct_drivers.emplace(driver);
          std::cerr
              << "Recorder direct connection configuration conflicts for "
              << "driver '" << driver
              << "'; falling back to Runtime playback/tap routes.\n";
          continue;
        }
        if (capability.mode == "cached_serialized") {
          MW_ASSERT(
              !advertisement.type_support_package.empty(),
              "Driver '{}' requires an advertised TypeSupport package for '{}'.",
              driver, advertisement.topic.msg_type);
          spec.type_support_packages.emplace(
              advertisement.type_support_package);
        }
      }
    }
  }
  return spec;
}

std::filesystem::path WriteRecorderConfig(
    const RecorderRuntimeSpec& spec,
    std::string_view role = "recorder",
    std::string log_level = "info") {
  YAML::Node root;
  YAML::Node ibmw = root["ibmw"];
  std::map<std::string, TransportCaptureCapability> extensions;
  for (const auto& [driver, capability] : spec.direct_drivers) {
    static_cast<void>(driver);
    MW_ASSERT(
        !capability.extension_name.empty() &&
            !capability.extension_path.empty(),
        "Direct Recorder driver is missing extension configuration.");
    const auto [found, inserted] =
        extensions.emplace(capability.extension_name, capability);
    if (!inserted) {
      MW_ASSERT(
          found->second.extension_path == capability.extension_path &&
              found->second.provider_instance_option ==
                  capability.provider_instance_option &&
              found->second.provider_clear_option ==
                  capability.provider_clear_option &&
              found->second.provider_strip_leading_slash_option ==
                  capability.provider_strip_leading_slash_option &&
              CanonicalExtensionOptions(found->second) ==
                  CanonicalExtensionOptions(capability),
          "Conflicting Recorder extension configuration for '{}'.",
          capability.extension_name);
    }
  }
  for (const auto& [extension_name, capability] : extensions) {
    YAML::Node options = capability.extension_options_yaml.empty()
                             ? YAML::Node(YAML::NodeType::Map)
                             : YAML::Load(capability.extension_options_yaml);
    if (!options || options.IsNull()) {
      options = YAML::Node(YAML::NodeType::Map);
    }
    const std::string unique = std::to_string(::getpid());
    if (!capability.provider_instance_option.empty()) {
      const auto key = capability.provider_instance_option;
      options[std::string(key)] =
          "ibmw_bag_" + std::string(role) + "_" + unique;
    }
    if (!capability.provider_clear_option.empty()) {
      options[capability.provider_clear_option] = "";
    }
    if (!capability.provider_strip_leading_slash_option.empty() &&
        options[capability.provider_strip_leading_slash_option]) {
      std::string domain =
          options[capability.provider_strip_leading_slash_option]
              .as<std::string>();
      while (!domain.empty() && domain.front() == '/') {
        domain.erase(domain.begin());
      }
      options[capability.provider_strip_leading_slash_option] = domain;
    }
    YAML::Node extension;
    extension["name"] = extension_name;
    extension["path"] = capability.extension_path;
    extension["options"] = options;
    ibmw["extensions"]["items"].push_back(extension);
  }
  std::transform(
      log_level.begin(), log_level.end(), log_level.begin(),
      [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
  ibmw["logging"]["core_level"] = log_level;
  YAML::Node backend;
  backend["type"] = "console";
  ibmw["logging"]["backends"].push_back(backend);
  for (const auto& [driver_name, capability] : spec.direct_drivers) {
    YAML::Node driver;
    driver["type"] = driver_name;
    if (!capability.driver_options_yaml.empty()) {
      driver["options"] = YAML::Load(capability.driver_options_yaml);
    }
    ibmw["messaging"]["drivers"].push_back(driver);
  }
  const auto config_path = std::filesystem::temp_directory_path() /
                           ("ibmw-bag-" + std::string(role) + "-" +
                            std::to_string(::getpid()) + ".yaml");
  std::ofstream output(config_path, std::ios::trunc);
  MW_ASSERT(output.is_open(), "Cannot write temporary Recorder config.");
  output << root;
  output.close();
  MW_ASSERT(output.good(), "Failed to write temporary Recorder config.");
  return config_path;
}

using PlaybackDriverSet = std::set<std::string>;

struct PlaybackRoute {
  PlaybackDriverSet drivers;
  std::optional<std::string> runtime_id;
};

using PlaybackRouteMap = std::unordered_map<std::string, PlaybackRoute>;

struct ResolvedPlaybackConfig {
  PlaybackRouteMap routes;
  std::filesystem::path runtime_registry_path = "/tmp/ibmw/runtimes";
};

std::string PlaybackTopicKey(
    const ibmw::transport::TopicDescriptor& topic) {
  return topic.topic + '\0' + topic.msg_type + '\0' +
         topic.serialization_type;
}

PlaybackDriverSet ResolvePlaybackDrivers(
    const YAML::Node& publishers,
    std::string_view topic) {
  if (!publishers || publishers.IsNull()) {
    return {};
  }
  MW_ASSERT(
      publishers.IsSequence(),
      "ibmw.messaging.publishers must be a sequence.");
  for (const auto& publisher : publishers) {
    MW_ASSERT(
        publisher.IsMap() && publisher["topic"] &&
            publisher["enable_drivers"],
        "Each playback publisher rule requires topic and enable_drivers.");
    const std::string expression = publisher["topic"].as<std::string>();
    try {
      if (!std::regex_match(
              topic.begin(), topic.end(),
              std::regex(expression, std::regex::ECMAScript))) {
        continue;
      }
    } catch (const std::exception& error) {
      MW_ASSERT(
          false, "Invalid playback publisher regex '{}': {}.",
          expression, error.what());
    }
    const auto configured =
        publisher["enable_drivers"].as<std::vector<std::string>>();
    return PlaybackDriverSet(configured.begin(), configured.end());
  }
  return {};
}

std::optional<std::string> ResolvePlaybackRuntime(
    const YAML::Node& runtime_routes,
    std::string_view topic) {
  if (!runtime_routes || runtime_routes.IsNull()) {
    return std::nullopt;
  }
  MW_ASSERT(
      runtime_routes.IsSequence(),
      "ibmw.bag_playback.runtime_routes must be a sequence.");
  for (const auto& route : runtime_routes) {
    MW_ASSERT(
        route.IsMap() && route["topic"] && route["runtime_id"],
        "Each Runtime playback route requires topic and runtime_id.");
    const std::string expression = route["topic"].as<std::string>();
    try {
      if (!std::regex_match(
              topic.begin(), topic.end(),
              std::regex(expression, std::regex::ECMAScript))) {
        continue;
      }
    } catch (const std::exception& error) {
      MW_ASSERT(
          false, "Invalid Runtime playback regex '{}': {}.",
          expression, error.what());
    }
    const std::string runtime_id = route["runtime_id"].as<std::string>();
    MW_ASSERT(
        !runtime_id.empty(),
        "Runtime playback route '{}' has an empty runtime_id.", expression);
    return runtime_id;
  }
  return std::nullopt;
}

ibmw::transport::QosDescriptor MergePlaybackQos(
    ibmw::transport::QosDescriptor recorded,
    const ibmw::transport::QosDescriptor& configured) {
  for (const auto& [policy, value] : configured.policies) {
    recorded.policies[policy] = value;
  }
  return recorded;
}

ResolvedPlaybackConfig ResolvePlaybackConfig(
    const std::filesystem::path& config_path,
    const std::vector<BagChannelInfo>& channels) {
  ResolvedPlaybackConfig result;
  const YAML::Node root = YAML::LoadFile(config_path.string());
  MW_ASSERT(
      root.IsMap() && root["ibmw"] && root["ibmw"].IsMap(),
      "Player config must contain an 'ibmw' map.");
  const YAML::Node ibmw = root["ibmw"];
  const YAML::Node messaging = ibmw["messaging"];
  MW_ASSERT(
      !messaging.IsDefined() || messaging.IsNull() || messaging.IsMap(),
      "ibmw.messaging must be a map.");
  const bool has_messaging = messaging.IsDefined() && !messaging.IsNull();
  const YAML::Node publishers =
      has_messaging ? messaging["publishers"] : YAML::Node{};
  const YAML::Node bag_playback = ibmw["bag_playback"];
  MW_ASSERT(
      !bag_playback.IsDefined() || bag_playback.IsNull() ||
          bag_playback.IsMap(),
      "ibmw.bag_playback must be a map.");
  const YAML::Node runtime_routes =
      bag_playback.IsDefined() && !bag_playback.IsNull()
          ? bag_playback["runtime_routes"]
          : YAML::Node{};
  const YAML::Node runtime_registry_path =
      bag_playback.IsDefined() && !bag_playback.IsNull()
          ? bag_playback["runtime_registry_path"]
          : YAML::Node{};
  if (runtime_registry_path.IsDefined() &&
      !runtime_registry_path.IsNull()) {
    result.runtime_registry_path =
        runtime_registry_path.as<std::string>();
    MW_ASSERT(
        !result.runtime_registry_path.empty(),
        "ibmw.bag_playback.runtime_registry_path cannot be empty.");
  }
  for (const auto& channel : channels) {
    auto drivers = ResolvePlaybackDrivers(
        publishers, channel.topic.topic);
    MW_ASSERT(
        !drivers.contains("local"),
        "Playback topic '{}' cannot use the local driver directly; configure "
        "ibmw.bag_playback.runtime_routes with a runtime_id.",
        channel.topic.topic);
    auto runtime_id = ResolvePlaybackRuntime(
        runtime_routes, channel.topic.topic);
    MW_ASSERT(
        !drivers.empty() || runtime_id.has_value(),
        "Player config has no route for topic '{}'.", channel.topic.topic);
    result.routes.try_emplace(
        PlaybackTopicKey(channel.topic),
        PlaybackRoute{
            .drivers = std::move(drivers),
            .runtime_id = std::move(runtime_id)});
  }
  return result;
}

std::string ExactTopicRegex(std::string_view topic) {
  constexpr std::string_view special = R"(\.^$|()[]*+?{})";
  std::string result;
  result.reserve(topic.size() * 2);
  for (const char value : topic) {
    if (special.find(value) != std::string_view::npos) {
      result.push_back('\\');
    }
    result.push_back(value);
  }
  return result;
}

std::filesystem::path WritePlayerConfig(
    const std::filesystem::path& source_path,
    const std::vector<BagChannelInfo>& channels,
    const PlaybackRouteMap& routes,
    std::string log_level) {
  const YAML::Node source = YAML::LoadFile(source_path.string());
  const YAML::Node source_ibmw = source["ibmw"];

  YAML::Node root;
  YAML::Node ibmw = root["ibmw"];
  for (const std::string_view key : {
           "config", "extensions", "main_loop", "watchdog",
           "scheduling", "logging", "memory"}) {
    if (source_ibmw[std::string(key)]) {
      ibmw[std::string(key)] =
          YAML::Clone(source_ibmw[std::string(key)]);
    }
  }
  if (ibmw["extensions"] && ibmw["extensions"]["items"]) {
    for (auto extension : ibmw["extensions"]["items"]) {
      if (!extension["path"]) {
        continue;
      }
      std::filesystem::path path = extension["path"].as<std::string>();
      if (path.is_relative() && path.has_parent_path()) {
        extension["path"] =
            std::filesystem::absolute(source_path.parent_path() / path)
                .lexically_normal()
                .string();
      }
    }
  }
  std::transform(
      log_level.begin(), log_level.end(), log_level.begin(),
      [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
  ibmw["logging"]["core_level"] = log_level;

  YAML::Node messaging = YAML::Clone(source_ibmw["messaging"]);
  messaging.remove("subscribers");
  messaging["publishers"] = YAML::Node(YAML::NodeType::Sequence);
  for (const auto& channel : channels) {
    const auto route = routes.find(PlaybackTopicKey(channel.topic));
    if (route == routes.end()) {
      continue;
    }
    PlaybackDriverSet bus_drivers = route->second.drivers;
    if (bus_drivers.empty()) {
      continue;
    }
    YAML::Node publisher;
    publisher["topic"] = ExactTopicRegex(channel.topic.topic);
    publisher["enable_drivers"] =
        std::vector<std::string>(bus_drivers.begin(), bus_drivers.end());
    messaging["publishers"].push_back(publisher);
  }
  if (messaging["publishers"].size() == 0) {
    messaging.remove("publishers");
  }

  ibmw["messaging"] = messaging;

  const auto config_path = std::filesystem::temp_directory_path() /
                           ("ibmw-bag-player-" +
                            std::to_string(::getpid()) + ".yaml");
  std::ofstream output(config_path, std::ios::trunc);
  MW_ASSERT(output.is_open(), "Cannot write temporary Player config.");
  output << root;
  output.close();
  MW_ASSERT(output.good(), "Failed to write temporary Player config.");
  return config_path;
}

class CliTypeSupportRegistry {
 public:
  void Load(const std::string& path) {
    std::lock_guard lock(mutex_);
    if (loaded_paths_.contains(path)) {
      return;
    }
    auto loader =
        std::make_unique<runtime::core::util::TypeSupportPkgLoader>();
    loader->LoadTypeSupportPkg(path);
    for (const auto* item : loader->GetTypeSupportArray()) {
      ibmw::util::TypeSupportRef type_support(item);
      type_supports_.try_emplace(
          std::string(type_support.TypeName()), type_support);
      type_support_packages_.try_emplace(
          std::string(type_support.TypeName()), path);
    }
    for (const auto* item : loader->GetSchemaSupportArray()) {
      ibmw::util::SchemaSupportRef schema(item);
      schemas_.try_emplace(
          std::string(schema.TypeName()) + '\0' +
              std::string(schema.SerializationType()),
          schema.ToDescriptor());
    }
    loaded_paths_.emplace(path);
    loaders_.emplace_back(std::move(loader));
  }

  ibmw::util::TypeSupportRef Find(std::string_view type_name) const {
    std::lock_guard lock(mutex_);
    const auto found = type_supports_.find(std::string(type_name));
    return found == type_supports_.end() ? ibmw::util::TypeSupportRef{}
                                        : found->second;
  }

  std::string PackagePath(std::string_view type_name) const {
    std::lock_guard lock(mutex_);
    const auto found = type_support_packages_.find(std::string(type_name));
    return found == type_support_packages_.end() ? std::string{}
                                                 : found->second;
  }

  std::optional<ibmw::util::SchemaDescriptor> FindSchema(
      std::string_view type_name,
      std::string_view serialization_type) const {
    std::lock_guard lock(mutex_);
    const auto found = schemas_.find(
        std::string(type_name) + '\0' + std::string(serialization_type));
    return found == schemas_.end()
               ? std::nullopt
               : std::optional<ibmw::util::SchemaDescriptor>(found->second);
  }

 private:
  std::set<std::string> loaded_paths_;
  std::vector<std::unique_ptr<runtime::core::util::TypeSupportPkgLoader>>
      loaders_;
  std::unordered_map<std::string, ibmw::util::TypeSupportRef> type_supports_;
  std::unordered_map<std::string, std::string> type_support_packages_;
  std::unordered_map<std::string, ibmw::util::SchemaDescriptor> schemas_;
  mutable std::mutex mutex_;
};

void LoadPlayerTypeSupportPackages(
    const std::filesystem::path& config_path,
    CliTypeSupportRegistry& registry) {
  if (config_path.empty()) {
    return;
  }
  const YAML::Node root = YAML::LoadFile(config_path.string());
  const YAML::Node packages = root["ibmw"] && root["ibmw"]["bag_playback"]
                                  ? root["ibmw"]["bag_playback"]
                                        ["type_support_pkgs"]
                                  : YAML::Node{};
  if (!packages) {
    return;
  }
  MW_ASSERT(
      packages.IsSequence(),
      "ibmw.bag_playback.type_support_pkgs must be a sequence.");
  for (const auto& package : packages) {
    std::filesystem::path path = package.IsScalar()
                                     ? package.as<std::string>()
                                     : package["path"].as<std::string>();
    MW_ASSERT(!path.empty(), "Player TypeSupport path cannot be empty.");
    if (path.is_relative()) {
      path = config_path.parent_path() / path;
    }
    registry.Load(path.lexically_normal().string());
  }
}

std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(
    std::string payload) {
  auto storage = std::make_shared<std::string>(std::move(payload));
  auto* view = new ibmw::util::BufferArrayView(
      storage->data(), storage->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view,
      [storage](const ibmw::util::BufferArrayView* ptr) { delete ptr; });
}

bool CanTranscodePlayback(
    CliTypeSupportRegistry& registry,
    const BagChannelInfo& channel,
    const TopicAdvertisement& advertisement) {
  if (channel.topic.msg_type != advertisement.topic.msg_type ||
      channel.topic.serialization_type ==
          advertisement.topic.serialization_type ||
      advertisement.type_support_package.empty()) {
    return false;
  }
  try {
    registry.Load(advertisement.type_support_package);
    const auto type_support = registry.Find(channel.topic.msg_type);
    return type_support &&
           type_support.CheckSerializationTypeSupported(
               channel.topic.serialization_type) &&
           type_support.CheckSerializationTypeSupported(
               advertisement.topic.serialization_type) &&
           advertisement.topic.schema.type_name ==
               advertisement.topic.msg_type &&
           advertisement.topic.schema.serialization_type ==
               advertisement.topic.serialization_type &&
           advertisement.topic.schema.schema_hash ==
               ibmw::util::ComputeSchemaHash(
                   advertisement.topic.schema.schema_data);
  } catch (const std::exception&) {
    return false;
  }
}

ibmw::transport::SerializedMessage TranscodePlaybackMessage(
    CliTypeSupportRegistry& registry,
    const TopicAdvertisement& advertisement,
    const ibmw::transport::SerializedMessage& message) {
  MW_ASSERT(
      message.payload != nullptr,
      "Cannot transcode a null playback payload for '{}'.",
      message.topic.topic);
  registry.Load(advertisement.type_support_package);
  const auto type_support = registry.Find(message.topic.msg_type);
  MW_ASSERT(
      type_support &&
          message.topic.msg_type == advertisement.topic.msg_type &&
          message.topic.serialization_type !=
              advertisement.topic.serialization_type &&
          type_support.CheckSerializationTypeSupported(
              message.topic.serialization_type) &&
          type_support.CheckSerializationTypeSupported(
              advertisement.topic.serialization_type),
      "TypeSupport cannot transcode topic '{}' from '{}' to '{}'.",
      message.topic.topic, message.topic.serialization_type,
      advertisement.topic.serialization_type);
  auto object = type_support.CreateSharedPtr();
  MW_ASSERT(
      type_support.Deserialize(
          message.topic.serialization_type,
          *message.payload->NativeHandle(), object.get()),
      "Playback deserialization failed for topic '{}' format '{}'.",
      message.topic.topic, message.topic.serialization_type);
  std::string converted = type_support.SimpleSerialize(
      advertisement.topic.serialization_type, object.get());
  MW_ASSERT(
      !converted.empty(),
      "Playback serialization failed for topic '{}' format '{}'.",
      advertisement.topic.topic,
      advertisement.topic.serialization_type);
  auto output = message;
  output.topic = advertisement.topic;
  output.payload = MakeOwnedPayload(std::move(converted));
  return output;
}

class TemporaryFileGuard {
 public:
  explicit TemporaryFileGuard(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~TemporaryFileGuard() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

 private:
  std::filesystem::path path_;
};

class EngineGuard {
 public:
  explicit EngineGuard(std::unique_ptr<runtime::core::Engine> engine)
      : engine_(std::move(engine)) {}
  ~EngineGuard() { Stop(); }

  runtime::core::Engine& Get() { return *engine_; }
  void SetFuture(std::future<void> future) { future_ = std::move(future); }
  void Stop() {
    if (!engine_) {
      return;
    }
    engine_->Shutdown();
    if (future_.valid()) {
      future_.wait();
    }
    engine_.reset();
  }

 private:
  std::unique_ptr<runtime::core::Engine> engine_;
  std::future<void> future_;
};

class PlayerOwnedPublisherRegistry {
 public:
  PlayerOwnedPublisherRegistry(
      runtime::core::transport::TransportBus& transport_bus,
      CliTypeSupportRegistry& type_support_registry)
      : transport_bus_(transport_bus),
        type_support_registry_(type_support_registry) {
    for (auto* driver : transport_bus_.GetUsedTransportDriver()) {
      drivers_.emplace(std::string(driver->Name()), driver);
    }
  }

  void Register(
      const BagChannelInfo& channel,
      const PlaybackDriverSet& requested_drivers) {
    const std::string key = PlaybackTopicKey(channel.topic);
    if (entries_.contains(key)) {
      return;
    }

    Entry entry;
    std::vector<runtime::core::transport::TransportDriver*> bus_drivers;
    for (const auto& driver : requested_drivers) {
      MW_ASSERT(
          driver != "local" && drivers_.contains(driver),
          "Player config does not initialize driver '{}' for topic '{}'.",
          driver, channel.topic.topic);
    }

    for (const auto& driver : requested_drivers) {
      auto* transport_driver = drivers_.at(driver);
      const auto* capability =
          transport_bus_.GetSerializedTransportCapability(*transport_driver);
      const bool direct =
          capability != nullptr && capability->HasValidContract() &&
          capability->publish_mode ==
              runtime::core::transport::SerializedPublishMode::kNativeSerialized &&
          capability->operations.supports_topic != nullptr &&
          capability->operations.supports_topic(
              capability->operations.context, channel.topic);
      if (!direct) {
        entry.bus_publish = true;
        bus_drivers.emplace_back(transport_driver);
        continue;
      }
      auto topic = channel.topic;
      if (capability->operations.publisher_qos != nullptr) {
        topic.qos = MergePlaybackQos(
            std::move(topic.qos),
            capability->operations.publisher_qos(
                capability->operations.context, channel.topic.topic,
                channel.topic.msg_type));
      }
      MW_ASSERT(
          capability->operations.register_publisher != nullptr,
          "Driver '{}' native publisher capability is incomplete.", driver);
      const auto handle = capability->operations.register_publisher(
          capability->operations.context, {.topic = std::move(topic)});
      MW_ASSERT(
          handle,
          "Driver '{}' serialized publisher registration failed for topic '{}'.",
          driver, channel.topic.topic);
      entry.direct_publishers.emplace_back(DirectPublisher{
          .capability = capability, .handle = handle});
    }
    if (entry.bus_publish) {
      const auto type_support =
          type_support_registry_.Find(channel.topic.msg_type);
      std::string package_path =
          type_support_registry_.PackagePath(channel.topic.msg_type);
      if (package_path.empty()) {
        package_path = "ibmw-bag-player";
      }
      const std::string module_name =
          "ibmw-bag-player-" + channel.topic.serialization_type;
      runtime::core::transport::SendTypeWrapper wrapper;
      wrapper.info = runtime::core::transport::TransportInfo{
          .msg_type = channel.topic.msg_type,
          .topic_name = channel.topic.topic,
          .pkg_path = package_path,
          .module_name = module_name,
          .msg_type_support_ref = type_support};
      MW_ASSERT(
          transport_bus_.RegisterSendType(
              std::move(wrapper), bus_drivers),
          "Player publisher registration failed for topic '{}'.",
          channel.topic.topic);
      const auto* registered =
          transport_bus_.GetTransportCatalog()->GetSendTypeWrapperPtr(
              channel.topic.msg_type, channel.topic.topic,
              package_path, module_name);
      MW_ASSERT(
          registered != nullptr,
          "Cannot resolve Player publisher for topic '{}'.",
          channel.topic.topic);
      entry.transport_info = &registered->info;
    }

    MW_ASSERT(
        entry.transport_info != nullptr ||
            !entry.direct_publishers.empty(),
        "No external playback driver is configured for topic '{}'.",
        channel.topic.topic);
    entries_.emplace(key, std::move(entry));
  }

  bool Supports(const TopicAdvertisement& advertisement) const {
    return entries_.contains(PlaybackTopicKey(advertisement.topic));
  }

  void Publish(
      const TopicAdvertisement& advertisement,
      const ibmw::transport::SerializedMessage& message) const {
    const auto entry = entries_.find(PlaybackTopicKey(advertisement.topic));
    MW_ASSERT(
        entry != entries_.end(),
        "No Player-owned publisher for topic '{}'.",
        advertisement.topic.topic);
    MW_ASSERT(
        message.payload != nullptr,
        "Playback payload is null for topic '{}'.",
        advertisement.topic.topic);
    for (const auto& publisher : entry->second.direct_publishers) {
      MW_ASSERT(
          publisher.capability->operations.send != nullptr &&
              publisher.capability->operations.send(
                  publisher.capability->operations.context, publisher.handle,
                  message),
          "Serialized playback publish failed for topic '{}'.",
          advertisement.topic.topic);
    }
    if (entry->second.transport_info == nullptr) {
      return;
    }

    ibmw::transport::Context context;
    context.SetSerializationType(message.topic.serialization_type);
    runtime::core::transport::MessageFrame frame{
        .info = *entry->second.transport_info,
        .msg_ptr = nullptr,
        .ctx_ref = context};
    frame.serialization_cache.emplace(
        message.topic.serialization_type, message.payload);
    transport_bus_.Send(std::move(frame));
  }

  bool WaitForAcknowledgments(std::chrono::milliseconds timeout) const {
    std::set<const runtime::core::transport::SerializedTransportCapabilityV1*>
        capabilities;
    for (const auto& [key, entry] : entries_) {
      static_cast<void>(key);
      for (const auto& publisher : entry.direct_publishers) {
        capabilities.emplace(publisher.capability);
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (const auto* capability : capabilities) {
      const auto now = std::chrono::steady_clock::now();
      const auto remaining =
          now >= deadline
              ? std::chrono::milliseconds::zero()
              : std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
      if (capability->operations.wait_for_acknowledgments == nullptr ||
          !capability->operations.wait_for_acknowledgments(
              capability->operations.context, remaining)) {
        return false;
      }
    }
    return true;
  }

  bool SupportsAcknowledgments() const noexcept {
    bool has_direct_publisher = false;
    for (const auto& [key, entry] : entries_) {
      static_cast<void>(key);
      for (const auto& publisher : entry.direct_publishers) {
        has_direct_publisher = true;
        if (publisher.capability == nullptr ||
            !publisher.capability->supports_ack ||
            publisher.capability->operations.wait_for_acknowledgments ==
                nullptr) {
          return false;
        }
      }
    }
    return has_direct_publisher;
  }

 private:
  struct DirectPublisher {
    const runtime::core::transport::SerializedTransportCapabilityV1* capability =
        nullptr;
    runtime::core::transport::SerializedPublisherHandle handle;
  };

  struct Entry {
    const runtime::core::transport::TransportInfo* transport_info = nullptr;
    bool bus_publish = false;
    std::vector<DirectPublisher> direct_publishers;
  };

  runtime::core::transport::TransportBus& transport_bus_;
  CliTypeSupportRegistry& type_support_registry_;
  std::unordered_map<std::string, runtime::core::transport::TransportDriver*>
      drivers_;
  std::unordered_map<std::string, Entry> entries_;
};

std::string FormatDuration(uint64_t nanoseconds) {
  const uint64_t seconds = nanoseconds / 1000000000ull;
  const uint64_t remainder = nanoseconds % 1000000000ull;
  std::ostringstream output;
  output << seconds << '.' << std::setw(9) << std::setfill('0') << remainder
         << 's';
  return output.str();
}

int RunInfo(int argc, const char* const* argv) {
  const auto options = ParseInfoOptions(argc, argv);
  if (options.help_requested) {
    return 0;
  }
  auto reader = CreateBagReader(
      options.bag_path,
      ReaderOpenOptions{.expected_storage_id = options.storage_id});
  const auto& info = reader->Info();

  std::cout << "Files:             " << info.files.size() << '\n'
            << "Bag size:          " << info.total_size << " B\n"
            << "Storage id:        " << info.storage_id << '\n'
            << "Bag version:       " << info.version << '\n'
            << "Duration:          " << FormatDuration(info.Duration()) << '\n'
            << "Start:             " << info.start_timestamp << " ns\n"
            << "End:               " << info.end_timestamp << " ns\n"
            << "Messages:          " << info.message_count << '\n'
            << "Closed cleanly:    "
            << (info.closed_cleanly ? "yes" : "no") << '\n'
            << "Dropped messages:  " << info.dropped_message_count << '\n'
            << "Duplicate copies:  " << info.duplicate_message_count << '\n';
  if (!info.error_message.empty()) {
    std::cout << "Recording error:    " << info.error_message << '\n';
  }
  std::cout << "Compression:       " << info.compression_mode;
  if (!info.compression_format.empty()) {
    std::cout << '/' << info.compression_format;
  }
  std::cout << "\n\n";

  std::cout << "File information:\n";
  for (const auto& file : info.files) {
    std::cout << "  " << file.path.filename().string()
              << "  size=" << file.size
              << " messages=" << file.message_count
              << " compression=" << file.compression;
    if (!file.split_reason.empty()) {
      std::cout << " split_reason=" << file.split_reason;
    }
    std::cout << '\n';
  }

  std::cout << "\nTopic information:\n";
  for (const auto& channel : info.channels) {
    std::cout << "  Topic: " << channel.topic.topic << '\n'
              << "    Type: " << channel.topic.msg_type << '\n'
              << "    Messages: " << channel.message_count << '\n'
              << "    Serialization: "
              << channel.topic.serialization_type << '\n'
              << "    QoS:";
    if (channel.topic.qos.Empty()) {
      std::cout << " {}\n";
    } else {
      std::cout << '\n';
      for (const auto& [policy, value] : channel.topic.qos.policies) {
        std::cout << "      " << policy << ": " << value << '\n';
      }
    }
  }

  bool playable = info.closed_cleanly && info.version == 1 &&
                  !info.files.empty();
  uint64_t file_message_count = 0;
  for (const auto& file : info.files) {
    playable = playable && std::filesystem::is_regular_file(file.path) &&
               file.size == std::filesystem::file_size(file.path);
    file_message_count += file.message_count;
  }
  playable = playable && file_message_count == info.message_count;
  std::cout << "\nSchema information:\n";
  for (const auto& schema : info.schemas) {
    std::cout << "  " << schema.descriptor.schema_name
              << " encoding=" << schema.descriptor.schema_encoding
              << " hash=" << schema.descriptor.schema_hash
              << " size=" << schema.descriptor.schema_data.size()
              << " valid=" << (schema.hash_valid ? "yes" : "no") << '\n';
    playable = playable && schema.hash_valid &&
               !schema.descriptor.type_name.empty() &&
               !schema.descriptor.serialization_type.empty() &&
               !schema.descriptor.schema_name.empty() &&
               !schema.descriptor.schema_encoding.empty() &&
               !schema.descriptor.schema_data.empty() &&
               !schema.descriptor.schema_hash.empty();
  }
  uint64_t channel_message_count = 0;
  for (const auto& channel : info.channels) {
    channel_message_count += channel.message_count;
    playable = playable && !channel.topic.topic.empty() &&
               !channel.topic.msg_type.empty() &&
               !channel.topic.serialization_type.empty() &&
               channel.topic.msg_type == channel.topic.schema.type_name &&
               channel.topic.serialization_type ==
                   channel.topic.schema.serialization_type &&
               channel.topic.schema.schema_hash ==
                   ibmw::util::ComputeSchemaHash(
                       channel.topic.schema.schema_data);
  }
  playable = playable && channel_message_count == info.message_count &&
             (info.message_count == 0 || !info.channels.empty());

  uint64_t readable_message_count = 0;
  std::string playback_error;
  if (playable) {
    try {
      reader->ReadMessages({}, [&](BagMessage&& message) {
        if (message.payload == nullptr) {
          playable = false;
          return false;
        }
        ++readable_message_count;
        return true;
      });
      playable = playable && readable_message_count == info.message_count;
    } catch (const std::exception& error) {
      playable = false;
      playback_error = error.what();
    }
  }
  if (!playback_error.empty()) {
    std::cout << "Playback validation error: " << playback_error << '\n';
  }
  std::cout << "\nComplete playback: " << (playable ? "yes" : "no") << '\n';
  return playable ? 0 : 1;
}

int RunConvert(int argc, const char* const* argv) {
  const auto options = ParseConvertOptions(argc, argv);
  if (options.help_requested) {
    return 0;
  }

  const auto result = ConvertBag(BagConvertOptions{
      .input_path = options.input,
      .input_format = options.input_format,
      .output_format = options.output_format,
      .input_storage_id = options.input_storage_id,
      .input_storage_config_file = options.input_storage_config_file,
      .output =
          {.output_path = options.output,
           .storage_id = options.storage_id,
           .max_bag_size = options.max_bag_size,
           .max_bag_duration_ns = ParseDurationNanoseconds(
               options.max_bag_duration_seconds, "Max bag duration"),
           .compression_mode = options.compression_mode,
           .compression_format = options.compression_format,
           .compression_queue_size = options.compression_queue_size,
           .compression_threads =
               options.compression_threads == 0
                   ? std::max<size_t>(1, std::thread::hardware_concurrency())
                   : options.compression_threads,
           .storage_preset_profile = options.storage_preset_profile,
           .storage_config_file = options.storage_config_file}});

  MW_ASSERT(
      result.input_message_count == result.output_message_count,
      "Converted message count {} does not match input message count {}.",
      result.output_message_count, result.input_message_count);
  std::cout << "Converted " << result.output_message_count << " message(s), "
            << result.channel_count << " channel(s), and "
            << result.schema_count << " schema(s) from "
            << result.input_storage_id << " to " << result.output_storage_id
            << " at '" << result.output_path.string() << "'.\n";
  if (!result.input_closed_cleanly) {
    std::cout << "Warning: the input bag was not closed cleanly.\n";
  }
  return 0;
}

class KeyboardControlGuard {
 public:
  KeyboardControlGuard(
      PlayerControl& control,
      bool disabled)
      : control_(control) {
    const bool interactive = ::isatty(STDIN_FILENO) != 0;
    if (disabled || !interactive) {
      return;
    }
    MW_ASSERT(
        ::tcgetattr(STDIN_FILENO, &old_termios_) == 0,
        "Cannot read terminal settings.");
    auto raw = old_termios_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    MW_ASSERT(
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0,
        "Cannot enable keyboard controls.");
    terminal_changed_ = true;
    running_ = true;
    thread_ = std::thread([this] {
      while (running_.load() && !control_.IsStopped()) {
        pollfd descriptor{
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0};
        if (::poll(&descriptor, 1, 100) <= 0 ||
            (descriptor.revents & POLLIN) == 0) {
          continue;
        }
        char input = 0;
        if (::read(STDIN_FILENO, &input, 1) == 1 && input == ' ') {
          control_.TogglePause();
          std::cout << (control_.IsPaused() ? "Playback paused.\n"
                                            : "Playback resumed.\n");
        }
      }
    });
  }

  ~KeyboardControlGuard() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
    if (terminal_changed_) {
      static_cast<void>(
          ::tcsetattr(STDIN_FILENO, TCSANOW, &old_termios_));
    }
  }

 private:
  PlayerControl& control_;
  std::atomic<bool> running_{false};
  bool terminal_changed_ = false;
  termios old_termios_{};
  std::thread thread_;
};

int RunPlay(int argc, const char* const* argv) {
  const auto options = ParsePlayOptions(argc, argv);
  if (options.help_requested) {
    return 0;
  }
  SimpleLoggerLevelGuard logger_level(options.log_level);
  auto reader = CreateBagReader(
      options.bag_path,
      ReaderOpenOptions{
          .expected_storage_id = options.storage_id,
          .storage_config_file = options.storage_config_file});
  MW_ASSERT(
      options.start_offset_seconds * 1000000000.0 <=
          static_cast<double>(reader->Info().Duration()),
      "Playback start offset is outside the bag duration.");

  CliTypeSupportRegistry playback_type_support_registry;
  const auto route_compatible =
      [&playback_type_support_registry](
          const BagChannelInfo& channel,
          const TopicAdvertisement& advertisement) {
        return CanTranscodePlayback(
            playback_type_support_registry, channel, advertisement);
      };
  const auto transcode =
      [&playback_type_support_registry](
          const TopicAdvertisement& advertisement,
          const ibmw::transport::SerializedMessage& message) {
        return TranscodePlaybackMessage(
            playback_type_support_registry, advertisement, message);
      };
  std::vector<BagChannelInfo> output_channels;
  const std::set<std::string> selected_topics(
      options.topics.begin(), options.topics.end());
  for (auto channel : reader->Info().channels) {
    if (!selected_topics.empty() &&
        !selected_topics.contains(channel.topic.topic)) {
      continue;
    }
    const auto remap = options.remaps.find(channel.topic.topic);
    if (remap != options.remaps.end()) {
      channel.topic.topic = remap->second;
    }
    output_channels.emplace_back(std::move(channel));
  }
  auto playback_config =
      ResolvePlaybackConfig(options.config_file, output_channels);
  const auto& playback_routes = playback_config.routes;
  const bool has_player_owned_routes = std::any_of(
      playback_routes.begin(), playback_routes.end(),
      [](const auto& route) { return !route.second.drivers.empty(); });
  std::map<std::string, std::string> runtime_routes;
  for (const auto& [key, route] : playback_routes) {
    if (route.runtime_id) {
      runtime_routes.emplace(key, *route.runtime_id);
    }
  }
  std::optional<TemporaryFileGuard> player_config_guard;
  std::unique_ptr<EngineGuard> player_engine_guard;
  std::unique_ptr<PlayerOwnedPublisherRegistry> player_publishers;
  if (has_player_owned_routes) {
    LoadPlayerTypeSupportPackages(
        options.config_file, playback_type_support_registry);
    const auto config_path = WritePlayerConfig(
        options.config_file, output_channels, playback_routes,
        options.log_level);
    player_config_guard.emplace(config_path);
    auto engine = std::make_unique<runtime::core::Engine>();
    engine->Initialize({.cfg_file_path = config_path.string()});
    player_engine_guard = std::make_unique<EngineGuard>(std::move(engine));
    player_publishers = std::make_unique<PlayerOwnedPublisherRegistry>(
        player_engine_guard->Get().GetTransportBus(),
        playback_type_support_registry);
    for (const auto& channel : output_channels) {
      const auto route = playback_routes.find(
          PlaybackTopicKey(channel.topic));
      if (route == playback_routes.end() || route->second.drivers.empty()) {
        continue;
      }
      player_publishers->Register(
          channel, route->second.drivers);
    }
    MW_ASSERT(
        options.wait_for_all_acked_seconds < 0.0 ||
            player_publishers->SupportsAcknowledgments(),
        "--wait-for-all-acked requires a configured playback capability "
        "that supports acknowledgments.");
    player_engine_guard->SetFuture(
        player_engine_guard->Get().AsyncStart());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  PlayerControl control(options.start_paused);
  g_stop_requested = false;
  g_pause_toggle_requested = false;
  SignalGuard signal_guard;
  KeyboardControlGuard keyboard(
      control, options.disable_keyboard_controls);
  std::jthread signal_monitor([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested() && !control.IsStopped()) {
      if (g_pause_toggle_requested.exchange(false)) {
        control.TogglePause();
        if (options.log_level == "debug" || options.log_level == "info") {
          std::cout << (control.IsPaused() ? "Playback paused.\n"
                                          : "Playback resumed.\n");
        }
      }
      if (g_stop_requested.load()) {
        control.Stop();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  BagPlayer::EventCallback event_callback;
  if (options.log_level == "debug" || options.log_level == "info") {
    event_callback =
        [](std::string_view message) { std::cout << message << '\n'; };
  }
  if (options.log_level == "debug") {
    std::cout << "Playback read-ahead queue: "
              << options.read_ahead_queue_size
              << ", selected topics: " << options.topics.size()
              << ", remaps: " << options.remaps.size() << '\n';
  }
  BagPlayer player(
      *reader,
      PlayerOptions{
          .read_ahead_queue_size = options.read_ahead_queue_size,
          .rate = options.rate,
          .topics = options.topics,
          .loop = options.loop,
          .remaps = options.remaps,
          .delay_seconds = options.delay_seconds,
          .start_paused = options.start_paused,
          .start_offset_seconds = options.start_offset_seconds,
          .runtime_routes = std::move(runtime_routes),
          .runtime_registry_path =
              std::move(playback_config.runtime_registry_path)},
      control, std::move(event_callback),
      [publisher_registry = player_publishers.get()](
          const TopicAdvertisement& advertisement) {
        return publisher_registry != nullptr &&
               publisher_registry->Supports(advertisement);
      },
      [publisher_registry = player_publishers.get()](
          const TopicAdvertisement& advertisement,
          const ibmw::transport::SerializedMessage& message) {
        MW_ASSERT(
            publisher_registry != nullptr,
            "Player publisher registry is unavailable.");
        publisher_registry->Publish(advertisement, message);
      },
      route_compatible,
      transcode);
  std::cout << "Playing bag '" << reader->Info().bag_path.string()
            << "' at " << options.rate << "x.\n";
  if (options.start_paused &&
      (options.disable_keyboard_controls || ::isatty(STDIN_FILENO) == 0)) {
    std::cout << "Playback starts paused: send SIGUSR2 to PID " << ::getpid()
              << " to toggle pause.\n";
  }
  const uint64_t published = player.Play();
  if (options.wait_for_all_acked_seconds >= 0.0) {
    const auto timeout = std::chrono::milliseconds(static_cast<int64_t>(
        options.wait_for_all_acked_seconds * 1000.0));
    MW_ASSERT(
        player_publishers == nullptr ||
            player_publishers->WaitForAcknowledgments(timeout),
        "Timed out waiting for Player publisher acknowledgments.");
  }
  control.Stop();
  signal_monitor.request_stop();
  if (player_engine_guard) {
    player_engine_guard->Stop();
  }
  std::cout << "Published " << published << " message(s).\n";
  return 0;
}

int RunRecord(int argc, const char* const* argv) {
  auto options = ParseRecordOptions(argc, argv);
  if (options.help_requested) {
    return 0;
  }
  SimpleLoggerLevelGuard logger_level(options.log_level);
  RuntimeRegistry registry;
  BagTopicGraph graph;
  graph.ReplaceRuntimes(registry.DiscoverLocalRuntimes());
  RecordTopicSelector selector({
      .explicit_topics = options.topics,
      .all = options.all,
      .regex = options.regex,
      .exclude_regex = options.exclude_regex,
      .include_hidden_topics = options.include_hidden_topics,
      .include_unpublished_topics = options.include_unpublished_topics,
      .ignore_leaf_topics = options.ignore_leaf_topics});
  auto selected_topics = selector.Select(graph);
  const auto topic_has_publisher = [&graph](std::string_view topic) {
    const auto advertisements = graph.Find(topic);
    return std::any_of(
        advertisements.begin(), advertisements.end(),
        [](const auto& advertisement) { return advertisement.published; });
  };
  const auto topics_with_publishers = [&topic_has_publisher](
                                          const std::set<std::string>& topics) {
    std::set<std::string> result;
    for (const auto& topic : topics) {
      if (topic_has_publisher(topic)) {
        result.emplace(topic);
      }
    }
    return result;
  };
  auto capture_topics = topics_with_publishers(selected_topics);
  if (options.use_sim_time && topic_has_publisher("/clock")) {
    capture_topics.emplace("/clock");
  }
  const auto runtime_spec = BuildRecorderRuntimeSpec(
      graph,
      std::vector<std::string>(
          capture_topics.begin(), capture_topics.end()));

  std::optional<TemporaryFileGuard> config_guard;
  CliTypeSupportRegistry type_support_registry;
  for (const auto& path : runtime_spec.type_support_packages) {
    type_support_registry.Load(path);
  }
  const auto load_topic_type_support =
      [&](const std::set<std::string>& topics) {
        for (const auto& topic : topics) {
          for (const auto& advertisement : graph.Find(topic)) {
            if (!advertisement.type_support_package.empty()) {
              type_support_registry.Load(
                  advertisement.type_support_package);
            }
          }
        }
      };
  load_topic_type_support(capture_topics);
  CachedTransportSubscriptionRegistry cached_subscription_registry;
  std::unique_ptr<EngineGuard> engine_guard;
  std::unordered_map<std::string, runtime::core::transport::TransportDriver*>
      recorder_drivers;
  if (!runtime_spec.direct_drivers.empty()) {
    const auto config_path = WriteRecorderConfig(
        runtime_spec, "recorder", options.log_level);
    config_guard.emplace(config_path);
    auto engine = std::make_unique<runtime::core::Engine>();
    engine->Initialize({.cfg_file_path = config_path.string()});
    engine_guard = std::make_unique<EngineGuard>(std::move(engine));
    for (auto* driver : engine_guard->Get()
                            .GetTransportBus()
                            .GetUsedTransportDriver()) {
      recorder_drivers.emplace(std::string(driver->Name()), driver);
    }
  }

  BagCapturePlanner initial_planner(graph);
  BagCapturePlanner dynamic_planner(graph);
  RecorderCoordinator* runtime_tap_coordinator = nullptr;
  std::mutex runtime_tap_error_mutex;
  std::optional<std::string> runtime_tap_error;
  const auto configure_planner =
      [&](BagCapturePlanner& planner, bool allow_cached_subscription) {
        for (const auto& [driver_name, driver] : recorder_drivers) {
          const auto* capability = engine_guard->Get().GetTransportBus()
                                       .GetSerializedTransportCapability(*driver);
          if (capability != nullptr && capability->HasValidContract() &&
              capability->capture_mode ==
                  runtime::core::transport::SerializedCaptureMode::kNativeSerialized) {
            planner.RegisterTransportSourceFactory(
                driver_name,
                [capability](
                    const TopicAdvertisement& advertisement,
                    std::string_view source_name) {
                  return std::make_unique<TransportSubscriptionSource>(
                      std::string(source_name), *capability,
                      advertisement, advertisement.topic.qos);
                });
            continue;
          }
          if (!allow_cached_subscription) {
            continue;
          }
          planner.RegisterTransportSourceFactory(
              driver_name,
              [driver, &type_support_registry,
               &cached_subscription_registry](
                  const TopicAdvertisement& advertisement,
                  std::string_view source_name) {
                const auto capability =
                    advertisement.capture_capabilities.find(
                        std::string(source_name));
                MW_ASSERT(
                    capability !=
                            advertisement.capture_capabilities.end() &&
                        capability->second.mode == "cached_serialized",
                    "Driver '{}' does not advertise cached serialized "
                    "capture.",
                    source_name);
                auto type_support =
                    type_support_registry.Find(advertisement.topic.msg_type);
                MW_ASSERT(
                    type_support,
                    "TypeSupport '{}' was not loaded from '{}'.",
                    advertisement.topic.msg_type,
                    advertisement.type_support_package);
                return std::make_unique<CachedTransportSubscriptionSource>(
                    std::string(source_name), *driver, advertisement,
                    type_support, cached_subscription_registry,
                    advertisement.topic.qos);
              });
        }
        planner.SetRuntimeTapSourceFactory(
            [&runtime_tap_coordinator, &runtime_tap_error_mutex,
             &runtime_tap_error](
                const TopicAdvertisement& advertisement) {
              return std::make_unique<RuntimeTapSource>(
                  advertisement,
                  std::make_unique<UnixRuntimeTapClient>(
                      advertisement.control_socket,
                      [&runtime_tap_coordinator](uint64_t dropped) {
                        if (runtime_tap_coordinator != nullptr) {
                          runtime_tap_coordinator->AddDroppedMessages(dropped);
                        }
                      },
                      [&runtime_tap_error_mutex, &runtime_tap_error](
                          const std::string& error) {
                        {
                          std::lock_guard lock(runtime_tap_error_mutex);
                          if (!runtime_tap_error) {
                            runtime_tap_error = error;
                          }
                        }
                        g_stop_requested = true;
                      }));
            });
      };
  configure_planner(initial_planner, true);
  configure_planner(dynamic_planner, false);

  RecorderCoordinator coordinator(
      RecorderCoordinator::Options{
          .storage =
              {.output_path = options.output,
               .storage_id = options.storage_id,
               .max_bag_size = options.max_bag_size,
               .max_bag_duration_ns = ParseDurationNanoseconds(
                   options.max_bag_duration_seconds, "Max bag duration"),
               .compression_mode = options.compression_mode,
               .compression_format = options.compression_format,
               .compression_queue_size = options.compression_queue_size,
               .compression_threads =
                   options.compression_threads == 0
                       ? std::max<size_t>(
                             1, std::thread::hardware_concurrency())
                       : options.compression_threads,
               .storage_preset_profile = options.storage_preset_profile,
               .storage_config_file = options.storage_config_file},
          .queue =
              {.max_messages =
                   options.max_cache_size == 0 ? size_t{1} : size_t{0},
               .max_bytes = options.max_cache_size,
               // Never let storage backpressure block a transport callback.
               // A bounded queue drops the newest message and the drop count
               // is persisted in bag metadata by RecorderCoordinator.
               .full_policy = RecordQueue::FullPolicy::kDropNewest},
          .snapshot_mode = options.snapshot_mode,
          // A zero cache size still uses a one-message bounded queue.  Direct
          // synchronous storage writes would put disk latency on the
          // transport callback path.
          .synchronous_write = false},
      CreateStorage(options.storage_id));
  coordinator.Initialize();
  coordinator.Start();
  runtime_tap_coordinator = &coordinator;

  const auto register_unpublished_channels =
      [&](const std::set<std::string>& topics) {
        for (const auto& topic : topics) {
          for (const auto& advertisement : graph.Find(topic)) {
            if (advertisement.published) {
              continue;
            }
            coordinator.RegisterChannel(StorageChannelDescriptor{
                .topic = advertisement.topic});
          }
        }
      };
  register_unpublished_channels(selected_topics);

  std::cout << "Recording to '"
            << std::filesystem::absolute(options.output).string()
            << "' using " << options.storage_id << " storage\n";
  if (options.snapshot_mode) {
    std::cout << "Snapshot mode: send SIGUSR1 to PID " << ::getpid()
              << " to flush the current snapshot.\n";
  }
  if (options.start_paused) {
    std::cout << "Recording starts paused: send SIGUSR2 to PID " << ::getpid()
              << " to toggle pause.\n";
  }

  g_stop_requested = false;
  g_snapshot_requested = false;
  g_pause_toggle_requested = false;
  SignalGuard signal_guard;
  std::atomic<bool> recording_paused{options.start_paused};
  std::atomic<bool> sim_time_received{false};
  std::atomic<uint64_t> current_sim_time{0};
  std::atomic<bool> record_clock{selected_topics.contains("/clock")};
  std::mutex conversion_error_mutex;
  std::optional<std::string> conversion_error;
  const auto fail_conversion =
      [&](const ibmw::transport::SerializedMessage& message,
          std::string_view reason) {
        std::lock_guard lock(conversion_error_mutex);
        if (!conversion_error) {
          conversion_error =
              "Cannot transcode topic '" + message.topic.topic + "' type '" +
              message.topic.msg_type + "' from '" +
              message.topic.serialization_type + "' to '" +
              options.serialization_format + "': " + std::string(reason);
        }
        g_stop_requested = true;
      };
  const auto accept_message =
      [&](ibmw::transport::SerializedMessage&& message) {
        if (recording_paused.load()) {
          return;
        }
        if (options.serialization_format != "auto" &&
            message.topic.serialization_type !=
                options.serialization_format) {
          const auto type_support =
              type_support_registry.Find(message.topic.msg_type);
          const auto target_schema = type_support_registry.FindSchema(
              message.topic.msg_type, options.serialization_format);
          if (!type_support || !target_schema ||
              !type_support.CheckSerializationTypeSupported(
                  message.topic.serialization_type) ||
              !type_support.CheckSerializationTypeSupported(
                  options.serialization_format)) {
            fail_conversion(
                message,
                "the loaded TypeSupport or target Schema is unavailable");
            return;
          }
          auto object = type_support.CreateSharedPtr();
          if (!type_support.Deserialize(
                  message.topic.serialization_type,
                  *message.payload->NativeHandle(), object.get())) {
            fail_conversion(message, "source deserialization failed");
            return;
          }
          std::string converted = type_support.SimpleSerialize(
              options.serialization_format, object.get());
          if (converted.empty()) {
            fail_conversion(message, "target serialization failed");
            return;
          }
          message.payload = MakeOwnedPayload(std::move(converted));
          message.topic.serialization_type = options.serialization_format;
          message.topic.schema = *target_schema;
        }
        if (options.use_sim_time) {
          if (message.topic.topic == "/clock") {
            const auto timestamp = ExtractSimTime(message);
            if (!timestamp) {
              coordinator.AddDroppedMessages();
              return;
            }
            current_sim_time = *timestamp;
            sim_time_received = true;
            message.publish_timestamp = *timestamp;
            message.receive_timestamp = *timestamp;
            if (!record_clock.load()) {
              return;
            }
          } else if (!sim_time_received.load()) {
            return;
          } else {
            message.publish_timestamp = current_sim_time.load();
            message.receive_timestamp = current_sim_time.load();
          }
        }
        if (coordinator.Accept(std::move(message)) ==
            RecordQueue::PushResult::kClosed) {
          g_stop_requested = true;
        }
      };
  DynamicCaptureController controller(
      accept_message,
      [](std::string_view topic, std::string_view description, bool started) {
        std::cout << "  " << (started ? "started " : "stopped ") << topic
                  << " -> " << description << '\n';
      });
  const auto build_initial_plan =
      [&initial_planner](std::string_view topic)
      -> CapturePlan {
    return initial_planner.BuildPlan({std::string(topic)});
  };
  const auto build_dynamic_plan =
      [&dynamic_planner](std::string_view topic)
      -> CapturePlan {
    return dynamic_planner.BuildPlan({std::string(topic)});
  };
  controller.Reconcile(capture_topics, graph, build_initial_plan);
  if (engine_guard) {
    engine_guard->SetFuture(engine_guard->Get().AsyncStart());
  }
  while (!g_stop_requested.load()) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.polling_interval_ms));
    if (g_pause_toggle_requested.exchange(false)) {
      const bool paused = !recording_paused.load();
      recording_paused = paused;
      std::cout << (paused ? "Recording paused.\n" : "Recording resumed.\n");
    }
    if (g_snapshot_requested.exchange(false)) {
      if (options.snapshot_mode) {
        coordinator.TriggerSnapshot();
        std::cout << "Snapshot completed.\n";
      } else {
        std::cout << "Ignoring SIGUSR1 because snapshot mode is disabled.\n";
      }
    }
    if (options.no_discovery) {
      continue;
    }
    graph.ReplaceRuntimes(registry.DiscoverLocalRuntimes());
    selected_topics = selector.Select(graph);
    register_unpublished_channels(selected_topics);
    record_clock = selected_topics.contains("/clock");
    capture_topics = topics_with_publishers(selected_topics);
    if (options.use_sim_time && topic_has_publisher("/clock")) {
      capture_topics.emplace("/clock");
    }
    load_topic_type_support(capture_topics);
    controller.Reconcile(capture_topics, graph, build_dynamic_plan);
  }
  std::exception_ptr source_stop_error;
  try {
    controller.StopAll();
  } catch (...) {
    source_stop_error = std::current_exception();
  }
  runtime_tap_coordinator = nullptr;
  std::exception_ptr coordinator_close_error;
  try {
    coordinator.Close();
  } catch (...) {
    coordinator_close_error = std::current_exception();
  }
  std::exception_ptr engine_stop_error;
  if (engine_guard) {
    try {
      engine_guard->Stop();
    } catch (...) {
      engine_stop_error = std::current_exception();
    }
  }
  {
    std::lock_guard lock(conversion_error_mutex);
    MW_ASSERT(!conversion_error, "{}", conversion_error.value_or(""));
  }
  {
    std::lock_guard lock(runtime_tap_error_mutex);
    MW_ASSERT(!runtime_tap_error, "{}", runtime_tap_error.value_or(""));
  }
  if (source_stop_error) {
    std::rethrow_exception(source_stop_error);
  }
  if (coordinator_close_error) {
    std::rethrow_exception(coordinator_close_error);
  }
  if (engine_stop_error) {
    std::rethrow_exception(engine_stop_error);
  }

  const auto& summary = coordinator.Summary();
  std::cout << "Recorded " << summary.message_count << " message(s) to '"
            << summary.output_path.string() << "'.\n";
  return 0;
}

}  // namespace

}  // namespace ibmw::extensions::record_playback_extension

extern "C" int IbmwRunBagCommand(int argc, const char* const* argv) {
  using namespace ibmw::extensions::record_playback_extension;
  try {
    if (argc < 2 || std::string_view(argv[1]) == "help" ||
        std::string_view(argv[1]) == "--help" ||
        std::string_view(argv[1]) == "-h") {
      PrintBagUsage();
      return 0;
    }
    const std::string command = argv[1];
    if (command == "record") {
      return RunRecord(argc, argv);
    }
    if (command == "info") {
      return RunInfo(argc, argv);
    }
    if (command == "play") {
      return RunPlay(argc, argv);
    }
    if (command == "convert") {
      return RunConvert(argc, argv);
    }
    std::cerr << "Unknown ibmw bag command '" << command << "'.\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "ibmw bag failed: " << error.what() << '\n';
    return 1;
  }
}
