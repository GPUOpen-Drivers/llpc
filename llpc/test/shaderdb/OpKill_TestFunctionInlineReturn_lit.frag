#version 450

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; Test that after SPIR-V lowering all operations after second kill
; have been replaced with a branch to the return block; however,
; the operations after first kill should not be touched as these
; are part of return .

; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call void{{.*}} @lgc.create.kill
; SHADERTEST-NEXT: br label %"{{.*}}.exit"
; SHADERTEST: call void{{.*}} @lgc.create.kill
; SHADERTEST-NEXT: br label %[[LABEL:[0-9]*]]
; SHADERTEST-NOT: call void{{.*}} @lgc.create.kill
; SHADERTEST: [[LABEL]]:
; SHADERTEST: call void{{.*}} @lgc.create.write.generic.output

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

layout(location = 0) in mediump vec4 v_color1;
layout(location = 2) in mediump vec4 v_coords;
layout(location = 0) out mediump vec4 o_color;

layout(set = 0, binding = 0, std140) uniform buf0
{
    float zero;
} param;

void myfunc0 (void)
{
    discard;
}

highp float myfunc1(int b)
{
    if (b == 0)
    {
      discard;
    }
    if (b > 2)
    {
      return 3.0;
    }
    return 5.0;
}

void main (void)
{
    o_color = v_color1;
    o_color.x = myfunc1(int(param.zero));
    if (v_coords.x+v_coords.y > 0.0) {
      myfunc0();
      myfunc0();
    }
}
