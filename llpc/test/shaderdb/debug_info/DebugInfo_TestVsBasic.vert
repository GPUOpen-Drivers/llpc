#version 450 core

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
; RUN: amdllpc -trim-debug-info=false -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: !spirv.InOut [[D0:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D1:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D2:![0-9]*]]
; SHADERTEST: !spirv.InOut [[D3:![0-9]*]]
; SHADERTEST: define spir_func void @main() #{{[0-9]*}} !spirv.ExecutionModel [[D4:![0-9]*]] {
; SHADERTEST: load {{.*}} !dbg [[D5:![0-9]*]]
; SHADERTEST: store {{.*}} !dbg [[D5]]
; SHADERTEST: load {{.*}} !dbg [[D6:![0-9]*]]
; SHADERTEST: fadd {{.*}} !dbg [[D6]]
; SHADERTEST: store {{.*}} !dbg [[D6]]
; SHADERTEST: load {{.*}} !dbg [[D7:![0-9]*]]
; SHADERTEST: fadd {{.*}} !dbg [[D7]]
; SHADERTEST: store {{.*}} !dbg [[D7]]
; SHADERTEST: ret void, !dbg [[D8:![0-9]*]]
; SHADERTEST: !llvm.dbg.cu = !{[[D9:![0-9]*]]}
; SHADERTEST: !llvm.module.flags = !{[[D10:![0-9]*]], [[D11:![0-9]*]]}
; SHADERTEST: [[D0]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D1]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D2]] = !{{.*}} i64, i64 } { i64 {{[0-9]*}}, i64 0
; SHADERTEST: [[D3]] = !{{.*}} i64, i64 }, { i64, i64 },
; SHADERTEST: [[D12:![0-9]*]] = distinct !DICompileUnit(language: DW_LANG_C99, file: [[D13:![0-9]*]], producer: "spirv", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly, enums: [[D14:![0-9]*]])
; SHADERTEST: [[D13]] = !DIFile(filename: "spirv.dbg.cu", directory: ".")
; SHADERTEST: [[D14]] = !{}
; SHADERTEST: [[D10:![0-9]*]] = !{i32 2, !"Dwarf Version", i32 4}
; SHADERTEST: [[D11:![0-9]*]] = !{i32 2, !"Debug Info Version", i32 3}
; SHADERTEST: [[D4]] = !{i32 0}
; SHADERTEST: [[D5]] = !DILocation(line: 9, scope: [[D15:![0-9]*]])
; SHADERTEST: [[D15]] = distinct !DISubprogram(name: "main", linkageName: "main", scope: [[D16:![0-9]*]], file: [[D16]], type: [[D17:![0-9]*]], spFlags: DISPFlagDefinition, unit: [[D12]], retainedNodes: [[D14]])
; SHADERTEST: [[D16]] = !DIFile(filename: "", directory: ".")
; SHADERTEST: [[D17]] = !DISubroutineType(types: [[D14]])
; SHADERTEST: [[D6:![0-9]*]] = !DILocation(line: 10, scope: [[D15]])
; SHADERTEST: [[D7:![0-9]*]] = !DILocation(line: 11, scope: [[D15]])
; SHADERTEST: [[D8:![0-9]*]] = !DILocation(line: 13, scope: [[D15]])
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
