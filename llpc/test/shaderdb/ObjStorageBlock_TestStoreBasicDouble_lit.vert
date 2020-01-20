#version 450 core

layout(std430, binding = 0) buffer Block
{
    double d1;
    dvec2  d2;
    dvec3  d3;
    dvec4  d4;
} block;

void main()
{
    block.d1 += 1.0;
    block.d2 += dvec2(2.0);
    block.d3 += dvec3(3.0);
    block.d4 += dvec4(4.0);

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32(<4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2f32(<2 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32(<4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2f32(<2 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 48, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> {{%[^,]+}}, i32 80, i32 0, i32 0)
; SHADERTEST: shufflevector <8 x i32> {{%[^,]+}}, <8 x i32> undef, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
; SHADERTEST: shufflevector <8 x i32> {{%[^,]+}}, <8 x i32> undef, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 64, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4f32(<4 x float> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 80, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
