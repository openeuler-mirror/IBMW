// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Two-process check of the shm_ext plugin as a shared object, loaded the way
// the engine loads extensions: the core ExtensionLoader dlopens the plugin and
// resolves its entry symbol, the extension object is handed to an Engine
// (ExtensionBus::RegisterExtension plus an extension item without a path), and
// the traffic goes through the public SenderAccess/ReceiverAccess API with the
// JSON type support. ExtensionBus itself never unloads a plugin (its loaders
// leak the handle), so each cycle ends with ExtensionLoader::UnLoadExtension()
// (destroy + dlclose) and the next cycle loads the plugin again in the same
// process. The plugin is linked -z nodelete, so it stays mapped after dlclose
// (DYN_CYCLE reports mapped_after_unload) and the second load gets a new
// extension object on the same runtime. Every cycle uses the same topic names,
// so a reload re-creates topics the persisting runtime already had. Because
// the plugin stays mapped, only cycle 1 has sender threads that predate the
// first dlopen. After the cycles, a plugin load with an invalid driver option
// checks that an exception thrown inside the plugin reaches this process as a
// std::exception whose dynamic type is ibmw::common::util::IbmwException with
// the message intact, and DYN_ABI lines report which DSO each C++ runtime
// symbol binds to, for the exe and for the plugin; they must agree and must not
// be the device's chipset libc++. See selfdev/shm/docs/测试文档.md §6.6.
//
// Usage: ibmw_shm_ext_dyn_runner pub|sub --plugin <path> --sync <dir>
//            [--topics N] [--early N] [--messages N] [--cycles N] [--timeout-s N]
// IBMW_SHM_ROOT must be set. Start both roles with the same arguments, in any
// order; <sync> must exist and be empty.
//
// pub: one sender thread per topic; --early of them are created before the
// plugin is loaded. They send probes until the subscriber reports a probe on
// every topic, then messages 1..--messages. The driver runs the lossless
// profile, so the subscriber must see exactly 1..N on every topic, in order.
// sub: writes <sync>/ready.<cycle> after a probe on every topic and
// <sync>/done.<cycle> ("OK ..." or "FAIL ...") once every message arrived or the
// timeout expired; the publisher shuts down only after done.<cycle>.
// Each cycle prints a DYN_CYCLE line; the exit code is 0 only if all passed.
// Send() returns void in the public API, so a lost message shows up only on the
// subscriber side, as a missing or out-of-order sequence number.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include <dlfcn.h>

#include "base/exception.h"
#include "cpp/module_base.h"
#include "cpp/util/json_transport.h"
#include "engine/engine.h"
#include "engine/extension/extension_loader.h"
#include "proc_creds.h"
#include "yaml-cpp/yaml.h"

namespace shm_ext_dyn {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct DynMsg {
  static constexpr const char* kTypeName = "shm_ext_dyn/Msg";
  uint32_t topic = 0;
  uint32_t kind = 0;  // kProbe or kData
  uint64_t seq = 0;
};
constexpr uint32_t kProbe = 0;
constexpr uint32_t kData = 1;

void to_json(nlohmann::json& j, const DynMsg& m) { j = nlohmann::json{{"t", m.topic}, {"k", m.kind}, {"s", m.seq}}; }
void from_json(const nlohmann::json& j, DynMsg& m) {
  j.at("t").get_to(m.topic);
  j.at("k").get_to(m.kind);
  j.at("s").get_to(m.seq);
}

struct Args {
  std::string role;
  std::string plugin;
  std::string sync;
  size_t topics = 20;
  size_t early = 6;
  uint64_t messages = 2000;
  int cycles = 2;
  int timeout_s = 60;
};

std::vector<std::string> Topics(const Args& a) {
  std::vector<std::string> topics;
  for (size_t i = 0; i < a.topics; ++i) topics.push_back("dyn/t" + std::to_string(i));
  return topics;
}

std::string SyncFile(const Args& a, const char* what, int cycle) {
  return a.sync + "/" + what + "." + std::to_string(cycle);
}

// Written under a temporary name and renamed, so the reader never sees half of it.
void WriteFile(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    out << text << "\n";
  }
  std::filesystem::rename(tmp, path);
}

bool WaitForFile(const std::string& path, Clock::time_point deadline, std::string* text = nullptr) {
  while (Clock::now() < deadline) {
    std::ifstream in(path);
    if (in) {
      if (text != nullptr) std::getline(in, *text);
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

// Whether the plugin is mapped into this process.
bool Mapped(const std::string& plugin) {
  std::error_code ec;
  const auto real = std::filesystem::canonical(plugin, ec).string();
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    if (line.find(ec ? plugin : real) != std::string::npos) return true;
  }
  return false;
}

// Engine configuration of one cycle: the extension item has an empty path, so
// the engine takes the registered extension object from our ExtensionLoader.
// Lossless delivery, so every message must arrive. The runtime graph is off: it
// is not part of this check and its default directory, /tmp/ibmw-<uid>, cannot
// be created on the device.
YAML::Node Config(const std::vector<std::string>& topics, bool pub,
                  const char* profile = "core_metrics_e2e_lossless_v1") {
  std::ostringstream y;
  y << "ibmw:\n"
       "  extensions:\n"
       "    items:\n"
       "      - name: shm_extension\n"
       "        path: \"\"\n"
       "  runtime_graph:\n"
       "    enabled: false\n"
       "  logging:\n"
       "    core_level: WARN\n"
       "    backends:\n"
       "      - type: console\n"
       "  messaging:\n"
       "    drivers:\n"
       "      - type: shm\n"
       "        options:\n"
       "          benchmark_profile: "
    << profile
    << "\n"
       "          publisher_too_slow_policy: WAIT_FOR_CONSUMER\n"
       "          subscriber_queue_full_policy: BLOCK_PRODUCER\n"
       "          subscriber_queue_capacity: 256\n"
    << (pub ? "    publishers:\n" : "    subscribers:\n");
  for (const auto& t : topics) y << "      - topic: \"" << t << "\"\n        enable_drivers: [shm]\n";
  y << "  modules:\n"
       "    modules:\n"
       "      - name: "
    << (pub ? "ShmExtDynPub" : "ShmExtDynSub") << "\n";
  return YAML::Load(y.str());
}

class PubModule : public ibmw::Module {
 public:
  explicit PubModule(std::vector<std::string> topics) : topics_(std::move(topics)) {}
  ibmw::ModuleDescriptor Info() const override { return ibmw::ModuleDescriptor{.module_name = "ShmExtDynPub"}; }
  bool Setup(ibmw::Runtime rt) override {
    for (const auto& t : topics_) {
      senders_.push_back(std::make_unique<ibmw::transport::SenderAccess<DynMsg>>(rt.GetTransport().GetPublisher(t)));
      if (!senders_.back()->RegisterSendType()) return false;
    }
    return true;
  }
  bool Run() override { return true; }
  void Stop() override {}
  ibmw::transport::SenderAccess<DynMsg>& Sender(size_t i) { return *senders_[i]; }

 private:
  std::vector<std::string> topics_;
  std::vector<std::unique_ptr<ibmw::transport::SenderAccess<DynMsg>>> senders_;
};

// Per-topic receive state; written by the listener thread of that topic.
struct TopicState {
  std::mutex mu;
  bool probed = false;
  uint64_t received = 0;
  uint64_t errors = 0;  // wrong topic, out of order, duplicate or beyond N
};

class SubModule : public ibmw::Module {
 public:
  SubModule(std::vector<std::string> topics, uint64_t messages)
      : topics_(std::move(topics)), messages_(messages), state_(topics_.size()) {}
  ibmw::ModuleDescriptor Info() const override { return ibmw::ModuleDescriptor{.module_name = "ShmExtDynSub"}; }
  bool Setup(ibmw::Runtime rt) override {
    for (size_t i = 0; i < topics_.size(); ++i) {
      receivers_.push_back(
          std::make_unique<ibmw::transport::ReceiverAccess<DynMsg>>(rt.GetTransport().GetSubscriber(topics_[i])));
      TopicState& st = state_[i];
      const uint32_t index = static_cast<uint32_t>(i);
      const uint64_t n = messages_;
      if (!receivers_.back()->OnMessage([&st, index, n](const std::shared_ptr<const DynMsg>& m) {
            std::lock_guard<std::mutex> lock(st.mu);
            if (m->topic != index) {
              ++st.errors;
            } else if (m->kind == kProbe) {
              st.probed = true;
            } else if (m->seq == st.received + 1 && m->seq <= n) {
              ++st.received;
            } else {
              ++st.errors;
            }
          })) {
        return false;
      }
    }
    return true;
  }
  bool Run() override { return true; }
  void Stop() override {}

  bool AllProbed() {
    for (auto& st : state_) {
      std::lock_guard<std::mutex> lock(st.mu);
      if (!st.probed) return false;
    }
    return true;
  }
  // received, errors summed over the topics; complete when every topic has all N.
  bool Totals(uint64_t& received, uint64_t& errors) {
    bool complete = true;
    received = errors = 0;
    for (auto& st : state_) {
      std::lock_guard<std::mutex> lock(st.mu);
      received += st.received;
      errors += st.errors;
      complete = complete && st.received == messages_;
    }
    return complete;
  }

 private:
  std::vector<std::string> topics_;
  uint64_t messages_;
  std::vector<TopicState> state_;
  std::vector<std::unique_ptr<ibmw::transport::ReceiverAccess<DynMsg>>> receivers_;
};

// Publisher threads: each owns one topic and waits for Go() before it touches
// the plugin, so a thread created before the plugin was loaded first enters its
// code (and thread_locals) after dlopen.
class Senders {
 public:
  Senders(const Args& a, Clock::time_point deadline) : args_(a), deadline_(deadline) {}
  ~Senders() { Stop(); }
  Senders(const Senders&) = delete;
  Senders& operator=(const Senders&) = delete;

  void Spawn(size_t index) { threads_.emplace_back([this, index] { Loop(index); }); }
  void Go(PubModule* module) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      module_ = module;
    }
    cv_.notify_all();
  }
  void SetReady() { ready_.store(true); }
  // Joins the threads after they sent everything (call after SetReady()).
  void Finish() {
    for (auto& t : threads_) {
      if (t.joinable()) t.join();
    }
  }
  // Lets the threads finish what they send, releases those still waiting for
  // Go() or a probe answer, and joins all of them.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      abort_ = true;
    }
    stop_.store(true);
    cv_.notify_all();
    for (auto& t : threads_) {
      if (t.joinable()) t.join();
    }
  }
  uint64_t Sent() const { return sent_.load(); }
  uint64_t Probes() const { return probes_.load(); }
  bool Failed() const { return failed_.load(); }

 private:
  void Loop(size_t index) {
    PubModule* module = nullptr;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return module_ != nullptr || abort_; });
      if (module_ == nullptr) return;
      module = module_;
    }
    auto& sender = module->Sender(index);
    DynMsg m;
    m.topic = static_cast<uint32_t>(index);
    m.kind = kProbe;
    while (!ready_.load()) {
      if (stop_.load() || Clock::now() > deadline_) {
        failed_.store(true);
        return;
      }
      sender.Send(m);
      probes_.fetch_add(1);
      std::this_thread::sleep_for(1ms);
    }
    m.kind = kData;
    for (uint64_t seq = 1; seq <= args_.messages; ++seq) {
      // Send() can block (BLOCK_PRODUCER); this only stops a sender between sends.
      if (stop_.load() || Clock::now() > deadline_) {
        failed_.store(true);
        return;
      }
      m.seq = seq;
      sender.Send(m);
    }
    sent_.fetch_add(args_.messages);
  }

  const Args& args_;
  const Clock::time_point deadline_;
  std::mutex mu_;
  std::condition_variable cv_;
  PubModule* module_ = nullptr;
  bool abort_ = false;
  std::atomic_bool ready_{false};
  std::atomic_bool stop_{false};
  std::atomic_bool failed_{false};
  std::atomic_uint64_t sent_{0};
  std::atomic_uint64_t probes_{0};
  std::vector<std::thread> threads_;
};

using ibmw::runtime::core::Engine;
using ibmw::runtime::core::extension::ExtensionLoader;

// Starts an engine around the loaded extension, runs `body`, then shuts the
// engine and the extension down (ExtensionBus::Shutdown only does that for
// extensions it loaded itself).
void WithEngine(ibmw::EngineExtensionBase* ext, ibmw::Module& module, const YAML::Node& config,
                const std::function<void()>& body) {
  Engine engine;
  engine.GetExtensionBus().RegisterExtension(ext);
  engine.GetModuleBus().RegisterModule(module.NativeHandle());
  Engine::Options options;
  options.config_node = config;
  engine.Initialize(options);
  auto running = engine.AsyncStart();
  try {
    body();
  } catch (...) {
    engine.Shutdown();
    running.get();
    ext->Shutdown();
    throw;
  }
  engine.Shutdown();
  running.get();
  ext->Shutdown();
}

ibmw::EngineExtensionBase* Load(ExtensionLoader& loader, const Args& a) {
  loader.LoadExtension(a.plugin);
  auto* ext = loader.GetExtension();
  if (ext == nullptr || ext->Name() != "shm_extension") throw std::runtime_error("unexpected extension in plugin");
  return ext;
}

bool PubCycle(const Args& a, int cycle) {
  const auto topics = Topics(a);
  const auto deadline = Clock::now() + std::chrono::seconds(a.timeout_s);
  Senders senders(a, deadline);
  for (size_t i = 0; i < a.early && i < a.topics; ++i) senders.Spawn(i);  // before dlopen

  ExtensionLoader loader;
  auto* ext = Load(loader, a);
  const bool mapped = Mapped(a.plugin);
  std::string verdict = "none";
  bool ready = false;
  {
    PubModule module(topics);
    WithEngine(ext, module, Config(topics, true), [&] {
      // No sender may outlive the module or the engine, also on an exception.
      struct StopSenders {
        Senders& s;
        ~StopSenders() { s.Stop(); }
      } stop_senders{senders};
      senders.Go(&module);
      for (size_t i = a.early; i < a.topics; ++i) senders.Spawn(i);  // after dlopen
      ready = WaitForFile(SyncFile(a, "ready", cycle), deadline);
      if (ready) {
        senders.SetReady();
        senders.Finish();
      }
      if (ready && !senders.Failed()) WaitForFile(SyncFile(a, "done", cycle), deadline, &verdict);
    });
  }
  loader.UnLoadExtension();
  const bool mapped_after = Mapped(a.plugin);
  const bool ok = ready && !senders.Failed() && senders.Sent() == a.topics * a.messages &&
                  verdict.rfind("OK", 0) == 0 && !loader.GetDynamicLib().IsLoaded() && mapped && mapped_after;
  std::printf("DYN_CYCLE role=pub cycle=%d topics=%zu early_threads=%zu sent=%llu probes=%llu mapped=%d "
              "unloaded=%d mapped_after_unload=%d sub=\"%s\" result=%s\n",
              cycle, a.topics, a.early, static_cast<unsigned long long>(senders.Sent()),
              static_cast<unsigned long long>(senders.Probes()), mapped ? 1 : 0,
              loader.GetDynamicLib().IsLoaded() ? 0 : 1, mapped_after ? 1 : 0, verdict.c_str(), ok ? "PASS" : "FAIL");
  std::fflush(stdout);
  return ok;
}

bool SubCycle(const Args& a, int cycle) {
  const auto topics = Topics(a);
  const auto deadline = Clock::now() + std::chrono::seconds(a.timeout_s);
  ExtensionLoader loader;
  auto* ext = Load(loader, a);
  const bool mapped = Mapped(a.plugin);
  uint64_t received = 0, errors = 0;
  bool complete = false;
  {
    SubModule module(topics, a.messages);
    WithEngine(ext, module, Config(topics, false), [&] {
      bool ready = false;
      while (Clock::now() < deadline) {
        if (!ready && module.AllProbed()) {
          WriteFile(SyncFile(a, "ready", cycle), "ready");
          ready = true;
        }
        complete = module.Totals(received, errors);
        if (complete) break;
        std::this_thread::sleep_for(2ms);
      }
      complete = module.Totals(received, errors) && errors == 0;
      WriteFile(SyncFile(a, "done", cycle), std::string(complete ? "OK" : "FAIL") + " received=" +
                                                 std::to_string(received) + " errors=" + std::to_string(errors));
    });
  }
  loader.UnLoadExtension();
  const bool mapped_after = Mapped(a.plugin);
  const bool ok =
      complete && received == a.topics * a.messages && !loader.GetDynamicLib().IsLoaded() && mapped && mapped_after;
  std::printf("DYN_CYCLE role=sub cycle=%d topics=%zu received=%llu expected=%llu errors=%llu mapped=%d "
              "unloaded=%d mapped_after_unload=%d result=%s\n",
              cycle, a.topics, static_cast<unsigned long long>(received),
              static_cast<unsigned long long>(a.topics * a.messages), static_cast<unsigned long long>(errors),
              mapped ? 1 : 0, loader.GetDynamicLib().IsLoaded() ? 0 : 1, mapped_after ? 1 : 0, ok ? "PASS" : "FAIL");
  std::fflush(stdout);
  return ok;
}

// Owning DSO of `sym` as the exe resolves it (RTLD_DEFAULT) and as dlsym on the
// plugin handle resolves it (the handle's dependency order). The plugin's own
// relocations bind through the global scope first, so the plugin column is a
// proxy, not a dump of its relocations. The plugin and the exe throw and catch
// each other's exceptions and share thread_locals and operator new, so both
// must see the same copy, and not the chipset libc++ of the device image.
bool AbiCheck(ExtensionLoader& loader) {
  static const char* const kSymbols[] = {"__cxa_throw", "__gxx_personality_v0", "__cxa_thread_atexit",
                                         "__emutls_get_address", "_Znwm", "_ZTISt9exception"};
  auto owner = [](void* addr) -> std::string {
    Dl_info info{};
    if (addr == nullptr || dladdr(addr, &info) == 0 || info.dli_fname == nullptr) return "unresolved";
    return info.dli_fname;
  };
  bool ok = true;
  for (const char* sym : kSymbols) {
    const std::string exe = owner(dlsym(RTLD_DEFAULT, sym));
    const std::string plugin = owner(loader.GetDynamicLib().GetSymbol(sym));
    const bool same = exe == plugin && exe != "unresolved" && exe.find("chipset") == std::string::npos;
    std::printf("DYN_ABI symbol=%s exe=%s plugin=%s result=%s\n", sym, exe.c_str(), plugin.c_str(),
                same ? "PASS" : "FAIL");
    ok = ok && same;
  }
  return ok;
}

// An unknown benchmark_profile makes ShmTransportDriver::Initialize (plugin
// code, its MW_REQUIRE throws ibmw::common::util::IbmwException) throw during
// Engine::Initialize; the engine's transport subsystem (exe code) rolls back and
// rethrows it. Extension options would not do: the core ExtensionBase linked
// into the plugin catches what OnInit throws and the engine throws its own.
// It must arrive here as a std::exception (one shared runtime, so its typeinfo
// is shared) with IbmwException as the dynamic type name and the message
// intact; two libc++abi copies would turn it into std::terminate or an
// unmatched type. typed_catch (a typed handler) and typed_cast (dynamic_cast)
// report whether the exe's own IbmwException typeinfo also matches. The build
// presets hidden visibility, so the exe and the plugin each hold a local copy
// of that typeinfo; libstdc++ falls back to comparing names (x86: 1), libc++
// compares addresses (OHOS: 0). That is a
// framework property, not a plugin defect, so it is reported but not gated on.
bool ThrowCheck(const Args& a) {
  ExtensionLoader loader;
  auto* ext = Load(loader, a);
  bool type_ok = false;
  bool typed_catch = false;
  bool typed_cast = false;
  std::string type;
  std::string what;
  {
    PubModule module(Topics(a));
    Engine engine;
    engine.GetExtensionBus().RegisterExtension(ext);
    engine.GetModuleBus().RegisterModule(module.NativeHandle());
    Engine::Options options;
    options.config_node = Config(Topics(a), true, "no_such_profile");
    try {
      engine.Initialize(options);
    } catch (const std::exception& e) {
      type = typeid(e).name();
      type_ok = type == typeid(::ibmw::common::util::IbmwException).name();
      what = e.what();
      // The handler match and dynamic_cast both compare the exe's typeinfo
      // with the plugin's.
      try {
        throw;
      } catch (const ::ibmw::common::util::IbmwException&) {
        typed_catch = true;
      } catch (...) {
      }
      typed_cast = dynamic_cast<const ::ibmw::common::util::IbmwException*>(&e) != nullptr;
    } catch (...) {
      type = "not a std::exception";
    }
  }
  const bool abi = AbiCheck(loader);
  loader.UnLoadExtension();
  const bool ok = type_ok && what.find("Shm benchmark delivery controls") != std::string::npos && abi;
  std::printf("DYN_THROW type_ok=%d type=%s typed_catch=%d typed_cast=%d what=\"%s\" abi=%s result=%s\n",
              type_ok ? 1 : 0, type.c_str(), typed_catch ? 1 : 0, typed_cast ? 1 : 0, what.c_str(),
              abi ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
  std::fflush(stdout);
  return ok;
}

int Main(int argc, char** argv) {
  Args a;
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s pub|sub --plugin <path> --sync <dir> [--topics N] [--early N] [--messages N] "
                 "[--cycles N] [--timeout-s N]\n",
                 argv[0]);
    return 64;
  }
  a.role = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--plugin") {
      a.plugin = v;
    } else if (k == "--sync") {
      a.sync = v;
    } else if (k == "--topics") {
      a.topics = std::stoul(v);
    } else if (k == "--early") {
      a.early = std::stoul(v);
    } else if (k == "--messages") {
      a.messages = std::stoull(v);
    } else if (k == "--cycles") {
      a.cycles = std::stoi(v);
    } else if (k == "--timeout-s") {
      a.timeout_s = std::stoi(v);
    } else {
      std::fprintf(stderr, "unknown argument %s\n", k.c_str());
      return 64;
    }
  }
  if ((a.role != "pub" && a.role != "sub") || a.plugin.empty() || a.sync.empty() || argc % 2 != 0) {
    std::fprintf(stderr, "need pub|sub, --plugin and --sync\n");
    return 64;
  }
  if (std::getenv("IBMW_SHM_ROOT") == nullptr) {
    std::fprintf(stderr, "IBMW_SHM_ROOT must be set\n");
    return 64;
  }
  // ThrowCheck runs last: the plugin is never unmapped (-z nodelete), so a
  // load before cycle 1 would mean no sender thread predates the first dlopen.
  bool ok = true;
  for (int cycle = 1; cycle <= a.cycles; ++cycle) {
    ok = (a.role == "pub" ? PubCycle(a, cycle) : SubCycle(a, cycle)) && ok;
  }
  ok = ThrowCheck(a) && ok;
  std::printf("DYN_RESULT role=%s cycles=%d result=%s\n", a.role.c_str(), a.cycles, ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

}  // namespace shm_ext_dyn

int main(int argc, char** argv) {
  ibmw::extensions::shm_extension::test::PrintProcCreds(stdout, "runner");
  try {
    return shm_ext_dyn::Main(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "shm_ext_dyn_runner failed: %s\n", e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "shm_ext_dyn_runner failed: exception not derived from std::exception\n");
    return 1;
  }
}
