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
  lib/PreCoroutineLowering.cpp
  lib/DXILMetadata.cpp
  lib/DXILSupport.cpp
  lib/GpurtDialect.cpp
  lib/LegacyCleanupContinuations.cpp
  lib/LgcCpsDialect.cpp
  lib/LgcRtDialect.cpp
  lib/LowerAwait.cpp
  lib/LowerRaytracingPipeline.cpp
  lib/PassRegistry.inc
  lib/PayloadAccessQualifiers.cpp
  lib/RegisterBuffer.cpp
  lib/RemoveTypesMetadata.cpp
  lib/SaveContinuationState.cpp

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

target_include_directories(LLVMContinuations PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(LLVMContinuations PUBLIC llvm_dialects)
set_compiler_options(LLVMContinuations)

# TableGen for dialects
set(CONTINUATIONS_TABLEGEN_EXE llvm-dialects-tblgen)
set(CONTINUATIONS_TABLEGEN_TARGET llvm-dialects-tblgen)

macro(cont_tablegen DIALECTNAME FILE OUTPUT_FILENAME)
  set(LLVM_TARGET_DEFINITIONS "${FILE}")
  set(TBLGEN_TARGET "${OUTPUT_FILENAME}TableGen")

  tablegen(CONTINUATIONS "${OUTPUT_FILENAME}.h.inc" -gen-dialect-decls --dialect "${DIALECTNAME}" "${CONTINUATIONS_TABLEGEN_DEFINES}"
      EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
  tablegen(CONTINUATIONS "${OUTPUT_FILENAME}.cpp.inc" -gen-dialect-defs --dialect "${DIALECTNAME}" "${CONTINUATIONS_TABLEGEN_DEFINES}"
      EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../../imported/llvm-dialects/include)
  add_public_tablegen_target(${TBLGEN_TARGET})

  add_dependencies(LLVMContinuations ${TBLGEN_TARGET})
endmacro()

cont_tablegen(continuations include/continuations/ContinuationsDialect.td ContinuationsDialect)
cont_tablegen(lgc.cps include/lgc/LgcCpsDialect.td LgcCpsDialect)
cont_tablegen(lgc.rt include/lgc/LgcRtDialect.td LgcRtDialect)
cont_tablegen(lgc.gpurt include/lgc/GpurtDialect.td GpurtDialect)

target_compile_features(LLVMContinuations PUBLIC cxx_std_17)
set_target_properties(LLVMContinuations PROPERTIES CXX_EXTENSIONS OFF)

add_subdirectory(plugin)
add_subdirectory(test)
if(LLPC_BUILD_TESTS)
  add_subdirectory(unittests)
endif()
