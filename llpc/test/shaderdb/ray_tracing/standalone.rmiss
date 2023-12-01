// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST: _miss1:
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST

#version 460
#extension GL_EXT_ray_tracing : enable

layout(location = 0) rayPayloadInEXT vec3 hitValue;

layout(shaderRecordEXT, std430) buffer SBT {
  float b;
};

void main()
{
  hitValue = vec3(b, b, b);
}
