set(LLVM_RAYTRACINGPLUGIN_LINK_INTO_TOOLS ON CACHE BOOL "Link raytracing plugin into tools" FORCE)

add_llvm_pass_plugin(RaytracingPlugin
  Plugin.cpp

  LINK_COMPONENTS
  Support
)

target_link_libraries(RaytracingPlugin PRIVATE LLVMRaytracing)
set_compiler_options(RaytracingPlugin)
