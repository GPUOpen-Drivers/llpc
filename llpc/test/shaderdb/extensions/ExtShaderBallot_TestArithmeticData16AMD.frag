#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/


#extension GL_AMD_shader_ballot: enable
#extension GL_AMD_gpu_shader_half_float: enable
#extension GL_AMD_gpu_shader_int16: enable

layout(location = 0) out vec4 fv4Out;

layout(location = 0) in flat i16vec2 i16v2In;
layout(location = 1) in flat u16vec3 u16v3In;
layout(location = 2) in flat f16vec4 f16v4In;

void main()
{
    vec4 fv4 = vec4(0.0);

    fv4.xy  += addInvocationsAMD(i16v2In);
    fv4.xyz += addInvocationsAMD(u16v3In);
    fv4     += addInvocationsAMD(f16v4In);

    fv4.xy  += addInvocationsNonUniformAMD(i16v2In);
    fv4.xyz += addInvocationsNonUniformAMD(u16v3In);
    fv4     += addInvocationsNonUniformAMD(f16v4In);

    fv4.xy  += minInvocationsAMD(i16v2In);
    fv4.xyz += minInvocationsAMD(u16v3In);
    fv4     += minInvocationsAMD(f16v4In);

    fv4.xy  += minInvocationsNonUniformAMD(i16v2In);
    fv4.xyz += minInvocationsNonUniformAMD(u16v3In);
    fv4     += minInvocationsNonUniformAMD(f16v4In);

    fv4.xy  += maxInvocationsAMD(i16v2In);
    fv4.xyz += maxInvocationsAMD(u16v3In);
    fv4     += maxInvocationsAMD(f16v4In);

    fv4.xy  += maxInvocationsNonUniformAMD(i16v2In);
    fv4.xyz += maxInvocationsNonUniformAMD(u16v3In);
    fv4     += maxInvocationsNonUniformAMD(f16v4In);

    fv4.xy  += addInvocationsInclusiveScanAMD(i16v2In);
    fv4.xyz += addInvocationsInclusiveScanAMD(u16v3In);
    fv4     += addInvocationsInclusiveScanAMD(f16v4In);

    fv4.xy  += addInvocationsInclusiveScanNonUniformAMD(i16v2In);
    fv4.xyz += addInvocationsInclusiveScanNonUniformAMD(u16v3In);
    fv4     += addInvocationsInclusiveScanNonUniformAMD(f16v4In);

    fv4.xy  += minInvocationsInclusiveScanAMD(i16v2In);
    fv4.xyz += minInvocationsInclusiveScanAMD(u16v3In);
    fv4     += minInvocationsInclusiveScanAMD(f16v4In);

    fv4.xy  += minInvocationsInclusiveScanNonUniformAMD(i16v2In);
    fv4.xyz += minInvocationsInclusiveScanNonUniformAMD(u16v3In);
    fv4     += minInvocationsInclusiveScanNonUniformAMD(f16v4In);

    fv4.xy  += maxInvocationsInclusiveScanAMD(i16v2In);
    fv4.xyz += maxInvocationsInclusiveScanAMD(u16v3In);
    fv4     += maxInvocationsInclusiveScanAMD(f16v4In);

    fv4.xy  += maxInvocationsInclusiveScanNonUniformAMD(i16v2In);
    fv4.xyz += maxInvocationsInclusiveScanNonUniformAMD(u16v3In);
    fv4     += maxInvocationsInclusiveScanNonUniformAMD(f16v4In);

    fv4.xy  += addInvocationsExclusiveScanAMD(i16v2In);
    fv4.xyz += addInvocationsExclusiveScanAMD(u16v3In);
    fv4     += addInvocationsExclusiveScanAMD(f16v4In);

    fv4.xy  += addInvocationsExclusiveScanNonUniformAMD(i16v2In);
    fv4.xyz += addInvocationsExclusiveScanNonUniformAMD(u16v3In);
    fv4     += addInvocationsExclusiveScanNonUniformAMD(f16v4In);

    fv4.xy  += minInvocationsExclusiveScanAMD(i16v2In);
    fv4.xyz += minInvocationsExclusiveScanAMD(u16v3In);
    fv4     += minInvocationsExclusiveScanAMD(f16v4In);

    fv4.xy  += minInvocationsExclusiveScanNonUniformAMD(i16v2In);
    fv4.xyz += minInvocationsExclusiveScanNonUniformAMD(u16v3In);
    fv4     += minInvocationsExclusiveScanNonUniformAMD(f16v4In);

    fv4.xy  += maxInvocationsExclusiveScanAMD(i16v2In);
    fv4.xyz += maxInvocationsExclusiveScanAMD(u16v3In);
    fv4     += maxInvocationsExclusiveScanAMD(f16v4In);

    fv4.xy  += maxInvocationsExclusiveScanNonUniformAMD(i16v2In);
    fv4.xyz += maxInvocationsExclusiveScanNonUniformAMD(u16v3In);
    fv4     += maxInvocationsExclusiveScanNonUniformAMD(f16v4In);

    fv4Out = fv4;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
