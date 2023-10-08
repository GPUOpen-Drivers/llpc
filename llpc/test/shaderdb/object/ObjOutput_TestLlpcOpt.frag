#version 450

void main()
{
}
// BEGIN_SHADERTEST
/*

; Test that the amdllpc option set the optimization level through the interface.
; We expect the "OPT_QUICK" result for --llpc-opt=none because it is currently suppose to bump the opt level to quick
; to work around bug in the AMDGPU backend at noopt.
; RUN: amdllpc --llpc-opt=none    -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_QUICK %s
; RUN: amdllpc --llpc-opt=quick   -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_QUICK %s
; RUN: amdllpc --llpc-opt=default -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_DEFAULT %s
; RUN: amdllpc --llpc-opt=fast    -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_FAST %s

; Test that the LGC option overrides the amdllpc option.
; RUN: amdllpc --llpc-opt=fast --opt=none    -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_NONE %s

; SHADERTEST-LABEL: {{^// LLPC}} calculated hash results (graphics pipeline)
; OPT_NONE:  TargetMachine optimization level = 0
; OPT_QUICK:  TargetMachine optimization level = 1
; OPT_DEFAULT:  TargetMachine optimization level = 2
; OPT_FAST:  TargetMachine optimization level = 3
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; OPT_NONE:  PassManager optimization level = 0
; OPT_QUICK:  PassManager optimization level = 1
; OPT_DEFAULT:  PassManager optimization level = 2
; OPT_FAST:  PassManager optimization level = 3
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
