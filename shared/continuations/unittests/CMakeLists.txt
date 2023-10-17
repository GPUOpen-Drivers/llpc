# Continuations Unit tests.
# To execute all unit tests, run:
#   cmake --build . --target check-continuations-units

add_custom_target(ContinuationsUnitTests)
set_target_properties(ContinuationsUnitTests PROPERTIES FOLDER "Continuations Tests")

function(add_continuations_unittest test_dirname)
  add_unittest(ContinuationsUnitTests ${test_dirname} ${ARGN})
endfunction()

# Add a LIT target to execute all unit tests.
# Required by lit.site.cfg.py.in.
set(CONTINUATIONS_UNIT_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(CONTINUATIONS_UNIT_TEST_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Main config for unit tests.
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

add_lit_testsuite(check-continuations-units "Running the Continuations unit tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  ${exclude_from_check_all}
  DEPENDS
    ContinuationsUnitTests
)

add_continuations_unittest(ContinuationsUnitTargetTests
  RemainingArgumentDwordTests.cpp
)

target_link_libraries(ContinuationsUnitTargetTests PRIVATE
  LLVMContinuations
  LLVMCore
  LLVMSupport
)

set_compiler_options(ContinuationsUnitTargetTests)
