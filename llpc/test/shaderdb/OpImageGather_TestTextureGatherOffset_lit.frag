#version 450

layout(set = 0, binding = 0) uniform sampler2D      samp2D;
layout(set = 1, binding = 0) uniform sampler2DArray samp2DArray[4];
layout(set = 0, binding = 1) uniform sampler2DRect  samp2DRect;

layout(set = 2, binding = 0) uniform Uniforms
{
    int index;
    ivec2 i2;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    vec4 f4 = textureGatherOffset(samp2D, vec2(0.1), i2, 2);
    f4 += textureGatherOffset(samp2DArray[index], vec3(0.2), i2, 3);
    f4 += textureGatherOffset(samp2DRect, vec2(1.0), ivec2(1));

    fragColor = f4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.gather.v4f32(i32 1, i32 0, <8 x {{.*}}, <4 x {{.*}}, i32 293, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, i32 2, float 0.000000e+00, <2 x {{.*}})
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 1, i32 0)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 1, i32 0)
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.gather.v4f32(i32 5, i32 0, <8 x {{.*}}, <4 x {{.*}}, i32 293, <3 x float> <float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0x3FC99999A0000000>, i32 3, float 0.000000e+00, <2 x {{.*}})
; SHADERTEST: call { <8 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.image.desc.ptr.s[p4v8i32,i32]"(i32 0, i32 1)
; SHADERTEST: call { <4 x i32> addrspace(4)*, i32 } (...) @"llpc.call.get.sampler.desc.ptr.s[p4v4i32,i32]"(i32 0, i32 1)
; SHADERTEST: call reassoc nnan nsz arcp contract <4 x float> (...) @llpc.call.image.gather.v4f32(i32 1, i32 0, <8 x {{.*}}, <4 x {{.*}}, i32 293, <2 x float> <float 1.000000e+00, float 1.000000e+00>, i32 0, float 0.000000e+00, <2 x i32> <i32 1, i32 1>)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.l.o.2d.v4f32.f32(i32 4,{{.*}}, float 0x3FB99999A0000000, float 0x3FB99999A0000000,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.l.o.2darray.v4f32.f32(i32 8,{{.*}}, float 0x3FC99999A0000000, float 0x3FC99999A0000000, float 0.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.image.gather4.l.o.2d.v4f32.f32(i32 1, i32 257, float 1.000000e+00, float 1.000000e+00,{{.*}},{{.*}}, i1 false, i32 0, i32 0)
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
