#version 450 core

#extension GL_AMD_gcn_shader: enable

#extension GL_ARB_gpu_shader_int64: enable

layout(location = 0) in vec3 f3;
layout(location = 1) out vec4 f4;

void main()
{
    float f1 = cubeFaceIndexAMD(f3);
    vec2 f2 = cubeFaceCoordAMD(f3);

    uint64_t u64 = timeAMD();

    f4.x = f1;
    f4.yz = f2;
    f4.w = float(u64);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract float (...) @llpc.call.cube.face.index.f32(<3 x float>
; SHADERTEST: = call reassoc nnan nsz arcp contract <2 x float> (...) @llpc.call.cube.face.coord.v2f32(<3 x float>
; SHADERTEST: = call i64 (...) @llpc.call.read.clock.i64(i1 false)
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubeid
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubema
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubesc
; SHADERTEST: call {{.*}} float @llvm.amdgcn.cubetc
; SHADERTEST: [[TIME:%[^ ]*]] = call i64 @llvm.amdgcn.s.memtime()
; SHADERTEST: = call i64 asm sideeffect "; %1", "=r,0"(i64 [[TIME]])
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
