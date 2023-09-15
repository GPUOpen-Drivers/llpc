set(LLVM_CONTINUATIONSPLUGIN_LINK_INTO_TOOLS ON CACHE BOOL "Link plugin into tools" FORCE)

add_llvm_pass_plugin(ContinuationsPlugin
  Plugin.cpp
)

target_link_libraries(ContinuationsPlugin PRIVATE LLVMContinuations)
set_compiler_options(ContinuationsPlugin)
