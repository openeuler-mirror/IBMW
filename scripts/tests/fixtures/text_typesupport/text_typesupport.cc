// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <stdexcept>
#include "Inheritance.hpp"
#include "Names.hpp"

void Check(bool value) { if (!value) throw std::runtime_error("JSON codec regression"); }
int main() {
  inheritance::Derived original;
  original.base_value(4);
  original.middle_value(5);
  original.derived_value(6);
  original.bounded_values({7, 8});
  const auto json = IbmwToJson(original);
  Check(json == ibmw::util::text::Json({{"base_value", 4}, {"middle_value", 5},
                                      {"derived_value", 6}, {"bounded_values", {7, 8}}}));
  inheritance::Derived restored;
  IbmwFromJson(json, restored);
  Check(restored == original);
  IbmwFromJson({{"base_value", 9}}, restored);
  Check(restored.base_value() == 9 && restored.derived_value() == 6);
  bool rejected = false;
  try { IbmwFromJson({{"bounded_values", {1, 2, 3}}}, restored); }
  catch (const std::exception&) { rejected = true; }
  Check(rejected);
  const auto check_struct = [](auto object) {
    IbmwFromJson({{"member_value", 42}}, object);
    Check(IbmwToJson(object).at("member_value") == 42);
  };
  check_struct(::value{});
  check_struct(names::value{});
  check_struct(names::output{});
  check_struct(names::result{});
  enums::value enumeration;
  IbmwFromJson("second", enumeration);
  Check(enumeration == enums::value::second && IbmwToJson(enumeration) == "second");
  enums::output output;
  IbmwFromJson("third", output);
  Check(IbmwToJson(output) == "third");
  enums::result result;
  IbmwFromJson("fifth", result);
  Check(IbmwToJson(result) == "fifth");
}
