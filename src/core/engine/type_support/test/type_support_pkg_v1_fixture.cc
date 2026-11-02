// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstddef>
#include <atomic>
#include <iterator>
#include <utility>

#include "type_support/type_support_pkg_main.h"

namespace {

std::atomic<unsigned> load_count{0};
void (*load_hook)(void*) = nullptr;
void* load_hook_context = nullptr;

template <size_t Size>
constexpr ibmw_string_view_t View(const char (&text)[Size]) {
  return ibmw_string_view_t{.str = text, .len = Size - 1};
}

ibmw_string_view_t TypeName(void*) {
  return View("json:fixture.Message");
}

void* Create(void*) {
  return new int(0);
}

void Destroy(void*, void* value) {
  delete static_cast<int*>(value);
}

void Copy(void*, const void* from, void* to) {
  *static_cast<int*>(to) = *static_cast<const int*>(from);
}

void Move(void*, void* from, void* to) {
  *static_cast<int*>(to) = std::move(*static_cast<int*>(from));
}

bool Serialize(
    void*, ibmw_string_view_t, const void*,
    const ibmw_buffer_array_allocator_t*, ibmw_buffer_array_t*) {
  return false;
}

bool Deserialize(
    void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) {
  return false;
}

size_t SerializationTypeCount(void*) {
  return 1;
}

const ibmw_string_view_t* SerializationTypes(void*) {
  static constexpr ibmw_string_view_t types[]{View("json")};
  return types;
}

const void* CustomTypeSupport(void*) {
  return nullptr;
}

const ibmw_type_support_base_t type_support{
    .type_name = TypeName,
    .create = Create,
    .destroy = Destroy,
    .copy = Copy,
    .move = Move,
    .serialize = Serialize,
    .deserialize = Deserialize,
    .serialization_types_supported_num = SerializationTypeCount,
    .serialization_types_supported_list = SerializationTypes,
    .custom_type_support_ptr = CustomTypeSupport,
    .ctx = nullptr};
#if defined(IBMW_TYPE_SUPPORT_FIXTURE_NULL_TYPE)
const ibmw_type_support_base_t* type_support_array[]{nullptr};
#elif defined(IBMW_TYPE_SUPPORT_FIXTURE_DUPLICATE_TYPE)
const ibmw_type_support_base_t* type_support_array[]{&type_support, &type_support};
#else
const ibmw_type_support_base_t* type_support_array[]{&type_support};
#endif
const ibmw_schema_support_v1_t schema{
#if defined(IBMW_TYPE_SUPPORT_FIXTURE_BAD_VERSION)
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1 + 1,
#else
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
#endif
    .struct_size =
#if defined(IBMW_TYPE_SUPPORT_FIXTURE_BAD_SIZE)
        IBMW_SCHEMA_SUPPORT_V1_REQUIRED_SIZE - 1U,
#else
        sizeof(ibmw_schema_support_v1_t),
#endif
    .type_name = View("json:fixture.Message"),
    .serialization_type = View("json"),
    .schema_name = View("fixture.Message"),
    .schema_encoding = View("jsonschema"),
    .schema_data = View("{}"),
    .schema_hash = View("fnv1a64:08f44b07b5901a25"),
    .reserved = {}};
const ibmw_schema_support_v1_t* schema_support_array[]{&schema};
const ibmw_schema_support_package_v1_t schema_support_package{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size =
#if defined(IBMW_TYPE_SUPPORT_FIXTURE_BAD_PACKAGE_SIZE)
        IBMW_SCHEMA_SUPPORT_PACKAGE_V1_REQUIRED_SIZE - 1U,
#else
        sizeof(ibmw_schema_support_package_v1_t),
#endif
    .entry_count = 1,
    .entries = schema_support_array,
    .reserved = {}};

}  // namespace

extern "C" {

IBMW_TYPE_SUPPORT_PKG_EXPORT void SetTypeSupportLoadHook(void (*hook)(void*), void* context) {
  load_hook = hook;
  load_hook_context = context;
  load_count = 0;
}

IBMW_TYPE_SUPPORT_PKG_EXPORT unsigned TypeSupportLoadCount() { return load_count.load(); }

const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  ++load_count;
  if (load_hook) load_hook(load_hook_context);
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
      .struct_size = sizeof(ibmw_type_support_package_v1_t),
      .entry_count = std::size(type_support_array),
      .entries = type_support_array,
      .schema_support = &schema_support_package,
      .reserved = {}};
  return &package;
}

}  // extern "C"
