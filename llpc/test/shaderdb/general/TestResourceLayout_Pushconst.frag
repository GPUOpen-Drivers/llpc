// Check the resource layout option '--resource-layout-scheme'

// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip --resource-layout-scheme=indirect -enable-opaque-pointers=true %s | FileCheck -check-prefix=SHADERTEST %s
// SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
// SHADERTEST: [[Desc:%[0-9]*]] = call ptr addrspace(4) (...) @lgc.create.load.push.constants.ptr.p4()
// SHADERTEST: [[Value:%[0-9]*]] = load <4 x float>, ptr addrspace(4) [[Desc]], align 16
// SHADERTEST: call void (...) @lgc.create.write.generic.output(<4 x float> [[Value]], i32 0, i32 0, i32 0, i32 0, i32 0, i32 undef)

// SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
// SHADERTEST: [[Desc:%[0-9]*]] = call ptr addrspace(4) @lgc.descriptor.table.addr(i32 9, i32 9, i32 -1, i32 0, i32 -1)
// SHADERTEST: [[Value:%[0-9]*]] = load <4 x float>, ptr addrspace(4) [[Desc]], align 16
// SHADERTEST: call void @lgc.output.export.generic.i32.i32.v4f32(i32 0, i32 0, <4 x float> [[Value]])

// SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST: define dllexport amdgpu_ps { <4 x float> } @_amdgpu_ps_main(i32 inreg %{{[^,]*}}, i32 inreg %{{[^,]*}}, i32 inreg %descTable0,
// SHADERTEST: [[Addr0:%[0-9]*]] = zext i32 %descTable0 to i64
// SHADERTEST: [[Addr1:%[0-9]*]] = or i64 %{{[0-9]*}}, [[Addr0]]
// SHADERTEST: [[Addr2:%[0-9]*]] = inttoptr i64 [[Addr1]] to ptr addrspace(4)
// SHADERTEST: load <4 x float>, ptr addrspace(4) [[Addr2]], align 16
// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450

layout( location = 0 ) out vec4 frag_color;

layout( push_constant ) uniform ColorBlock {
  vec4 Color;
} PushConstant;

void main() {
   frag_color = PushConstant.Color;
}
