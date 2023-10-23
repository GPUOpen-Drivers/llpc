#version 450

#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0, std430) buffer Buffers
{
    vec3 fv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3 = f16vec3(fv3);
    f16vec2 f16v2 = f16v3.yz;
    f16vec4 f16v4 = f16vec4(f16v3, f16v2.x);
    float16_t f16 = f16v2.y;

    f16 += length(f16);
    f16 += length(f16v2);
    f16 += length(f16v3);
    f16 += length(f16v4);

    f16 += distance(f16, f16);
    f16 += distance(f16v2, f16v2);
    f16 += distance(f16v3, f16v3);
    f16 += distance(f16v4, f16v4);

    f16 += dot(f16v3, f16v3);
    f16 += dot(f16v4, f16v4);
    f16 += dot(f16v2, f16v2);

    f16v3 = cross(f16v3, f16v3);

    f16   += normalize(f16);
    f16v2 += normalize(f16v2);
    f16v3 += normalize(f16v3);
    f16v4 += normalize(f16v4);

    f16   += faceforward(f16, f16, f16);
    f16v2 += faceforward(f16v2, f16v2, f16v2);
    f16v3 += faceforward(f16v3, f16v3, f16v3);
    f16v4 += faceforward(f16v4, f16v4, f16v4);

    f16   += reflect(f16, f16);
    f16v2 += reflect(f16v2, f16v2);
    f16v3 += reflect(f16v3, f16v3);
    f16v4 += reflect(f16v4, f16v4);

    f16   += refract(f16, f16, f16);
    f16v2 += refract(f16v2, f16v2, f16);
    f16v3 += refract(f16v3, f16v3, f16);
    f16v4 += refract(f16v4, f16v4, f16);

    fragColor = vec3(f16) + vec3(f16v2.x) + vec3(f16v3) + vec3(f16v4.xyz);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: = call {{.*}} half @llvm.fabs.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<2 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.sqrt.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<3 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.sqrt.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<4 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.sqrt.f16(half
; SHADERTEST: = call {{.*}} half @llvm.fabs.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<2 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.sqrt.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<3 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.sqrt.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<4 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.sqrt.f16(half
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<3 x half> %{{.*}}, <3 x half> %{{.*}})
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<4 x half> %{{.*}}, <4 x half> %{{.*}})
; SHADERTEST: = call {{.*}} half (...) @lgc.create.dot.product.f16(<2 x half> %{{.*}}, <2 x half> %{{.*}})
; SHADERTEST: = call {{.*}} <3 x half> (...) @lgc.create.cross.product.v3f16(<3 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.normalize.vector.f16(half
; SHADERTEST: = call {{.*}} <2 x half> (...) @lgc.create.normalize.vector.v2f16(<2 x half>
; SHADERTEST: = call {{.*}} <3 x half> (...) @lgc.create.normalize.vector.v3f16(<3 x half>
; SHADERTEST: = call {{.*}} <4 x half> (...) @lgc.create.normalize.vector.v4f16(<4 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.face.forward.f16(half
; SHADERTEST: = call {{.*}} <2 x half> (...) @lgc.create.face.forward.v2f16(<2 x half>
; SHADERTEST: = call {{.*}} <3 x half> (...) @lgc.create.face.forward.v3f16(<3 x half>
; SHADERTEST: = call {{.*}} <4 x half> (...) @lgc.create.face.forward.v4f16(<4 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.reflect.f16(half
; SHADERTEST: = call {{.*}} <2 x half> (...) @lgc.create.reflect.v2f16(<2 x half>
; SHADERTEST: = call {{.*}} <3 x half> (...) @lgc.create.reflect.v3f16(<3 x half>
; SHADERTEST: = call {{.*}} <4 x half> (...) @lgc.create.reflect.v4f16(<4 x half>
; SHADERTEST: = call {{.*}} half (...) @lgc.create.refract.f16(half
; SHADERTEST: = call {{.*}} <2 x half> (...) @lgc.create.refract.v2f16(<2 x half>
; SHADERTEST: = call {{.*}} <3 x half> (...) @lgc.create.refract.v3f16(<3 x half>
; SHADERTEST: = call {{.*}} <4 x half> (...) @lgc.create.refract.v4f16(<4 x half>
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
