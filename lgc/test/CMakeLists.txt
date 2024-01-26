##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to
 #  deal in the Software without restriction, including without limitation the
 #  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 #  sell copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 #  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 #  IN THE SOFTWARE.
 #
 #######################################################################################################################

set(LGC_TEST_DEPENDS lgc lgcdis FileCheck count not llvm-mc llvm-readelf llvm-objdump)
add_custom_target(lgc-test-depends DEPENDS ${LGC_TEST_DEPENDS})
set_target_properties(lgc-test-depends PROPERTIES FOLDER "Tests")

# required by lit.site.cfg.py.in
set(LGC_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# required by configure_lit_site_cfg
set(LLVM_LIT_OUTPUT_DIR ${LLVM_TOOLS_BINARY_DIR})

# When cross-compiling (or compiling a release tablegen), external llvm projects are defined but other projects are not.
# We expect that tests are not run in a cmake configuration where llpc_version does not exist, so we only use its
# definitions when it is available.
if (TARGET llpc_version)
  get_target_property(LGC_LIT_DEFINITIONS LLVMlgc COMPILE_DEFINITIONS)
  get_target_property(LIT_DEFINITIONS llpc_version INTERFACE_COMPILE_DEFINITIONS)
  list(APPEND LIT_DEFINITIONS ${LGC_LIT_DEFINITIONS})
endif()

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

add_lit_testsuite(check-lgc "Running the LGC regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  ${exclude_from_check_all}
  DEPENDS ${LGC_TEST_DEPENDS}
)
set_target_properties(check-lgc PROPERTIES FOLDER "Tests")

add_lit_testsuites(LGC ${CMAKE_CURRENT_SOURCE_DIR}
  ${exclude_from_check_all}
  DEPENDS ${LGC_TEST_DEPENDS}
)
