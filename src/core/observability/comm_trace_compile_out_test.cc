#include "observability/comm_trace.h"

#include <gtest/gtest.h>

namespace {

TEST(CommTraceCompileOutTest, MacrosHaveNoRuntimeDependency) {
  IBMW_COMM_TRACE_INITIALIZE();
  IBMW_COMM_TRACE_SCOPE(names_are_not_resolved, when_compiled_out);
  IBMW_COMM_TRACE_CALLBACK_SCOPE();
  IBMW_COMM_TRACE_CALLBACK_REGISTRATION_SCOPE(names_are_not_resolved);
  SUCCEED();
}

}  // namespace
