#version 450 core

layout(std430, binding = 0) buffer Block
{
    float f1;
    vec2  f2;
    vec3  f3;
    vec4  f4;
} block;

void main()
{
    block.f1 += 1.0;
    block.f2 += vec2(2.0);
    block.f3 += vec3(3.0);
    block.f4 += vec4(4.0);

    gl_Position = vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-load-scalarizer=false -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.raw.buffer.load.i32(<4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.i32(i32 {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 0, i32 0, i32 0)
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32(<4 x i32> {{%[^,]+}}, i32 8, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32(<2 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 8, i32 0, i32 0)
; SHADERTEST: call <3 x i32> @llvm.amdgcn.raw.buffer.load.v3i32(<4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v3i32(<3 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 16, i32 0, i32 0)
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> {{%[^,]+}}, <4 x i32> {{%[^,]+}}, i32 32, i32 0, i32 0)

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
