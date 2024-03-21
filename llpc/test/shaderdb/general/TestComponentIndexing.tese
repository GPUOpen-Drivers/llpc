// BEGIN_SHADERTEST
// This test is to verify input mapping of TES when component indexing is encountered. In such
// case, we are supposed to reserve all components of locations corresponding to a TES input
// in the location info mapping table. We also have to take component offset, specified by
// 'component' qualifier, into consideration and reserve enough components for such indexing.
// RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

// SHADERTEST-LABEL: {{^// LLPC}} location input/output mapping results (TES)
// layout(location = 0, component = 1) in vec3 i0[]
// SHADERTEST: (TES) Input:  [location, component] = [0, 0]  =>  Mapped = [0, 0]
// SHADERTEST: (TES) Input:  [location, component] = [0, 1]  =>  Mapped = [0, 1]
// SHADERTEST: (TES) Input:  [location, component] = [0, 2]  =>  Mapped = [0, 2]
// SHADERTEST: (TES) Input:  [location, component] = [0, 3]  =>  Mapped = [0, 3]
// layout(location = 1, component = 1) in vec2 i1[]
// SHADERTEST: (TES) Input:  [location, component] = [1, 0]  =>  Mapped = [1, 0]
// SHADERTEST: (TES) Input:  [location, component] = [1, 1]  =>  Mapped = [1, 1]
// SHADERTEST: (TES) Input:  [location, component] = [1, 2]  =>  Mapped = [1, 2]
// layout(location = 2) in dvec4 i2[]
// SHADERTEST: (TES) Input:  [location, component] = [2, 0]  =>  Mapped = [2, 0]
// SHADERTEST: (TES) Input:  [location, component] = [2, 1]  =>  Mapped = [2, 1]
// SHADERTEST: (TES) Input:  [location, component] = [2, 2]  =>  Mapped = [2, 2]
// SHADERTEST: (TES) Input:  [location, component] = [2, 3]  =>  Mapped = [2, 3]
// SHADERTEST: (TES) Input:  [location, component] = [3, 0]  =>  Mapped = [3, 0]
// SHADERTEST: (TES) Input:  [location, component] = [3, 1]  =>  Mapped = [3, 1]
// SHADERTEST: (TES) Input:  [location, component] = [3, 2]  =>  Mapped = [3, 2]
// SHADERTEST: (TES) Input:  [location, component] = [3, 3]  =>  Mapped = [3, 3]
// layout(location = 4) in dvec3 i3[]
// SHADERTEST: (TES) Input:  [location, component] = [4, 0]  =>  Mapped = [4, 0]
// SHADERTEST: (TES) Input:  [location, component] = [4, 1]  =>  Mapped = [4, 1]
// SHADERTEST: (TES) Input:  [location, component] = [4, 2]  =>  Mapped = [4, 2]
// SHADERTEST: (TES) Input:  [location, component] = [4, 3]  =>  Mapped = [4, 3]
// SHADERTEST: (TES) Input:  [location, component] = [5, 0]  =>  Mapped = [5, 0]
// SHADERTEST: (TES) Input:  [location, component] = [5, 1]  =>  Mapped = [5, 1]

// layout(location = 0) out vec3 o0
// SHADERTEST: (TES) Output: [location, component] = [0, 0]  =>  Mapped = [0, 0]
// layout(location = 1) out vec2 o1
// SHADERTEST: (TES) Output: [location, component] = [1, 0]  =>  Mapped = [1, 0]
// layout(location = 2) out dvec4 o2
// SHADERTEST: (TES) Output: [location, component] = [2, 0]  =>  Mapped = [2, 0]
// SHADERTEST: (TES) Output: [location, component] = [3, 0]  =>  Mapped = [3, 0]
// layout(location = 4) out dvec3 o3
// SHADERTEST: (TES) Output: [location, component] = [4, 0]  =>  Mapped = [4, 0]
// SHADERTEST: (TES) Output: [location, component] = [5, 0]  =>  Mapped = [5, 0]

// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST

#version 450 core

layout(triangles) in;

layout(location = 0, component = 1) in vec3 i0[];
layout(location = 1, component = 1) in vec2 i1[];
layout(location = 2) in dvec4 i2[];
layout(location = 4) in dvec3 i3[];

layout(location = 0) out vec3 o0;
layout(location = 1) out vec2 o1;
layout(location = 2) out dvec4 o2;
layout(location = 4) out dvec3 o3;

layout(binding = 0) uniform Uniform {
  uint index;
};

void main (void) {
  // Constant component indexing
  o0.x = i0[0][0];
  o0.y = i0[1][1];
  o0.z = i0[2][2];
  
  // Dynamic component indexing
  o1[index] = i1[index][index];

  // 64-bit constant component indexing
  o2[0] = i2[0][3];
  o2[1] = i2[1][2];
  o2[2] = i2[2][1];
  o2[3] = i2[0][0];
  
  // 64-bit dynamic component indexing
  o3[index] = i3[index][index];
}