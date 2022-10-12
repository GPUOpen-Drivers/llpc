#version 450 core

#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0) buffer Buffers
{
    uint u;
};

void main()
{
    f16vec2 f16v2 = unpackFloat2x16(u);
    f16v2 += f16vec2(0.25hf);
    u = packFloat2x16(f16v2);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -enable-opaque-pointers=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; RUN: amdllpc -enable-opaque-pointers=true -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: %{{[0-9]*}} = load <2 x half>, {{<2 x half> addrspace\(7\)\*|ptr addrspace\(7\)}} %{{[0-9]*}}, align 4
; SHADERTEST: store <2 x half> %{{[0-9]*}}, {{<2 x half> addrspace\(7\)\*|ptr addrspace\(7\)}} %{{[0-9]*}}, align 4
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
