#include <engine/type_support_pkg_loader.h>

#include <cpp/util/schema_support.h>
#include <cpp/util/type_support.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "installed TypeSupport failure: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "expected the installed sidecar path");
  ibmw::runtime::core::util::TypeSupportPkgLoader loader;
  loader.LoadTypeSupportPkg(argv[1]);

  const auto types = loader.GetTypeSupportArray();
  Require(types.size() == 1, "unexpected TypeSupport entry count");
  const ibmw::util::TypeSupportRef type(types.front());
  Require(type.TypeName() == "json:installed_typesupport.Record",
          "unexpected type name");
  Require(type.DefaultSerializationType() == "json",
          "unexpected serialization type");
  void* value = type.Create();
  Require(value != nullptr, "create callback returned null");
  type.Destroy(value);

  const auto schemas = loader.GetSchemaSupportArray();
  Require(schemas.size() == 1, "unexpected SchemaSupport entry count");
  const ibmw::util::SchemaSupportRef schema(schemas.front());
  Require(schema.TypeName() == type.TypeName(),
          "TypeSupport and SchemaSupport type names differ");
  Require(schema.SerializationType() == "json",
          "unexpected schema serialization type");
  Require(schema.SchemaName() == "installed_typesupport.Record",
          "unexpected schema name");
  Require(schema.SchemaEncoding() == "jsonschema",
          "unexpected schema encoding");
  Require(!schema.SchemaData().empty(), "schema data is empty");
  Require(schema.SchemaHash() ==
              ibmw::util::ComputeSchemaHash(schema.SchemaData()),
          "schema hash does not match schema data");
  std::cout << "PASS installed TypeSupport and SchemaSupport\n";
}
