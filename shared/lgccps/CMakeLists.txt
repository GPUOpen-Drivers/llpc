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

project(LgcCps LANGUAGES CXX)

function(set_compiler_options PROJECT_NAME)
  # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fdiagnostics-color=always)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fcolor-diagnostics)
  endif()
endfunction()

add_llvm_library(LLVMLgcCps
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

target_include_directories(LLVMLgcCps PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(LLVMLgcCps PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lib)

target_link_libraries(LLVMLgcCps PUBLIC llvm_dialects)
set_compiler_options(LLVMLgcCps)

set(LGCCPS_TABLEGEN_EXE $<TARGET_FILE:llvm-dialects-tblgen>)
set(LGCCPS_TABLEGEN_TARGET llvm-dialects-tblgen)
set(LLVM_TARGET_DEFINITIONS include/lgccps/LgcCpsDialect.td)
set(LLVM_TARGET_DEPENDS lgccps)

add_custom_command(
    OUTPUT lgccps
    COMMAND ${CMAKE_COMMAND} -E make_directory include/lgccps lib)

tablegen(LGCCPS include/lgccps/LgcCpsDialect.h.inc -gen-dialect-decls --dialect lgc.cps
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
tablegen(LGCCPS lib/LgcCpsDialect.cpp.inc -gen-dialect-defs --dialect lgc.cps
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
add_public_tablegen_target(LgcCpsDialectTableGen)

add_dependencies(LLVMLgcCps LgcCpsDialectTableGen)

target_compile_features(LLVMLgcCps PUBLIC cxx_std_17)
set_target_properties(LLVMLgcCps PROPERTIES CXX_EXTENSIONS OFF)

if(LLPC_BUILD_TESTS)
  add_subdirectory(unittests)
endif()
