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
  lib/CleanupContinuations.cpp
  lib/ContinuationsDialect.cpp
  lib/ContinuationsUtil.cpp
  lib/DXILCont.cpp
  lib/DXILContIntrinsicPrepare.cpp
  lib/DXILContLgcRtOpConverter.cpp
  lib/DXILContPostProcess.cpp
  lib/DXILContPreCoroutine.cpp
  lib/DXILMetadata.cpp
  lib/DXILSupport.cpp
  lib/LegacyCleanupContinuations.cpp
  lib/LowerAwait.cpp
  lib/LowerRaytracingPipeline.cpp
  lib/PassRegistry.inc
  lib/PayloadAccessQualifiers.cpp
  lib/RegisterBuffer.cpp
  lib/RemoveTypesMetadata.cpp
  lib/SaveContinuationState.cpp

  DEPENDS
  intrinsics_gen
  LLVMAnalysis

  LINK_COMPONENTS
  Analysis
  Core
  Coroutines
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

target_link_libraries(LLVMContinuations PUBLIC llvm_dialects PRIVATE LLVMLgcRt LLVMLgcCps)
set_compiler_options(LLVMContinuations)

# TableGen for continuations dialect
set(CONTINUATIONS_TABLEGEN_EXE $<TARGET_FILE:llvm-dialects-tblgen>)
set(CONTINUATIONS_TABLEGEN_TARGET llvm-dialects-tblgen)
set(LLVM_TARGET_DEFINITIONS include/continuations/ContinuationsDialect.td)
set(LLVM_TARGET_DEPENDS continuations)

add_custom_command(
    OUTPUT continuations
    COMMAND ${CMAKE_COMMAND} -E make_directory continuations)

tablegen(CONTINUATIONS continuations/ContinuationsDialect.h.inc -gen-dialect-decls --dialect continuations
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
tablegen(CONTINUATIONS ContinuationsDialect.cpp.inc -gen-dialect-defs --dialect continuations
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
add_public_tablegen_target(ContinuationsDialectTableGen)

add_dependencies(LLVMContinuations ContinuationsDialectTableGen LgcRtDialectTableGen LgcCpsDialectTableGen)

target_compile_features(LLVMContinuations PUBLIC cxx_std_17)
set_target_properties(LLVMContinuations PROPERTIES CXX_EXTENSIONS OFF)

add_subdirectory(plugin)
add_subdirectory(test)
