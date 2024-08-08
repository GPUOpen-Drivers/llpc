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

add_llvm_library(LLVMRaytracing
  lib/CleanupContinuations.cpp
  lib/Continuations.cpp
  lib/ContinuationsLint.cpp
  lib/CpsStackLowering.cpp
  lib/DXILContIntrinsicPrepare.cpp
  lib/DXILContLgcRtOpConverter.cpp
  lib/DXILContPostProcess.cpp
  lib/DXILSupport.cpp
  lib/GpurtContext.cpp
  lib/GpurtDialect.cpp
  lib/LegacyCleanupContinuations.cpp
  lib/ContinuationsStatsReport.cpp
  lib/LgcCpsDialect.cpp
  lib/LgcCpsJumpInliner.cpp
  lib/LgcIlCpsDialect.cpp
  lib/LgcRtDialect.cpp
  lib/LgcRtqDialect.cpp
  lib/LowerAwait.cpp
  lib/LowerRayQuery.cpp
  lib/LowerRaytracingPipeline.cpp
  lib/PassRegistry.inc
  lib/PayloadAccessQualifiers.cpp
  lib/RemoveTypesMetadata.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
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
if (EXISTS ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
  set(RAYTRACING_TABLEGEN_EXE ${LLVM_TOOLS_BINARY_PATH}/llvm-dialects-tblgen)
else()
  set(RAYTRACING_TABLEGEN_EXE $<TARGET_FILE:llvm-dialects-tblgen>)
endif()
set(RAYTRACING_TABLEGEN_TARGET llvm-dialects-tblgen)

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
  add_subdirectory(test)
  add_subdirectory(unittests)

  # Temporary aliases -- to be removed when Vulkan CI and DXCP have been updated.
  add_custom_target(check-continuations DEPENDS check-llvmraytracing)
  add_custom_target(check-continuations-units DEPENDS check-llvmraytracing-units)
endif()

# Temporary alias -- to be removed when Vulkan CI and DXCP have been updated.
if (LLPC_RAYTRACING_ADD_TRANSITION_TARGETS)
  add_library(LLVMContinuations ALIAS LLVMRaytracing)
endif()
