#version 450 core

layout(std140, binding = 0) uniform Block
{
    int  i;
    mat4 m4[2];
} block;

void main()
{
    int i = block.i;
    mat4 m4[2] = block.m4;
    m4[0][0] = vec4(3.0);
    m4[i][i] = vec4(2.0);
    gl_Position = m4[i][i];
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr ([4 x %llpc.matrix.column], ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2), i32 0, i32 1, i32 0), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr ([4 x %llpc.matrix.column], ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2), i32 0, i32 2, i32 0), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr ([4 x %llpc.matrix.column], ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2), i32 0, i32 3, i32 0), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2, i32 1), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2, i32 1, i32 1, i32 0), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2, i32 1, i32 2, i32 0), align 16
; SHADERTEST: %{{[0-9]*}} = load <4 x float>, ptr addrspace(7) getelementptr inbounds (<{ i32, [12 x i8], [2 x [4 x %{{[a-z.]*}}]] }>, ptr addrspace(7) @{{[a-z0-9]+}}, i32 0, i32 2, i32 1, i32 3, i32 0), align 16

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
