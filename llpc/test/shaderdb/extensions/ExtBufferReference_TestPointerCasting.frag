#version 450 core
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : enable
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_buffer_reference_uvec2 : enable

layout(set = 0, binding = 1) buffer BufData {
   int a;
   uvec2 b;
   uint64_t c;
} data;
layout(buffer_reference, std140) buffer BufRef {
   int a;
};
void main()
{
   uvec2 ptr_uvec2 = data.b;
   BufRef buf_1 = BufRef(ptr_uvec2);
   buf_1.a = 42;

   uint64_t ptr_uint64 = data.c;
   BufRef buf_2 = BufRef(ptr_uint64);
   buf_2.a = 42;

   data.b = uvec2(buf_1);
   data.c = uint64_t(buf_2);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results

; SHADERTEST: [[bc:%[0-9]*]] = bitcast <2 x i32> {{%[0-9]*}} to i64
; SHADERTEST: [[itop:%[0-9]*]] = inttoptr i64 [[bc]] to ptr addrspace(1)

; SHADERTEST: [[ld1:%[0-9]*]] = load i64, ptr addrspace(5)
; SHADERTEST: [[itop:%[0-9]*]] = inttoptr i64 [[ld1]] to ptr addrspace(1)

; SHADERTEST: [[int:%[0-9]*]] = ptrtoint ptr addrspace(1) {{%[0-9]*}} to i64
; SHADERTEST: [[bc2:%[0-9]*]] = bitcast i64 [[int]] to <2 x i32>

; SHADERTEST: [[ld2:%[0-9]*]] = load ptr addrspace(1), ptr addrspace(5)
; SHADERTEST: [[ptoi:%[0-9]*]] = ptrtoint ptr addrspace(1) [[ld2]] to i64


; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
