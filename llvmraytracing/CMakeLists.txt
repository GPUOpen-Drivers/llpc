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

cmake_minimum_required(VERSION 3.13.4)

project(LlvmRaytracing LANGUAGES CXX)

function(set_compiler_options PROJECT_NAME)
  # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fdiagnostics-color=always)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options("${PROJECT_NAME}" PRIVATE -fcolor-diagnostics)
  endif()
endfunction()

option(LLVMRAYTRACING_BUILD_TESTS "Build raytracing tests")

set(LLVMRaytracingLibFiles
  lib/CleanupContinuations.cpp
  lib/ContStateBuilder.cpp
  lib/Continuations.cpp
  lib/ContinuationsLint.cpp
  lib/CpsStackLowering.cpp
  lib/DXILContPrepareGpurtLibrary.cpp
  lib/DXILContLgcRtOpConverter.cpp
  lib/DXILContPostProcess.cpp
  lib/GpurtContext.cpp
  lib/GpurtDialect.cpp
  lib/LgcCpsDialect.cpp
  lib/LgcCpsJumpInliner.cpp
  lib/LgcIlCpsDialect.cpp
  lib/LgcRtDialect.cpp
  lib/LgcRtqDialect.cpp
  lib/LowerAwait.cpp
  lib/LowerRayQuery.cpp
  lib/LowerRaytracingPipeline.cpp
  lib/PassRegistry.inc
  lib/PipelineState.cpp
  lib/PayloadAccessQualifiers.cpp
  lib/RematSupport.cpp
  lib/RemoveTypesMetadata.cpp
  lib/SpecializeDriverShaders.cpp
  lib/StatisticsReporter.cpp)

add_llvm_library(LLVMRaytracing
  ${LLVMRaytracingLibFiles}

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  BinaryFormat
  Core
  Coroutines
  IPO
  Scalar
  Support
  TransformUtils
)

target_include_directories(LLVMRaytracing PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>
)

llvm_map_components_to_libnames(extra_llvm_libs CompilerUtils)

target_link_libraries(LLVMRaytracing PUBLIC llvm_dialects ${extra_llvm_libs} llpc_version)
set_compiler_options(LLVMRaytracing)

# TableGen for dialects
include(../cmake/DialectsTablegen.cmake)
set_dialects_tablegen_exe(RAYTRACING)

macro(raytracing_tablegen DIALECTNAME FILE OUTPUT_FILENAME)
  set(LLVM_TARGET_DEFINITIONS "${FILE}")
  set(TBLGEN_TARGET "${OUTPUT_FILENAME}TableGen")

  tablegen(RAYTRACING "${OUTPUT_FILENAME}.h.inc" -gen-dialect-decls --dialect "${DIALECTNAME}" "${RAYTRACING_TABLEGEN_DEFINES}"
      EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include)
  tablegen(RAYTRACING "${OUTPUT_FILENAME}.cpp.inc" -gen-dialect-defs --dialect "${DIALECTNAME}" "${RAYTRACING_TABLEGEN_DEFINES}"
      EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../imported/llvm-dialects/include)
  add_public_tablegen_target(${TBLGEN_TARGET})

  add_dependencies(LLVMRaytracing ${TBLGEN_TARGET})
  target_sources(LLVMRaytracing PRIVATE ${FILE})
endmacro()

raytracing_tablegen(lgc.ilcps include/lgc/LgcIlCpsDialect.td LgcIlCpsDialect)
raytracing_tablegen(lgc.cps include/lgc/LgcCpsDialect.td LgcCpsDialect)
raytracing_tablegen(lgc.rt include/lgc/LgcRtDialect.td LgcRtDialect)
raytracing_tablegen(lgc.rtq include/lgc/LgcRtqDialect.td LgcRtqDialect)
raytracing_tablegen(lgc.gpurt include/lgc/GpurtDialect.td GpurtDialect)

target_compile_features(LLVMRaytracing PUBLIC cxx_std_17)
set_target_properties(LLVMRaytracing PROPERTIES CXX_EXTENSIONS OFF)

add_subdirectory(plugin)

if(CONTINUATIONS_BUILD_TESTS)
  message(WARNING "Deprecated flag CONTINUATIONS_BUILD_TESTS used; use LLVMRAYTRACING_BUILD_TESTS instead")
  set(LLVMRAYTRACING_BUILD_TESTS ON)
endif()
if(LLVMRAYTRACING_BUILD_TESTS)
  if (TARGET llpc_version)
    get_target_property(LLVMRAYTRACING_LIT_DEFINITIONS LLVMRaytracing COMPILE_DEFINITIONS)
    get_target_property(LIT_DEFINITIONS llpc_version INTERFACE_COMPILE_DEFINITIONS)
    list(APPEND LIT_DEFINITIONS ${LLVMRAYTRACING_LIT_DEFINITIONS})
  endif()

  add_subdirectory(test)
  add_subdirectory(unittests)

  # Temporary aliases -- to be removed when Vulkan CI and DXCP have been updated.
  add_custom_target(check-continuations DEPENDS check-llvmraytracing)
  add_custom_target(check-continuations-units DEPENDS check-llvmraytracing-units)
endif()

