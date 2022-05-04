#version 450

layout(std430, column_major, set = 0, binding = 0) buffer BufferObject
{
    uint ui;
    double d;
    dvec4 dv4;
    dmat2x4 dm2x4;
};

layout(location = 0) out vec4 output0;

void main()
{
    dvec4 dv4temp = dv4;
    dv4temp.x += d;
    d = dv4temp.y;
    output0 = vec4(dv4temp);
    dv4temp = dm2x4[0];
    dm2x4[1] = dv4temp;
}


// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 32
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 48
; SHADERTEST: call <2 x i32> @llvm.amdgcn.raw.buffer.load.v2i32(<4 x i32> %{{[0-9]*}}, i32 8
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v2i32(<2 x i32> {{%[^,]+}}, <4 x i32> %{{[0-9]*}}, i32 8
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 64
; SHADERTEST: call <4 x i32> @llvm.amdgcn.raw.buffer.load.v4i32(<4 x i32> %{{[0-9]*}}, i32 80
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 96
; SHADERTEST: call void @llvm.amdgcn.raw.buffer.store.v4i32(<4 x i32> %{{[0-9]*}}, <4 x i32> %{{[0-9]*}}, i32 112


; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
