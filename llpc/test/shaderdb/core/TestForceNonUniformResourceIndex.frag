// Test not forcing NURI
// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip %s --force-non-uniform-resource-index-stage-mask=0x00000000 | FileCheck -check-prefix=NOTFORCENURITEST %s
// NOTFORCENURITEST-LABEL: {{^// LLPC}} pipeline before-patching results
// When not forcing NURI (Non Uniform Resource Index), there should be a `readfirstlane`.
// NOTFORCENURITEST: %{{[0-9]+}} = call i32 @llvm.amdgcn.readfirstlane(i32 %{{[0-9]+}})
// NOTFORCENURITEST: AMDLLPC SUCCESS
// END_SHADERTEST

// Test forcing NURI
// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip %s --force-non-uniform-resource-index-stage-mask=0xFFFFFFFF | FileCheck -check-prefix=FORCENURITEST %s
// FORCENURITEST-LABEL: {{^// LLPC}} pipeline before-patching results
// When forcing NURI (Non Uniform Resource Index), there should not be a `readfirstlane`.
// FORCENURITEST-NOT: %{{[0-9]+}} = call i32 @llvm.amdgcn.readfirstlane(i32 %{{[0-9]+}})
// FORCENURITEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450

#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) buffer Data
{
    vec4 color;
} data[];

layout(location = 0) out vec4 FragColor;
layout(location = 0) in flat int index;
void main()
{
  FragColor = data[index].color;
}
