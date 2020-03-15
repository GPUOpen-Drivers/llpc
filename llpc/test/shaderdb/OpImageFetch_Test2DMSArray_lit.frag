#version 450 core

layout(set = 0, binding = 0) uniform sampler2DMSArray samp;
layout(location = 0) in vec3 inUV;
layout(location = 0) out vec4 oColor;

void main()
{
    ivec3 iUV = ivec3(inUV);
    oColor = texelFetch(samp, iUV, 2);
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results


call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.fmask.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 0)
call <4 x float> (...) @llpc.call.image.load.with.fmask.v4f32(i32 7, i32 0,{{.*}}, i32 2)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
call i32 @llvm.amdgcn.image.load.3d.i32.i32(i32 1,{{.*}}, i32 0, i32 0)
call <4 x float> @llvm.amdgcn.image.load.2darraymsaa.v4f32.i32(i32 15, i32 {{.*}}, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
