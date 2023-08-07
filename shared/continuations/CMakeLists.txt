##
 #######################################################################################################################
 #
 #  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

cmake_minimum_required(VERSION 3.13.4)

project(Continuations LANGUAGES CXX)

function(set_compiler_options PROJECT_NAME)
  # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fdiagnostics-color=always)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fcolor-diagnostics)
  endif()
endfunction()

add_llvm_library(LLVMContinuations
  lib/LgcCpsDialect.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  IPO
  Scalar
  Support
  TransformUtils
)

target_include_directories(LLVMContinuations PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(LLVMContinuations PUBLIC llvm_dialects)
set_compiler_options(LLVMContinuations)

set(CONTINUATIONS_TABLEGEN_EXE ${LLVM_TOOLS_BINARY_DIR}/llvm-dialects-tblgen)
set(CONTINUATIONS_TABLEGEN_TARGET llvm-dialects-tblgen)
set(LLVM_TARGET_DEFINITIONS include/LgcCpsDialect.td)
set(LLVM_TARGET_DEPENDS continuations)

add_custom_command(
    OUTPUT continuations
    COMMAND ${CMAKE_COMMAND} -E make_directory continuations)

tablegen(CONTINUATIONS continuations/LgcCpsDialect.h.inc -gen-dialect-decls --dialect lgc.cps
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
tablegen(CONTINUATIONS LgcCpsDialect.cpp.inc -gen-dialect-defs --dialect lgc.cps
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
add_public_tablegen_target(LgcCpsDialectTableGen)

add_dependencies(LLVMContinuations LgcCpsDialectTableGen)

target_compile_features(LLVMContinuations PUBLIC cxx_std_17)
set_target_properties(LLVMContinuations PROPERTIES CXX_EXTENSIONS OFF)

if(LLPC_BUILD_TESTS)
  add_subdirectory(unittests)
endif()
