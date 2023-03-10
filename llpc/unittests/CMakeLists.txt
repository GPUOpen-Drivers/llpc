##
 #######################################################################################################################
 #
 #  Copyright (c) 2021 Google LLC. All Rights Reserved.
 #
 #  Permission is hereby granted, free of charge, to any person obtaining a copy
 #  of this software and associated documentation files (the "Software"), to deal
 #  in the Software without restriction, including without limitation the rights
 #  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 #  copies of the Software, and to permit persons to whom the Software is
 #  furnished to do so, subject to the following conditions:
 #
 #  The above copyright notice and this permission notice shall be included in all
 #  copies or substantial portions of the Software.
 #
 #  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 #  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 #  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 #  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 #  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 #  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 #  SOFTWARE.
 #
 #######################################################################################################################

 # LLPC Unit tests.
add_custom_target(LlpcUnitTests)
set_target_properties(LlpcUnitTests PROPERTIES FOLDER "LLPC Tests")

# To execute all LLPC unit tests, run:
#   cmake --build . --target check-amdllpc-units

include(LLVMConfig)

# Required to use LIT on Windows.
find_package(Python3 ${LLVM_MINIMUM_PYTHON_VERSION} REQUIRED
  COMPONENTS Interpreter)

# Find all the LLVM libraries necessary to use the LLVM components in gtest/gmock.
llvm_map_components_to_libnames(llvm_testing_support_lib TestingSupport)
set(LLVM_GTEST_LIBS llvm_gtest llvm_gtest_main ${llvm_testing_support_lib})
if(LLVM_PTHREAD_LIB)
  list(APPEND LLVM_GTEST_LIBS pthread)
endif()

# Based on add_unittest in llvm/cmake/modules/AddLLVM.cmake. Note that because LLPC is not an
# (internal) LLVM project in the CMake sense, we cannot use the LLVM implementation here.
function(add_llpc_unittest_impl test_suite test_name)
  add_executable(${test_name})
  add_dependencies(${test_suite} ${test_name})
  target_sources(${test_name} PRIVATE ${ARGN})

  set(outdir ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR})
  set_output_directory(${test_name} BINARY_DIR ${outdir} LIBRARY_DIR ${outdir})

  target_link_libraries(${test_name} PRIVATE ${LLVM_GTEST_LIBS} llpc_standalone_compiler)
  target_include_directories(${test_name} PRIVATE
    ${LLVM_INCLUDE_DIRS}  # This is necessary to discover the auto-generated llvm-config.h header.
  )
  set_compiler_options(${test_name} ${LLPC_ENABLE_WERROR})

  get_target_property(test_suite_folder ${test_suite} FOLDER)
  if(test_suite_folder)
    set_property(TARGET ${test_name} PROPERTY FOLDER "${test_suite_folder}")
  endif()
endfunction()

function(add_llpc_unittest test_dirname)
  add_llpc_unittest_impl(LlpcUnitTests ${test_dirname} ${ARGN})
endfunction()

add_subdirectory(context)
add_subdirectory(standaloneCompiler)
add_subdirectory(util)
add_subdirectory(vfx)

# Add a LIT target to execute all unit tests.
# Required by lit.site.cfg.py.in.
set(AMDLLPC_UNIT_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
# Required by configure_lit_site_cfg.
set(LLVM_LIT_OUTPUT_DIR ${LLVM_TOOLS_BINARY_DIR})

# Main config for unit tests.
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

add_lit_testsuite(check-amdllpc-units "Running the AMDLLPC unit tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS
    LlpcUnitTests
)
