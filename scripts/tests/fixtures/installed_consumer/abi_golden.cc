#include <engine/transport/loaned_sample_handle.h>
#include <engine/transport/message_frame.h>
#include <engine/transport/transport_driver.h>
#include <engine/transport/transport_driver_capabilities.h>
#include <engine/transport/transport_driver_dispatcher.h>

#include <cstddef>
#include <iostream>

using namespace ibmw::runtime::core::transport;

static_assert(sizeof(TransportDriverCapabilitiesV1) == 3 * sizeof(void*));
static_assert(sizeof(MessageFrame) == 96);
static_assert(sizeof(TransportDriverDispatcher) == 488);
static_assert(offsetof(MessageFrame, info) == 0);
static_assert(offsetof(MessageFrame, msg_ptr) == 8);
static_assert(offsetof(MessageFrame, ctx_ref) == 16);
static_assert(offsetof(MessageFrame, serialization_cache) == 24);
static_assert(offsetof(MessageFrame, msg_cache_ptr) == 80);
static_assert(sizeof(LoanedSampleHandle) == 40);
static_assert(offsetof(LoanedSampleHandle, user_payload_ptr) == 0);
static_assert(offsetof(LoanedSampleHandle, user_payload_size) == 8);
static_assert(offsetof(LoanedSampleHandle, mode) == 16);
static_assert(offsetof(LoanedSampleHandle, driver_token) == 24);
static_assert(offsetof(LoanedSampleHandle, source_driver) == 32);

int main() {
  std::cout << "ABI sizeof MessageFrame=" << sizeof(MessageFrame)
            << " Dispatcher=" << sizeof(TransportDriverDispatcher) << '\n';
}
