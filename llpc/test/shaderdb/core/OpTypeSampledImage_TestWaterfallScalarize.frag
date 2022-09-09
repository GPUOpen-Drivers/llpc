// Make sure that there is a single begin index
// Make sure that there is a single waterfall.readfirstlane for the offset

#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 3) flat in int _4;
layout(location = 1) flat in vec2 _6;

void main()
{
    int _12 = _4;
    _3 = texture(_11[nonuniformEXT(_12)], _6);
}

// BEGIN_SHADERTEST
//
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
// Explicitly check GFX10.3 ASIC variants:
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.0 %s | FileCheck -check-prefix=SHADERTEST %s
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.2 %s | FileCheck -check-prefix=SHADERTEST %s
// SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
// SHADERTEST: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST-NOT: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST: call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32
// SHADERTEST-NOT: call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.waterfall.end.v4f32
// SHADERTEST: AMDLLPC SUCCESS
//
// END_SHADERTEST
