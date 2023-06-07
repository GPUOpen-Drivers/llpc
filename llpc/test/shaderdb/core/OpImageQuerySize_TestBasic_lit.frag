#version 450 core

layout(set = 0, binding = 0) uniform sampler1D samp1D;
layout(set = 0, binding = 1) uniform sampler2D samp2D;
layout(set = 0, binding = 2) uniform sampler3D samp3D;
layout(set = 0, binding = 3) uniform samplerCube sampCube;
layout(set = 0, binding = 4) uniform sampler1DShadow samp1DS;
layout(set = 0, binding = 5) uniform sampler2DShadow samp2DS;
layout(set = 0, binding = 6) uniform samplerCubeShadow sampCubeS;
layout(set = 0, binding = 7) uniform samplerCubeArray sampCubeA;
layout(set = 0, binding = 8) uniform samplerCubeArrayShadow sampCubeAS;
layout(set = 0, binding = 9) uniform sampler2DRect samp2DRect;
layout(set = 0, binding = 10) uniform sampler2DRectShadow samp2DRectS;
layout(set = 0, binding = 11) uniform sampler1DArray samp1DA;
layout(set = 0, binding = 12) uniform sampler2DArray samp2DA;
layout(set = 0, binding = 13) uniform sampler1DArrayShadow samp1DAS;
layout(set = 0, binding = 14) uniform sampler2DArrayShadow samp2DAS;
layout(set = 0, binding = 15) uniform samplerBuffer sampBuffer;
layout(set = 0, binding = 16) uniform sampler2DMS samp2DMS;
layout(set = 0, binding = 17) uniform sampler2DMSArray samp2DMSA;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(0, 0, 0, 0);

    int size1D = textureSize(samp1D, 0);
    oColor += vec4(size1D, 0, 0, 0);

    ivec2 size2D = textureSize(samp2D, 0);
    oColor += vec4(size2D.x, size2D.y, 0, 0);

    ivec3 size3D = textureSize(samp3D, 0);
    oColor += vec4(size3D.x, size3D.y, size3D.z, 0);

    ivec2 sizeCube = textureSize(sampCube, 0);
    oColor += vec4(sizeCube.x, sizeCube.y, 0, 0);

    int size1DS = textureSize(samp1DS, 0);
    oColor += vec4(size1DS, 0, 0, 0);

    ivec2 size2DS = textureSize(samp2DS, 0);
    oColor += vec4(size2DS.x, size2DS.y, 0, 0);

    ivec2 sizeCubeS = textureSize(sampCubeS, 0);
    oColor += vec4(sizeCubeS.x, sizeCubeS.y, 0, 0);

    ivec3 sizeCubeA = textureSize(sampCubeA, 0);
    oColor += vec4(sizeCubeA.x, sizeCubeA.y, sizeCubeA.z, 0);

    ivec3 sizeCubeAS = textureSize(sampCubeAS, 0);
    oColor += vec4(sizeCubeAS.x, sizeCubeAS.y, sizeCubeAS.z, 0);

    ivec2 size2DRect = textureSize(samp2DRect);
    oColor += vec4(size2DRect.x, size2DRect.y, 0, 0);

    ivec2 size2DRectS = textureSize(samp2DRectS);
    oColor += vec4(size2DRectS.x, size2DRectS.y, 0, 0);

    ivec2 size1DA = textureSize(samp1DA, 0);
    oColor += vec4(size1DA.x, size1DA.y, 0, 0);

    ivec3 size2DA = textureSize(samp2DA, 0);
    oColor += vec4(size2DA.x, size2DA.y, size2DA.z, 0);

    int sizeBuffer = textureSize(sampBuffer);
    oColor += vec4(sizeBuffer, 0, 0, 0);

    ivec2 size2DMS = textureSize(samp2DMS);
    oColor += vec4(size2DMS.x, size2DMS.y, 0, 0);

    ivec3 size2DMSA = textureSize(samp2DMSA);
    oColor += vec4(size2DMSA.x, size2DMSA.y, size2DMSA.z, 0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}}  SPIR-V lowering results
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 0, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 1)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 2)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 2, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 3)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 3, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 4)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 0, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 5)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 6)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 3, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 7)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 8, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 8)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 8, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 9)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 10)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 1, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 11)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 4, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 12)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 5, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.i32(i32 0, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 16)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v2i32(i32 6, i32 512, {{.*}}, i32 0)
; SHADERTEST: call {{.*}} @lgc.create.get.desc.ptr.p4(i32 1, i32 1, i64 0, i32 17)
; SHADERTEST: call {{.*}} @lgc.create.image.query.size.v3i32(i32 7, i32 512, {{.*}}, i32 0)

; SHADERTEST-LABEL: {{^// LLPC}}  pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
