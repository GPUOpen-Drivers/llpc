// Check the resource layout option '--resource-layout-scheme'

// BEGIN_SHADERTEST
// RUN: amdllpc -enable-opaque-pointers=false -v %gfxip --resource-layout-scheme=indirect %s | FileCheck -check-prefix=SHADERTEST %s
// RUN: amdllpc -enable-opaque-pointers=true -v %gfxip --resource-layout-scheme=indirect %s | FileCheck -check-prefix=SHADERTEST %s

// SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
// SHADERTEST: call {{i8 addrspace\(4\)\*|ptr addrspace\(4\)}} @lgc.descriptor.table.addr(i32 6, i32 6, i32 1, i32 1, i32 -1)

// SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
// %descTable2 is for uniform buf, it should be %descTable1 in compact mode.
// SHADERTEST: define dllexport amdgpu_ps { <4 x float> } @_amdgpu_ps_main(i32 inreg %{{[^,]*}}, i32 inreg %{{[^,]*}}, i32 inreg %descTable2,
// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450
layout(location = 0) out highp vec4 o_color;
layout(set = 1, binding = 1) uniform buf
{
  vec4 color;
};

void main()
{
  o_color          = color;
}

