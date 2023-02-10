#version 450

layout(binding = 0) uniform Uniforms
{
    double d1_1;
    dvec3 d3_1;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    double d1_0 = inversesqrt(d1_1);

    dvec3 d3_0 = inversesqrt(d3_1);

    fragColor = (d1_0 >= d3_0.x) ? vec4(0.0) : vec4(1.0);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: %[[SQRT:[^ ,]*]] = call reassoc nnan nsz arcp contract double (...) @lgc.create.inverse.sqrt.f64(double
; SHADERTEST: %[[SQRT3:[^ ,]*]] = call reassoc nnan nsz arcp contract <3 x double> (...) @lgc.create.inverse.sqrt.v3f64(<3 x double>

; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST: %[[X:[^ ,]*]] = load double, ptr addrspace(7)
; SHADERTEST: %[[SCALING:[^ ,]*]] = fcmp reassoc nnan nsz arcp contract olt double %[[X]], 0x1000000000000000
; SHADERTEST: %[[SCALE_UP:[^ ,]*]] = select i1 %[[SCALING]], i32 256, i32 0
; SHADERTEST: %[[SCALE_DOWN:[^ ,]*]] = select i1 %[[SCALING]], i32 128, i32 0
; SHADERTEST: %[[SCALE_X:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.amdgcn.ldexp.f64(double %[[X]], i32 %[[SCALE_UP]])
; SHADERTEST: %[[EXP_OF_SCALE_X:[^ ,]*]] = call i32 @llvm.amdgcn.frexp.exp.i32.f64(double %[[SCALE_X]])
; SHADERTEST: %[[TOO_SMALL_SCALE_X:[^ ,]*]] = icmp slt i32 %[[EXP_OF_SCALE_X]], -1021
; SHADERTEST: %[[NEW_SCALE_X:[^ ,]*]] = select reassoc nnan nsz arcp contract i1 %[[TOO_SMALL_SCALE_X]], double 0.000000e+00, double %[[SCALE_X]]
; SHADERTEST: %[[Y:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.amdgcn.rsq.f64(double %[[NEW_SCALE_X]])
; SHADERTEST: %[[G0:[^ ,]*]] = fmul reassoc nnan nsz arcp contract double %[[NEW_SCALE_X]], %[[Y]]
; SHADERTEST: %[[H0:[^ ,]*]] = fmul reassoc nnan nsz arcp contract double 5.000000e-01, %[[Y]]
; SHADERTEST: %[[NEG_H0:[^ ,]*]] = fneg reassoc nnan nsz arcp contract double %[[H0]]
; SHADERTEST: %[[R0:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[NEG_H0]], double %[[G0]], double 5.000000e-01)
; SHADERTEST: %[[G1:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[G0]], double %[[R0]], double %[[G0]])
; SHADERTEST: %[[H1:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[H0]], double %[[R0]], double %[[H0]])
; SHADERTEST: %[[NEG_H1:[^ ,]*]] = fneg reassoc nnan nsz arcp contract double %[[H1]]
; SHADERTEST: %[[R1:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[NEG_H1]], double %[[G1]], double 5.000000e-01)
; SHADERTEST: %[[G2:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[G1]], double %[[R1]], double %[[G1]])
; SHADERTEST: %[[H2:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[H1]], double %[[R1]], double %[[H1]])
; SHADERTEST: %[[NEG_H2:[^ ,]*]] = fneg reassoc nnan nsz arcp contract double %[[H2]]
; SHADERTEST: %[[R2:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[NEG_H2]], double %[[G2]], double 5.000000e-01)
; SHADERTEST: %[[H3:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.fma.f64(double %[[H2]], double %[[R2]], double %[[H2]])
; SHADERTEST: %[[RSQ_X:[^ ,]*]] = fmul reassoc nnan nsz arcp contract double 2.000000e+00, %[[H3]]
; SHADERTEST: %[[SCALE_RSQ_X:[^ ,]*]] = call reassoc nnan nsz arcp contract double @llvm.amdgcn.ldexp.f64(double %[[RSQ_X]], i32 %[[SCALE_DOWN]])
; SHADERTEST: %[[EXP_OF_SCALE_RSQ_X:[^ ,]*]] = call i32 @llvm.amdgcn.frexp.exp.i32.f64(double %[[SCALE_RSQ_X]])
; SHADERTEST: %[[TOO_SMALL_SCALE_RSQ_X:[^ ,]*]] = icmp slt i32 %[[EXP_OF_SCALE_RSQ_X]], -1021
; SHADERTEST: %[[NEW_SCALE_RSQ_X:[^ ,]*]] = select reassoc nnan nsz arcp contract i1 %[[TOO_SMALL_SCALE_RSQ_X]], double 0.000000e+00, double %[[SCALE_RSQ_X]]
; SHADERTEST: %[[SPECIAL_X:[^ ,]*]] = call i1 @llvm.amdgcn.class.f64(double %[[NEW_SCALE_X]], i32 608)
; SHADERTEST: %[[FINAL_RSQ_X:[^ ,]*]] = select reassoc nnan nsz arcp contract i1 %[[SPECIAL_X]], double %[[Y]], double %[[NEW_SCALE_RSQ_X]]

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
