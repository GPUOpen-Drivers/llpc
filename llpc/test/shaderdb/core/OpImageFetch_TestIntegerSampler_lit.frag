#version 450 core

layout(set = 0, binding = 0) uniform isampler2D iSamp2D;
layout(set = 0, binding = 1) uniform usampler2D uSamp2D;
layout(location = 0) out ivec4 oColor1;
layout(location = 1) out uvec4 oColor2;

void main()
{
    oColor1 = texelFetch(iSamp2D, ivec2(0, 1), 0);
    oColor2 = texelFetch(uSamp2D, ivec2(0, 1), 0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 0
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 1, i32 1540, {{.*}}, <2 x i32> <i32 0, i32 1>, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i32 0, i32 1
; SHADERTEST: call <4 x i32> (...) @lgc.create.image.load.v4i32(i32 1, i32 1536, {{.*}}, <2 x i32> <i32 0, i32 1>, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2d.v4i32.i32(i32 15, i32 0, i32 1, <8 x i32> %{{[-0-9A-Za0z_.]+}}, i32 0, i32 0), !invariant.load
; SHADERTEST: call <4 x i32> @llvm.amdgcn.image.load.2d.v4i32.i32(i32 15, i32 0, i32 1, <8 x i32> %{{[-0-9A-Za0z_.]+}}, i32 0, i32 0), !invariant.load
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
