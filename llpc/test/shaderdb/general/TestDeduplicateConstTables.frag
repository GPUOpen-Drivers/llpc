// BEGIN_SHADERTEST
// RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
// SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
// SHADERTEST: [[VAR1:@.*]] = addrspace(5) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: [[VAR2:@.*]] = addrspace(5) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: [[VAR3:@.*]] = addrspace(5) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: [[VAR4:@.*]] = addrspace(5) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: [[VAR5:@.*]] = addrspace(5) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: [[VAR6:@.*]] = addrspace(5) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(5) [[VAR1]], i32
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(5) [[VAR2]], i32
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(5) [[VAR3]], i32
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(5) [[VAR4]], i32
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(5) [[VAR5]], i32
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(5) [[VAR6]], i32
//
// SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
// SHADERTEST: [[VAR:@.*]] = internal unnamed_addr addrspace(4) constant [28 x <4 x i32>] [<4 x i32> <i32 1059055552, i32 1057692236, i32 1062465857, i32 0
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(4) [[VAR]], i64
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(4) [[VAR]], i64
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(4) [[VAR]], i64
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(4) [[VAR]], i64
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(4) [[VAR]], i64
// SHADERTEST: getelementptr [28 x <4 x i32>], ptr addrspace(4) [[VAR]], i64
// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST


#version 460

const uvec4 _211[28] = uvec4[](uvec4(1059055552u, 1057692236u, 1062465857u, 0u), uvec4(3188284415u, 3211912104u, 1064587336u, 0u), uvec4(1053197116u, 3202258829u, 1058349232u, 0u), uvec4(1039685418u, 3192222363u, 1047084841u, 0u), uvec4(3192964587u, 1038352636u, 1047388173u, 0u), uvec4(1062520215u, 3197474974u, 1063356727u, 0u), uvec4(1037850661u, 3205763422u, 1058446204u, 0u), uvec4(1049678263u, 1061836040u, 1062659634u, 0u), uvec4(3199959344u, 1053446761u, 1057614893u, 0u), uvec4(1061258065u, 1046506363u, 1061780340u, 0u), uvec4(3204886141u, 1019442700u, 1057411553u, 0u), uvec4(3210859837u, 3195704642u, 1063935038u, 0u), uvec4(3204075131u, 3197512555u, 1058140859u, 0u), uvec4(1054956040u, 3182380177u, 1055232528u, 0u), uvec4(1046011770u, 1057194959u, 1057898596u, 0u), uvec4(1029739884u, 1064631963u, 1064658304u, 0u), uvec4(3205958373u, 3207889095u, 1064000637u, 0u), uvec4(3209492326u, 1048328368u, 1062629938u, 0u), uvec4(3192317658u, 3191631805u, 1049165551u, 0u), uvec4(3202323254u, 1061452681u, 1063390953u, 0u), uvec4(1046338590u, 1038982117u, 1048292130u, 0u), uvec4(1042332191u, 3210420945u, 1063176708u, 0u), uvec4(3208780301u, 1058367687u, 1064628776u, 0u), uvec4(1065233091u, 3169707339u, 1065240305u, 0u), uvec4(3194203417u, 3205774830u, 1058982739u, 0u), uvec4(1057812193u, 3207297698u, 1063122517u, 0u), uvec4(1055767050u, 1049621221u, 1057682673u, 0u), uvec4(3180576291u, 1058735275u, 1058807082u, 0u));

layout(binding = 0) uniform Uniforms
{
  int in1;
};

layout(location = 0) out vec4 _out;

void main()
{
    for (uint i = 0; i < 28; i++)
    {
        if (in1 != 1)
        {
            _out = _out + vec4(_211[i+in1].x, _211[i].y, _211[i].wx);
        }
        _out = _out + vec4(_211[in1].x, _211[i].y, _211[i].wx);
    }
}

