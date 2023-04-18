// Make sure that there are two non-overlapping waterfall loops
// First is scalarized and second is vector type

#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 1) flat in int _4;
layout(location = 2) flat in int _4b;
layout(location = 3) flat in vec2 _6;
layout(location = 4) flat in vec2 _7;

void main()
{
    int _12 = _4;
    vec2 coord2 = _7;
    vec4 samp0 = texture(_11[nonuniformEXT(_12)], _6);
    vec4 samp1 = texture(_11[nonuniformEXT(_12)], coord2);
    if (_4b == 0) {
      coord2 = coord2 * 2;
      samp1 = texture(_11[nonuniformEXT(_12)], coord2);
    }
    _3 = samp0 + samp1;
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
// SHADERTEST-NOT: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.waterfall.end.v4f32
// SHADERTEST-NOT: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.waterfall.end.v4f32
// SHADERTEST: call i32 @llvm.amdgcn.waterfall.begin.i32
// SHADERTEST: call <8 x i32> @llvm.amdgcn.waterfall.readfirstlane.v8i32.v8i32
// SHADERTEST: call <4 x i32> @llvm.amdgcn.waterfall.readfirstlane.v4i32.v4i32
// SHADERTEST: call {{.*}} <4 x float> @llvm.amdgcn.waterfall.end.v4f32
// SHADERTEST: AMDLLPC SUCCESS
//
// END_SHADERTEST
