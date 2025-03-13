#version 450 core
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


layout(location = 0) in vec4  f4;
layout(location = 1) in int   i1;
layout(location = 2) in uvec2 u2;

void main()
{
    vec4 f = f4;
    f += vec4(i1);
    f += vec4(u2, u2);

    gl_Position = f;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -trim-debug-info=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: !spirv.InOut [[D0:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D1:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D2:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D3:![0-9]*]]
; SHADERTEST: define dllexport spir_func void @main() #{{[0-9]*}} !dbg [[D15:![0-9]*]] !spirv.ExecutionModel [[D4:![0-9]*]]
; SHADERTEST: load {{.*}} !dbg [[D5:![0-9]*]]
; SHADERTEST: store {{.*}} !dbg [[D5]]
; SHADERTEST: load {{.*}} !dbg [[D6:![0-9]*]]
; SHADERTEST: fadd {{.*}} !dbg [[D6]]
; SHADERTEST: store {{.*}} !dbg [[D6]]
; SHADERTEST: load {{.*}} !dbg [[D7:![0-9]*]]
; SHADERTEST: fadd {{.*}} !dbg [[D7]]
; SHADERTEST: store {{.*}} !dbg [[D7]]
; SHADERTEST: ret void, !dbg [[D8:![0-9]*]]
; SHADERTEST: !llvm.module.flags = !{[[D10:![0-9]*]], [[D11:![0-9]*]]}
; SHADERTEST: !llvm.dbg.cu = !{[[D9:![0-9]*]]}
; SHADERTEST: [[D0]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D1]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D2]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D3]] = !{{.*}} i64, i64 }, { i64, i64 },
; SHADERTEST: [[D10:![0-9]*]] = !{i32 2, !"Dwarf Version", i32 4}
; SHADERTEST: [[D11:![0-9]*]] = !{i32 2, !"Debug Info Version", i32 3}
; SHADERTEST: [[D12:![0-9]*]] = distinct !DICompileUnit(language: DW_LANG_C99, file: [[D13:![0-9]*]], producer: "spirv", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)
; SHADERTEST: [[D13]] = !DIFile(filename: "DebugInfo_TestVsBasic.vert", directory: "{{.*}}")
; SHADERTEST: [[D15]] = distinct !DISubprogram(name: "main", linkageName: "main", scope: [[D13:![0-9]*]], file: [[D13]], line: 9, type: [[D16:![0-9]*]], scopeLine: 9, spFlags: DISPFlagDefinition, unit: [[D12]], retainedNodes: [[D14:![0-9]*]])
; SHADERTEST: [[D16]] = !DISubroutineType(types: [[D14]])
; SHADERTEST: [[D14]] = !{}
; SHADERTEST: [[D4]] = !{i32 0}
; SHADERTEST: [[D5]] = !DILocation(line: 9, scope: [[D15]])
; SHADERTEST: [[D6:![0-9]*]] = !DILocation(line: 10, scope: [[D15]])
; SHADERTEST: [[D7:![0-9]*]] = !DILocation(line: 11, scope: [[D15]])
; SHADERTEST: [[D8:![0-9]*]] = !DILocation(line: 13, scope: [[D15]])
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
