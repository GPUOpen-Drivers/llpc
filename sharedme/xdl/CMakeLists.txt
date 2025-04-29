##
 #######################################################################################################################
 #
 #  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

cmake_minimum_required(VERSION 3.21)

project(SharedMeXdl LANGUAGES CXX)

set(LLPC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${LLPC_SOURCE_DIR}/cmake/CompilerFlags.cmake")

add_llvm_library(sharedme_xdl
  lib/LgcXdlDialect.cpp
  util/ElementType.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  CompilerUtils
  Core
  Scalar
  Support
  TransformUtils
)

target_include_directories(sharedme_xdl PUBLIC
  $<BUILD_INTERFACE:${LLPC_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sharedme_xdl PUBLIC llvm_dialects ${extra_llvm_libs} llpc_version)
set_compiler_options(sharedme_xdl)

set(SHAREDME_XDL_TABLEGEN_DEFINES)

# TableGen for dialects
include("${LLPC_SOURCE_DIR}/cmake/DialectsTablegen.cmake")
set_dialects_tablegen_exe(SHAREDME_XDL)

macro(sharedme_xdl_tablegen DIALECTNAME FILE OUTPUT_FILENAME)
  set(LLVM_TARGET_DEFINITIONS "${FILE}")
  set(TBLGEN_TARGET "${OUTPUT_FILENAME}TableGen")

  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/lgc")
  tablegen(SHAREDME_XDL "include/lgc/${OUTPUT_FILENAME}.h.inc"
      -gen-dialect-decls --dialect "${DIALECTNAME}" "${SHAREDME_XDL_TABLEGEN_DEFINES}"
      EXTRA_INCLUDES
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${LLPC_SOURCE_DIR}/imported/llvm-dialects/include
      )
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/lib")
  tablegen(SHAREDME_XDL "include/lib/${OUTPUT_FILENAME}.cpp.inc"
      -gen-dialect-defs --dialect "${DIALECTNAME}" "${SHAREDME_XDL_TABLEGEN_DEFINES}"
      EXTRA_INCLUDES
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${LLPC_SOURCE_DIR}/imported/llvm-dialects/include
      )
  add_public_tablegen_target(${TBLGEN_TARGET})

  add_dependencies(sharedme_xdl ${TBLGEN_TARGET})
  target_sources(sharedme_xdl PRIVATE ${FILE})
endmacro()

sharedme_xdl_tablegen(lgc.xdl include/lgc/LgcXdlDialect.td LgcXdlDialect)

if(LLPC_SHAREDME_XDL_BUILD_TEST)
  add_subdirectory(test)
endif()
