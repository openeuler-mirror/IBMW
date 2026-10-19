# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Every consumer of a DDS type must see the same codecs before instantiating
# TypeSupport. Include them from the generated type header itself.
file(READ "${TYPE_HEADER}" contents)
set(codec_include "#include \"${JSON_HEADER}\"")
string(FIND "${contents}" "${codec_include}" position)
if(position EQUAL -1)
  file(APPEND "${TYPE_HEADER}" "\n${codec_include}\n")
endif()
