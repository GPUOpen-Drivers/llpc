##
 #######################################################################################################################
 #
 #  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

# Only build if llpc is built as part of xgl
if(NOT LLPC_IS_STANDALONE)
  find_package(Python3 ${LLVM_MINIMUM_PYTHON_VERSION} REQUIRED
    COMPONENTS Interpreter)

  list(APPEND CMAKE_MODULE_PATH
    "${XGL_LLVM_BUILD_PATH}/lib/cmake/llvm"
    "${XGL_LLVM_BUILD_PATH}/${CMAKE_CFG_INTDIR}/lib/cmake/llvm" # Workaround for VS generator with older LLVM.
  )
  include(LLVMConfig)

  # required by lit.site.cfg.py.in.
  set(LLPC_AMBER_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(LLPC_AMBER_TEST_ICD_NAME ${CMAKE_BINARY_DIR}/icd/amdvlk${TARGET_ARCHITECTURE_BITS}${CMAKE_SHARED_LIBRARY_SUFFIX})
  # required by configure_lit_site_cfg.
  set(LLVM_LIT_OUTPUT_DIR ${LLVM_TOOLS_BINARY_DIR})
  # Main config for shaderdb tests.
  configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
    MAIN_CONFIG
      ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  )

  add_lit_testsuite(check-amber "Running the llpc amber regression tests"
    ${CMAKE_CURRENT_BINARY_DIR}
    ${exclude_from_check_all}
    DEPENDS
      FileCheck count not xgl
  )
endif()
