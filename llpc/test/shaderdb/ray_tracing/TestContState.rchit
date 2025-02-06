
// Test that continuations state has a reasonable size.
// NOTE: Hit attribute is extracted from system data, we want to make sure we only put necessary part into continuations
// state, but not the whole/partial structure which is extracted from.

// RUN: amdllpc %gfxip --llpc-raytracing-mode=continuations --report-cont-state-sizes %s 2>&1 | FileCheck -check-prefix=CHECK %s
// CHECK: Continuation state size of "_chit_1" (closesthit): 12 bytes
#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload {
  vec4 a;
  vec4 b;
  float c;
};

layout(binding = 0, set = 0) uniform accelerationStructureEXT g_bvh;
layout(binding = 1, set = 0, rgba32f) uniform image2D g_dst;
layout(binding = 2, set = 0) uniform Buf {
  vec4 u_a;
  vec4 u_b;
  float u_c;
};

layout(location = 14) rayPayloadInEXT RayPayload g_ray;
hitAttributeEXT vec2 g_attr;

void main() {
  g_ray.a = u_a;
  g_ray.b = u_b;
  g_ray.c = u_c;
  vec3 origin;
  origin.x = gl_LaunchIDEXT.x;
  origin.y = gl_LaunchIDEXT.y;
  origin.z = 0;

  vec4 bary = vec4((1.0 - g_attr.x) - g_attr.y, g_attr.x, g_attr.y, 0);

  traceRayEXT(g_bvh, /* ray flags */ 0, /* cull mask */ 0xff, 
              /* sbt offset */ 0, /* sbt stride */ 1, /* miss index */ 0,
              origin.xyz, /* tmin */ 0.0, /* direction */ vec3(1, 0, 0),
              /* tmax */ 48.0, /* payload location */ 14);

  vec4 result = g_ray.a + bary;

  imageStore(g_dst, ivec2(gl_LaunchIDEXT.xy), result);
}
