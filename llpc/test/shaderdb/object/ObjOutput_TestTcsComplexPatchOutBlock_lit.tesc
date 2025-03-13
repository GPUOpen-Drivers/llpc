#version 450 core
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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


layout(vertices = 5) out;

struct S
{
    int   x;
    vec4  y;
    float z[2];
};

layout(location = 2) patch out TheBlock
{
    float blockFa[3];
    S     blockSa[2];
    float blockF;
} tcBlock[2];

void main(void)
{
    float v = 1.3;

    for (int i0 = 0; i0 < 2; ++i0)
    {
        for (int i1 = 0; i1 < 3; ++i1)
        {
            tcBlock[i0].blockFa[i1] = v;
            v += 0.4;
        }

        for (int i1 = 0; i1 < 2; ++i1)
        {
            tcBlock[i0].blockSa[i1].x = int(v);
            v += 0.4;

            tcBlock[i0].blockSa[i1].y = vec4(v, v + 0.8, v + 1.6, v + 2.4);
            v += 0.4;

            for (int i2 = 0; i2 < 2; ++i2)
            {
                tcBlock[i0].blockSa[i1].z[i2] = v;
                v += 0.4;
            }
        }

        tcBlock[i0].blockF = v;
        v += 0.4;
    }

    gl_TessLevelInner[0] = 2.0;
    gl_TessLevelOuter[0] = 3.0;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} FE lowering results
; SHADERTEST: call void @lgc.output.export.generic{{.*}}f32
; SHADERTEST: void @lgc.output.export.generic{{.*}}
; SHADERTEST: call void @lgc.output.export.generic{{.*}}v4f32
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
