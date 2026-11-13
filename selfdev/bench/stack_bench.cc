// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// selfdev crash_handler CaptureStack / FormatStack vs backward-cpp
// StackTrace::load_here / TraceResolver / Printer. The resolver backend is
// chosen by the build (BACKWARD_HAS_BACKTRACE_SYMBOL or BACKWARD_HAS_DW) and
// printed at start-up.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "backward.hpp"
#include "bench_harness.h"
#include "ibmw_selfdev/crash_handler/crash_handler.h"

namespace bench = ibmw::selfdev::bench;
namespace crash = ibmw::selfdev::crash;

// Recurses `depth` frames, then runs the capture. Exported (not static) and
// noinline so every frame is real and has a dynamic symbol.
using CaptureFn = size_t (*)(void* ctx);

extern "C" [[gnu::noinline]] size_t IbmwBenchRecurse(int depth, CaptureFn fn, void* ctx) {
  if (depth <= 0) return fn(ctx);
  const size_t r = IbmwBenchRecurse(depth - 1, fn, ctx);
  bench::DoNotOptimize(r);  // keeps the call out of tail position
  return r + 0;
}

namespace {

struct CaptureCtx {
  size_t frames;
  void* buf[crash::kMaxFrames];
  backward::StackTrace* st;
};

size_t SelfdevCapture(void* p) {
  auto* c = static_cast<CaptureCtx*>(p);
  return crash::CaptureStack(c->buf, c->frames);
}

size_t BackwardCapture(void* p) {
  auto* c = static_cast<CaptureCtx*>(p);
  return c->st->load_here(c->frames);
}

const char* BackwardBackend() {
#if BACKWARD_HAS_DW == 1
  return "BACKWARD_HAS_DW (libdw)";
#elif BACKWARD_HAS_BFD == 1
  return "BACKWARD_HAS_BFD";
#elif BACKWARD_HAS_DWARF == 1
  return "BACKWARD_HAS_DWARF";
#elif BACKWARD_HAS_BACKTRACE_SYMBOL == 1
  return "BACKWARD_HAS_BACKTRACE_SYMBOL (backtrace_symbols)";
#else
  return "unknown";
#endif
}

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r(bench::ParseOptions(argc, argv));
  std::printf("# backward resolver backend: %s, unwinder: %s\n", BackwardBackend(),
              BACKWARD_HAS_UNWIND == 1 ? "_Unwind_Backtrace" : "other");

  // Capture: recursion depth d below main, capture d frames (the recursion
  // frames themselves), so both walk the same d frames.
  for (size_t d : {size_t{16}, size_t{32}, size_t{64}}) {
    const uint64_t n = r.N(200'000);
    r.Compare(
        "stack/capture depth " + std::to_string(d),
        [&] {
          CaptureCtx c{d, {}, nullptr};
          size_t got = 0;
          for (uint64_t i = 0; i < n; ++i) got += IbmwBenchRecurse(static_cast<int>(d) + 4, &SelfdevCapture, &c);
          bench::DoNotOptimize(got);
          return n;
        },
        [&] {
          backward::StackTrace st;
          CaptureCtx c{d, {}, &st};
          size_t got = 0;
          for (uint64_t i = 0; i < n; ++i) got += IbmwBenchRecurse(static_cast<int>(d) + 4, &BackwardCapture, &c);
          bench::DoNotOptimize(got);
          return n;
        },
        "backward");
  }

  // Symbolize + format 32 frames (the same captured pcs for both sides).
  CaptureCtx sc{32, {}, nullptr};
  const size_t nframes = IbmwBenchRecurse(40, &SelfdevCapture, &sc);
  backward::StackTrace bst;
  CaptureCtx bc{32, {}, &bst};
  IbmwBenchRecurse(40, &BackwardCapture, &bc);
  std::printf("# frames: selfdev=%zu backward=%zu\n", nframes, bst.size());

  std::vector<char> buf(64 * 1024);
  {
    const size_t len = crash::FormatStack(buf.data(), buf.size(), sc.buf, nframes, true, true);
    std::printf("# selfdev FormatStack output: %zu bytes; first line: %.*s", len,
                static_cast<int>(std::string(buf.data()).find('\n') + 1), buf.data());
  }
  {
    std::ostringstream os;
    backward::Printer p;
    p.address = true;
    p.print(bst, os);
    const std::string s = os.str();
    std::printf("# backward Printer output: %zu bytes\n", s.size());
  }

  const uint64_t nf = r.N(2'000);
  r.Compare(
      "stack/symbolize 32 frames (resolver reused)",
      [&] {
        size_t total = 0;
        for (uint64_t i = 0; i < nf; ++i) total += crash::FormatStack(buf.data(), buf.size(), sc.buf, nframes, true, true);
        bench::DoNotOptimize(total);
        return nf;
      },
      [&] {
        backward::TraceResolver tr;
        size_t total = 0;
        for (uint64_t i = 0; i < nf; ++i) {
          tr.load_stacktrace(bst);
          for (size_t k = 0; k < bst.size(); ++k) total += tr.resolve(bst[k]).object_function.size();
        }
        bench::DoNotOptimize(total);
        return nf;
      },
      "backward");
  // What a crash report costs once: fresh resolver/printer, as backward's
  // SignalHandling does on every signal; output to a string stream.
  r.Compare(
      "stack/crash report 32 frames (fresh printer)",
      [&] {
        size_t total = 0;
        for (uint64_t i = 0; i < nf; ++i) total += crash::FormatStack(buf.data(), buf.size(), sc.buf, nframes, true, true);
        bench::DoNotOptimize(total);
        return nf;
      },
      [&] {
        size_t total = 0;
        for (uint64_t i = 0; i < nf; ++i) {
          std::ostringstream os;
          backward::Printer p;
          p.address = true;
          p.print(bst, os);
          total += static_cast<size_t>(os.tellp());
        }
        bench::DoNotOptimize(total);
        return nf;
      },
      "backward");
  return 0;
}
