cmake_minimum_required(VERSION 3.13.4)

project(CompilerUtils LANGUAGES CXX)

function(set_compiler_options PROJECT_NAME)
  # Output with color if in terminal: https://github.com/ninja-build/ninja/wiki/FAQ
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fdiagnostics-color=always)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options("${PROJECT_NAME}" PRIVATE -fcolor-diagnostics)
  endif()
endfunction()

add_llvm_library(LLVMCompilerUtils
  lib/ArgPromotion.cpp
  lib/CompilerUtils.cpp
  lib/DxilToLlvm.cpp
  lib/TypeLowering.cpp
  lib/TypesMetadata.cpp
  lib/ValueOriginTracking.cpp
  lib/ValueOriginTrackingTestPass.cpp
  lib/ValueSpecialization.cpp
  lib/ValueSpecializationTestPass.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  Support
  TransformUtils
)

target_include_directories(LLVMCompilerUtils PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(LLVMCompilerUtils PUBLIC llvm_dialects)
set_compiler_options(LLVMCompilerUtils)

target_compile_features(LLVMCompilerUtils PUBLIC cxx_std_17)
set_target_properties(LLVMCompilerUtils PROPERTIES CXX_EXTENSIONS OFF)

add_subdirectory(plugin)
add_subdirectory(tool/cross-module-inline)
add_subdirectory(test)
