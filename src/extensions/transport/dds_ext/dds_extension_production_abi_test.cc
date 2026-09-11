#include <dlfcn.h>

#include <cstdint>
#include <iostream>

struct QueryV2 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t reserved;
  void* snapshot;
  void* request_seal;
};
static_assert(sizeof(QueryV2) == 32);

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    std::cerr << dlerror() << '\n';
    return 1;
  }
  using Query = const QueryV2* (*)() noexcept;
  const auto query = reinterpret_cast<Query>(dlsym(library, "ibmw_dds_comm_trace_query_v2"));
  const auto* api = query == nullptr ? nullptr : query();
  const bool valid = api != nullptr && api->abi_version == 2 &&
      api->struct_size == sizeof(*api) && api->reserved == 0;
  dlclose(library);
  return valid ? 0 : 1;
}
