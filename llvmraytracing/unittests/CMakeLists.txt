# Raytracing Unit tests.
# To execute all unit tests, run:
#   cmake --build . --target check-raytracing-units

add_custom_target(LlvmRaytracingUnitTests)
set_target_properties(LlvmRaytracingUnitTests PROPERTIES FOLDER "Raytracing Tests")

function(add_llvmraytracing_unittest test_dirname)
  add_unittest(LlvmRaytracingUnitTests ${test_dirname} ${ARGN})
endfunction()

# Add a LIT target to execute all unit tests.
# Required by lit.site.cfg.py.in.
set(LLVMRAYTRACING_UNIT_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LLVMRAYTRACING_UNIT_TEST_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Main config for unit tests.
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

add_lit_testsuite(check-llvmraytracing-units "Running the LLVM Raytracing unit tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  ${exclude_from_check_all}
  DEPENDS
    LlvmRaytracingUnitTests
)

add_llvmraytracing_unittest(LlvmRaytracingUnitTargetTests
  RemainingArgumentDwordTests.cpp
)

target_link_libraries(LlvmRaytracingUnitTargetTests PRIVATE
  LLVMRaytracing
  LLVMCore
  LLVMSupport
)

set_compiler_options(LlvmRaytracingUnitTargetTests)
