// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cpp/context/context.h"

namespace ibmw::context {
namespace {

TEST(ContextTest, DefaultConstructionKeepsCurrentThreadContext) {
  details::ScopedThreadContextRestore restore;
  auto current = std::make_shared<Context>();
  current->LetMe();
  ASSERT_EQ(details::g_thread_ctx.ctx_ptr.lock(), current);

  auto other = std::make_shared<Context>();
  EXPECT_EQ(details::g_thread_ctx.ctx_ptr.lock(), current);

  other->LetMe();
  EXPECT_EQ(details::g_thread_ctx.ctx_ptr.lock(), other);
}

// API-021: OpLog reports the caller's function, not OpLog::Log.
struct RecordedLog {
  uint32_t level = 0;
  std::string function;
  std::string text;
};

template <class Table>
const Table* EmptyTable() {
  static const Table table = [] {
    Table t{};
    t.struct_size = sizeof(Table);
    return t;
  }();
  return &table;
}

TEST(ContextTest, LogReportsCallerFunction) {
  static RecordedLog recorded;
  static const ibmw_logger_base_t logger{
      .struct_size = sizeof(ibmw_logger_base_t),
      .get_log_level = [](void*) { return IBMW_LOG_LEVEL_INFO; },
      .log = [](void*, ibmw_log_level_t lvl, uint32_t, const char*, const char* function_name,
                const char* log_data, size_t log_data_size) {
        recorded = {static_cast<uint32_t>(lvl), function_name, std::string(log_data, log_data_size)};
      },
      .ctx = nullptr};
  // Only the logger is used; other sub-handles just need complete empty tables.
  ibmw_core_base_t base{};
  base.abi_version = IBMW_CORE_ABI_VERSION;
  base.struct_size = sizeof(ibmw_core_base_t);
  base.info = [](void*) { return ibmw_module_info_t{}; };
  base.configurator = [](void*) { return EmptyTable<ibmw_configurator_base_t>(); };
  base.executor_bus = [](void*) { return EmptyTable<ibmw_executor_bus_base_t>(); };
  base.logger = [](void*) { return &logger; };
  base.memory_handle = [](void*) { return EmptyTable<ibmw_allocator_base_t>(); };
  base.service_handle = [](void*) { return EmptyTable<ibmw_service_handle_base_t>(); };
  base.transport_handle = [](void*) { return EmptyTable<ibmw_transport_handle_base_t>(); };
  base.parameter_handle = [](void*) { return EmptyTable<ibmw_parameter_handle_base_t>(); };

  auto ctx = std::make_shared<Context>(Runtime(&base));
  ctx->log().Info("hello {}", 1);
  EXPECT_EQ(recorded.level, static_cast<uint32_t>(IBMW_LOG_LEVEL_INFO));
  EXPECT_EQ(recorded.text, "hello 1");
  // GCC names the whole member function, clang only "TestBody"; both name
  // the caller, not OpLog::Log.
  EXPECT_NE(recorded.function.find("TestBody"), std::string::npos) << recorded.function;

  recorded = {};
  ctx->log().Debug("dropped");
  EXPECT_TRUE(recorded.text.empty());
}

// API-053: without a current context the assertion reports the caller.
TEST(ContextTest, MissingCurrentContextThrowsAtCaller) {
  details::ScopedThreadContextRestore restore;
  details::g_thread_ctx.ctx_ptr.reset();
  try {
    details::GetCurrentContext();
    FAIL() << "no exception";
  } catch (const ibmw::common::util::IbmwException& e) {
    EXPECT_NE(std::string(e.what()).find("context_test.cc"), std::string::npos) << e.what();
    EXPECT_NE(std::string(e.what()).find("Broken context"), std::string::npos) << e.what();
  }
}

}  // namespace
}  // namespace ibmw::context
