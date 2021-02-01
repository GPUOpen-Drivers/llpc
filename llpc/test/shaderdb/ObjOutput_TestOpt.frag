#version 450

void main()
{
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc               -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_DEFAULT %s
; RUN: amdllpc --opt=none    -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_NONE %s
; RUN: amdllpc --opt=quick   -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_QUICK %s
; RUN: amdllpc --opt=default -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_DEFAULT %s
; RUN: amdllpc --opt=fast    -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck --check-prefixes=SHADERTEST,OPT_FAST %s
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
