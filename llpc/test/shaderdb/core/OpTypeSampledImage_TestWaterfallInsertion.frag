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
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.waterfall.begin.i32
; SHADERTEST-NEXT: %[[readfirstlane:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32
; SHADERTEST-NEXT: %[[sext:[0-9]+]] = sext i32 %[[readfirstlane]] to i64
; SHADERTEST-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
; SHADERTEST-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
; SHADERTEST-NEXT: load <4 x i32>, ptr addrspace(4) %[[gep2]], align 16
; SHADERTEST-NEXT: load <8 x i32>, ptr addrspace(4) %[[gep1]], align 32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
