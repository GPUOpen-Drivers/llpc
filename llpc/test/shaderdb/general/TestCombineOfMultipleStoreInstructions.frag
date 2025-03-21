#version 460
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


// BEGIN_SHADERTEST
// RUN: amdllpc -verify-ir -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s

// SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
// SHADERTEST: @{{.*}} = internal unnamed_addr addrspace(4) constant [54 x float] [float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float -1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float -1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00, float 0.000000e+00, float -1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float -1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00]
// SHADERTEST: AMDLLPC SUCCESS
// END_SHADERTEST

layout(push_constant, std430) uniform RootConstants
{
    uvec2 _m0;
    uint _m1;
    uint _m2;
    uint _m3;
    uint _m4;
    uint _m5;
    uint _m6;
    uint _m7;
} registers;

layout(location = 0) out vec4 SV_Target;
float _49[54];

void main()
{
    _49[0u] = 0.0;
    _49[1u] = 0.0;
    _49[2u] = 1.0;
    _49[3u] = 0.0;
    _49[4u] = 1.0;
    _49[5u] = 0.0;
    _49[6u] = -1.0;
    _49[7u] = 0.0;
    _49[8u] = 0.0;
    _49[9u] = 0.0;
    _49[10u] = 0.0;
    _49[11u] = -1.0;
    _49[12u] = 0.0;
    _49[13u] = 1.0;
    _49[14u] = 0.0;
    _49[15u] = 1.0;
    _49[16u] = 0.0;
    _49[17u] = 0.0;
    _49[18u] = -1.0;
    _49[19u] = 0.0;
    _49[20u] = 0.0;
    _49[21u] = 0.0;
    _49[22u] = 0.0;
    _49[23u] = 1.0;
    _49[24u] = 0.0;
    _49[25u] = 1.0;
    _49[26u] = 0.0;
    _49[27u] = -1.0;
    _49[28u] = 0.0;
    _49[29u] = 0.0;
    _49[30u] = 0.0;
    _49[31u] = 0.0;
    _49[32u] = -1.0;
    _49[33u] = 0.0;
    _49[34u] = -1.0;
    _49[35u] = 0.0;
    _49[36u] = 1.0;
    _49[37u] = 0.0;
    _49[38u] = 0.0;
    _49[39u] = 0.0;
    _49[40u] = 1.0;
    _49[41u] = 0.0;
    _49[42u] = 0.0;
    _49[43u] = 0.0;
    _49[44u] = 1.0;
    _49[45u] = -1.0;
    _49[46u] = 0.0;
    _49[47u] = 0.0;
    _49[48u] = 0.0;
    _49[49u] = 1.0;
    _49[50u] = 0.0;
    _49[51u] = 0.0;
    _49[52u] = 0.0;
    _49[53u] = -1.0;

    uvec4 _160 = uvec4(registers._m1, registers._m2, registers._m3, registers._m4);
    SV_Target.z = _49[2u + _160.z];
    SV_Target.w = _49[3u + _160.w];
}

