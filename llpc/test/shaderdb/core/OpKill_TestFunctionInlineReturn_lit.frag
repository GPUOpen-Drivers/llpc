#version 450

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; Test that after SPIR-V lowering there are only two kills and
; both branch directly to the return block.

; Check that the selection of constants is correctly preserved
; in the inlined function call. Selection relies on SimplifyCFG
; correctly lowering a PHI, in a manner that can fail if
; PHIs are not updated correctly.

; SHADERTEST-LABEL: {{^// LLPC.*}} SPIR-V lowering results
; SHADERTEST: call void{{.*}} @lgc.create.kill
; SHADERTEST-NEXT: br label %[[LABEL:[0-9]*]]

; SHADERTEST: "myfunc1(i1;.exit":
; SHADERTEST: select i1 %{{.*}}, float 3.000000e+00, float 5.000000e+00

; SHADERTEST: call void{{.*}} @lgc.create.kill
; SHADERTEST-NEXT: br label %[[LABEL]]

; SHADERTEST-NOT: call void{{.*}} @lgc.create.kill

; SHADERTEST: [[LABEL]]:
; SHADERTEST-NOT: call void{{.*}} @lgc.create.kill
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
