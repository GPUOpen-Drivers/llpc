// If implicit invariant marking is allowed for instructions contributing to gl_Position exports, the
// fast math flag is disabled for these instructions. This occurs if invariance is expected even if no
// invariance flag is being used in SPIR-V. Enabling the FMF can sometimes break rendering with FMA
// instructions being generated.
// Check if FMF for position exports is disabled if --enable-implicit-invariant-exports is set to 1.

#version 450

layout(location = 0) in vec4 inPos;

layout(std140, binding = 0) uniform block {
    uniform mat4 mvp;
};

void main()
{
    gl_Position = mvp * inPos;
}

// BEGIN_WITHOUT_IIE
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=WITHOUT_IIE %s
; WITHOUT_IIE-LABEL: {{^// LLPC}} pipeline before-patching results
; WITHOUT_IIE: %[[val:.*]] = extractvalue [4 x <4 x float>] %{{.*}}, 3
; WITHOUT_IIE: %[[mul:.*]] = fmul <4 x float> %[[val]], %{{.*}}
; WITHOUT_IIE: %[[arg:.*]] = fadd <4 x float> %{{.*}}, %[[mul]]
; WITHOUT_IIE-NEXT: call void @lgc.output.export.builtin.Position.i32.v4f32(i32 0, <4 x float> %[[arg]]) #0
; WITHOUT_IIE: AMDLLPC SUCCESS
*/
// END_WITHOUT_IIE

// BEGIN_WITH_IIE
/*
; RUN: amdllpc -v --enable-implicit-invariant-exports=0 %s | FileCheck -check-prefix=WITH_IIE %s
; WITH_IIE-LABEL: {{^// LLPC}} pipeline before-patching results
; WITH_IIE: %[[val:.*]] = extractvalue [4 x <4 x float>] %{{.*}}, 3
; WITH_IIE: %[[mul:.*]] = fmul reassoc nnan nsz arcp contract afn <4 x float> %[[val]], %{{.*}}
; WITH_IIE: %[[arg:.*]] = fadd reassoc nnan nsz arcp contract afn <4 x float> %{{.*}}, %[[mul]]
; WITH_IIE-NEXT: call void @lgc.output.export.builtin.Position.i32.v4f32(i32 0, <4 x float> %[[arg]]) #0
; WITH_IIE: AMDLLPC SUCCESS
*/
// END_WITH_IIE
