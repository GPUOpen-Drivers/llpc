#version 450
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/


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
