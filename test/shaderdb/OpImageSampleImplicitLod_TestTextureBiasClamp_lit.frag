#version 450
#extension GL_ARB_sparse_texture_clamp : enable

layout(set = 0, binding = 0) uniform sampler1D          samp1D[4];
layout(set = 1, binding = 0) uniform sampler2D          samp2D;
layout(set = 2, binding = 0) uniform sampler3D          samp3D;
layout(set = 3, binding = 0) uniform samplerCube        sampCube;
layout(set = 4, binding = 0) uniform sampler1DArray     samp1DArray;
layout(set = 5, binding = 0) uniform sampler2DArray     samp2DArray;
layout(set = 6, binding = 0) uniform samplerCubeArray   sampCubeArray;

layout(set = 7, binding = 0) uniform Uniforms
{
    int   index;
    float lodClamp;
};

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(0.0);

    fragColor += textureClampARB(samp1D[index], 0.1, lodClamp, 2.0);

    fragColor += textureClampARB(samp2D, vec2(0.1), lodClamp, 2.0);

    fragColor += textureClampARB(samp3D, vec3(0.1), lodClamp, 2.0);

    fragColor += textureClampARB(sampCube, vec3(0.1), lodClamp, 2.0);

    fragColor += textureClampARB(samp1DArray, vec2(0.1), lodClamp, 2.0);

    fragColor += textureClampARB(samp2DArray, vec3(0.1), lodClamp, 2.0);

    fragColor += textureClampARB(sampCubeArray, vec4(0.1), lodClamp, 2.0);
}

// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: <4 x float> @spirv.image.sample.f32.1D.bias.minlod({{.*}}, float 0x3FB99999A0000000, float 2.000000e+00, {{.*}})
; SHADERTEST: <4 x float> @spirv.image.sample.f32.2D.bias.minlod({{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: <4 x float> @spirv.image.sample.f32.3D.bias.minlod({{.*}}, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: <4 x float> @spirv.image.sample.f32.Cube.bias.minlod({{.*}}, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: <4 x float> @spirv.image.sample.f32.1DArray.bias.minlod({{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: <4 x float> @spirv.image.sample.f32.2DArray.bias.minlod({{.*}}, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: <4 x float> @spirv.image.sample.f32.CubeArray.bias.minlod({{.*}}, <4 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.1D.bias.minlod{{.*}}({{.*}}, float 0x3FB99999A0000000, float 2.000000e+00, {{.*}})
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.2D.bias.minlod{{.*}}({{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.3D.bias.minlod{{.*}}({{.*}}, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.Cube.bias.minlod{{.*}}({{.*}}, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.1DArray.bias.minlod{{.*}}({{.*}}, <2 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.2DArray.bias.minlod{{.*}}({{.*}}, <3 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})
; SHADERTEST: call <4 x i32>{{.*}}@llpc.call.load.sampler.desc.v4i32
; SHADERTEST: call <8 x i32>{{.*}}@llpc.call.load.resource.desc.v8i32
; SHADERTEST: call <4 x float> @llpc.image.sample.f32.CubeArray.bias.minlod{{.*}}({{.*}}, <4 x float> <float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000>, float 2.000000e+00, {{.*}})

; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: call i32 @llvm.amdgcn.readfirstlane
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.1d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.2d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float 0x3FB99999A0000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.3d.v4f32.f32.f32({{.*}}, float 2.000000e+00, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call float @llvm.amdgcn.cubesc(float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000)
; SHADERTEST: call float @llvm.amdgcn.cubetc(float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000)
; SHADERTEST: call float @llvm.amdgcn.cubema(float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000)
; SHADERTEST: call float @llvm.amdgcn.cubeid(float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0x3FB99999A0000000)
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.cube.v4f32.f32.f32({{.*}}, float 2.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.1darray.v4f32.f32.f32({{.*}}, float 2.000000e+00, float 0x3FB99999A0000000, float 0.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.2darray.v4f32.f32.f32({{.*}}, float 2.000000e+00, float 0x3FB99999A0000000, float 0x3FB99999A0000000, float 0.000000e+00, {{.*}})
; SHADERTEST: load <4 x i32>, <4 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: load <8 x i32>, <8 x i32> addrspace(4)* %{{[0-9]*}}
; SHADERTEST: call <4 x float> @llvm.amdgcn.image.sample.b.cl.cube.v4f32.f32.f32({{.*}}, float 2.000000e+00, {{.*}})

; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
