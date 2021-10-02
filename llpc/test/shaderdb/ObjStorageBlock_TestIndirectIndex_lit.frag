#version 450

layout(std430, column_major, set = 0, binding = 0) buffer BufferObject
{
    uint ui;
    vec4 v4;
    vec4 v4_array[2];
}ssbo[2];

layout(set = 1, binding = 0) uniform Uniforms
{
    int i;
    int j;
};

layout(location = 0) out vec4 output0;

void main()
{
    output0 = ssbo[i].v4_array[j];

    ssbo[i].ui = 0;
    ssbo[i].v4 = vec4(3);
    ssbo[i].v4_array[j] = vec4(4);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %{{[^ ]+}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>] addrspace(7)* @{{[0-9]}}, i32 0, i32 %{{[^, ]+}}, i32 3, i32 %{{[A-Za-z0-9_.]+}}
; SHADERTEST: %{{[^ ]+}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>] addrspace(7)* @{{[0-9]}}, i32 0, i32 %{{[^, ]+}}, i32 0
; SHADERTEST: %{{[^ ]+}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>] addrspace(7)* @{{[0-9]}}, i32 0, i32 %{{[^, ]+}}, i32 2
; SHADERTEST: %{{[^ ]+}} = getelementptr [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>], [2 x <{ i32, [12 x i8], [4 x float], [2 x [4 x float]] }>] addrspace(7)* @{{[0-9]}}, i32 0, i32 %{{[^, ]+}}, i32 3, i32 %{{[A-Za-z0-9_.]+}}

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST-COUNT-3: call i8 addrspace(7)* {{.*}} @lgc.create.load.buffer.desc.{{[0-9a-z.]*}}(i32 0, i32 0, i32 %{{[^, ]+}},

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
