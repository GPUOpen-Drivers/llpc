#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 **********************************************************************************************************************/


layout(location = 0) in flat int i0;
layout(location = 1) in float i1;

layout(location = 0) out int o0;
layout(location = 1) out float o1;

void main()
{
    o0 = i0;
    o1 = i1;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -trim-debug-info=false -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: !spirv.InOut [[D0:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D1:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D2:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D2]]
; SHADERTEST: define dllexport spir_func void @main() #{{[0-9]*}} !dbg [[D12:![0-9]*]] !spirv.ExecutionModel [[D3:![0-9]*]]
; SHADERTEST: load {{.*}} !dbg [[D4:![0-9]*]]
; SHADERTEST: store {{.*}} !dbg [[D4]]
; SHADERTEST: load {{.*}} !dbg [[D5:![0-9]*]]
; SHADERTEST: store {{.*}} !dbg [[D5]]
; SHADERTEST: ret void, !dbg [[D5]]
; SHADERTEST: !llvm.module.flags = !{[[D7:![0-9]*]], [[D8:![0-9]*]]}
; SHADERTEST: !llvm.dbg.cu = !{[[D6:![0-9]*]]}
; SHADERTEST: [[D0]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D1]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D2]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D7:![0-9]*]] = !{i32 2, !"Dwarf Version", i32 4}
; SHADERTEST: [[D8:![0-9]*]] = !{i32 2, !"Debug Info Version", i32 3}
; SHADERTEST: [[D9:![0-9]*]] = distinct !DICompileUnit(language: DW_LANG_C99, file: [[D10:![0-9]*]], producer: "spirv", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)
; SHADERTEST: [[D10]] = !DIFile(filename: "DebugInfo_TestFsBasic.frag", directory: "{{.*}}")
; SHADERTEST: [[D12]] = distinct !DISubprogram(name: "main", linkageName: "main", scope: [[D10:![0-9]*]], file: [[D10]], line: 11, type: [[D13:![0-9]*]], scopeLine: 11, spFlags: DISPFlagDefinition, unit: [[D9]], retainedNodes: [[D11:![0-9]*]])
; SHADERTEST: [[D13]] = !DISubroutineType(types: [[D11]])
; SHADERTEST: [[D11]] = !{}
; SHADERTEST: [[D3]] = !{i32 4}
; SHADERTEST: [[D4]] = !DILocation(line: 11, scope: [[D12]])
; SHADERTEST: [[D15:![0-9]*]] = !DILocation(line: 12, scope: [[D12]])
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
