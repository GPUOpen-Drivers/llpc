#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 3) flat in int _4;
layout(location = 0) flat in vec4 _5;
layout(location = 1) flat in vec2 _6;
layout(location = 2) flat in int _7;
layout(location = 4) flat in int _8;
layout(location = 5) flat in int _9;
layout(location = 6) flat in int _10;

void main()
{
    int _12 = _4;
    _3 = texture(_11[nonuniformEXT(_12)], vec2(0.0));
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; Make sure that the begin indices chosen are the non-uniform offsets rather than the whole resource desc
; Make sure that there's a waterfall.readfirstlane for both the image resource desc and sample desc
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST-DAG: call i32 @llvm.amdgcn.waterfall.begin.i32
; SHADERTEST-DAG: call i32 @llvm.amdgcn.waterfall.begin.i32
; SHADERTEST-DAG: call <8 x i32> @llvm.amdgcn.waterfall.readfirstlane.v8i32.v8i32
; SHADERTEST-DAG: call <4 x i32> @llvm.amdgcn.waterfall.readfirstlane.v4i32.v4i32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
