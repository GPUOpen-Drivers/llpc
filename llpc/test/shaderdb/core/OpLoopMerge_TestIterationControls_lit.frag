#version 450

#extension GL_EXT_control_flow_attributes : enable

void main()
{
        [[min_iterations(3), max_iterations(7)]]   for (int i = 0; i < 8; ++i) { }
        [[iteration_multiple(2)]]                  while(true) {  }
        [[peel_count(5)]]                          do {  } while(true);
        [[partial_count(4)]]                       for (int i = 0; i < 8; ++i) { }
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
