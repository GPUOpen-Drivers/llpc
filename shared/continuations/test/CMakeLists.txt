set(CONTINUATIONS_TEST_DEPENDS opt FileCheck count not)
add_custom_target(continuations-test-depends DEPENDS ${CONTINUATIONS_TEST_DEPENDS})
set_target_properties(continuations-test-depends PROPERTIES FOLDER "Tests")

# required by lit.site.cfg.py.in
set(CONTINUATIONS_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# required by configure_lit_site_cfg
set(LLVM_LIT_OUTPUT_DIR ${LLVM_TOOLS_BINARY_DIR})
configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

add_lit_testsuite(check-continuations "Running the continuations regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  ${exclude_from_check_all}
  DEPENDS ${CONTINUATIONS_TEST_DEPENDS}
)
set_target_properties(check-continuations PROPERTIES FOLDER "Tests")

add_lit_testsuites(CONTINUATIONS ${CMAKE_CURRENT_SOURCE_DIR}
  ${exclude_from_check_all}
  DEPENDS ${CONTINUATIONS_TEST_DEPENDS}
)
