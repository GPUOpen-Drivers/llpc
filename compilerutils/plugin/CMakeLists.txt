set(LLVM_COMPILERUTILSPLUGIN_LINK_INTO_TOOLS ON CACHE BOOL "Link plugin into tools" FORCE)

add_llvm_pass_plugin(CompilerUtilsPlugin
  Plugin.cpp

  LINK_COMPONENTS
  Support
)

target_link_libraries(CompilerUtilsPlugin PRIVATE LLVMCompilerUtils)
set_compiler_options(CompilerUtilsPlugin)
