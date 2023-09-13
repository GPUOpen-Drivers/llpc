##
 #######################################################################################################################
 #
 #  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

project(LgcRt LANGUAGES CXX)

function(set_compiler_options PROJECT_NAME)
  # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fdiagnostics-color=always)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options("${PROJECT_NAME}" PRIVATE -fcolor-diagnostics)
  endif()
endfunction()

add_llvm_library(LLVMLgcRt
  lib/LgcRtDialect.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Core
  Support
)

target_include_directories(LLVMLgcRt PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(LLVMLgcRt PUBLIC llvm_dialects)
set_compiler_options(LLVMLgcRt)

# TableGen for the lgc.rt dialect
set(LGCRT_TABLEGEN_EXE ${LLVM_TOOLS_BINARY_DIR}/llvm-dialects-tblgen)
set(LGCRT_TABLEGEN_TARGET llvm-dialects-tblgen)
set(LLVM_TARGET_DEFINITIONS include/lgcrt/LgcRtDialect.td)
set(LLVM_TARGET_DEPENDS lgcrt)

add_custom_command(
    OUTPUT lgcrt
    COMMAND ${CMAKE_COMMAND} -E make_directory lgcrt)

tablegen(LGCRT lgcrt/LgcRtDialect.h.inc -gen-dialect-decls --dialect lgc.rt ${LGCRT_TABLEGEN_DEFINES}
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
tablegen(LGCRT LgcRtDialect.cpp.inc -gen-dialect-defs --dialect lgc.rt ${LGCRT_TABLEGEN_DEFINES}
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
add_public_tablegen_target(LgcRtDialectTableGen)

add_dependencies(LLVMLgcRt LgcRtDialectTableGen)

target_compile_features(LLVMLgcRt PUBLIC cxx_std_17)
set_target_properties(LLVMLgcRt PROPERTIES CXX_EXTENSIONS OFF)
