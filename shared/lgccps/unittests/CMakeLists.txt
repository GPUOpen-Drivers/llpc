# LgcCps Unit tests.
# To execute all unit tests, run:
#   cmake --build . --target check-lgccps-units

add_custom_target(LgcCpsUnitTests)
set_target_properties(LgcCpsUnitTests PROPERTIES FOLDER "LgcCps Tests")

function(add_lgccps_unittest test_dirname)
  add_unittest(LgcCpsUnitTests ${test_dirname} ${ARGN})
endfunction()

# Add a LIT target to execute all unit tests.
# Required by lit.site.cfg.py.in.
set(LGCCPS_UNIT_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LGCCPS_UNIT_TEST_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Main config for unit tests.
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

add_lit_testsuite(check-lgccps-units "Running the LgcCps unit tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  ${exclude_from_check_all}
  DEPENDS
    LgcCpsUnitTests
)

add_lgccps_unittest(LgcCpsUnitTargetTests
  RemainingArgumentDwordTests.cpp
)

target_link_libraries(LgcCpsUnitTargetTests PRIVATE
  LLVMLgcCps
)

set_compiler_options(LgcCpsUnitTargetTests)
