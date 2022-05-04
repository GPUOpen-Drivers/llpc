#version 450

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; Test that after SPIR-V lowering all operations after first kill have been
; replaced with a branch to the return block.

; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call void{{.*}} @lgc.create.kill
; SHADERTEST-NEXT: br label %[[LABEL:[0-9]*]]
; SHADERTEST-NOT: call void{{.*}} @lgc.create.kill
; SHADERTEST: [[LABEL]]:
; SHADERTEST-NEXT: call void{{.*}} @lgc.create.write.generic.output

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

layout(location = 0) in mediump vec4 v_color1;
layout(location = 1) in mediump vec4 v_color2;
layout(location = 2) in mediump vec4 v_coords;
layout(location = 0) out mediump vec4 o_color;

void myfunc0 (void)
{
    discard;
}

void myfunc1 (void)
{
    o_color = v_color2;
}

void main (void)
{
    o_color = v_color1;
    if (v_coords.x+v_coords.y > 0.0) {
      myfunc0();
      myfunc1();
      myfunc0();
    }
}
