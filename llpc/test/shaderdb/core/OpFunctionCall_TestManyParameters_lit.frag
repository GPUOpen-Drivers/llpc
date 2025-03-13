#version 450
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


layout(location = 0) out vec4 fragColor;

bool func(
    int   i1,
    ivec2 i2,
    ivec3 i3,
    ivec4 i4,
    float f1,
    vec2  f2,
    vec3  f3,
    vec4  f4,
    bool  b1,
    bvec2 b2,
    bvec3 b3,
    bvec4 b4)
{
    bool value = false;

    value = any(b4) && all(b3) && any(b2) && b1;

    if (value == true)
    {
        value = ((f4.y + f3.y + f2.x + f1) >= 0.0);
    }
    else
    {
        value = ((i4.x - i3.y + i2.x - i1) == 6);
    }

    return value;
}

void main()
{
    bool value = func(1,
                      ivec2(6),
                      ivec3(7),
                      ivec4(-3),
                      0.5,
                      vec2(1.0),
                      vec3(5.0),
                      vec4(-7.0),
                      false,
                      bvec2(true),
                      bvec3(false),
                      bvec4(true));

    fragColor = (value == false) ? vec4(1.0) : vec4(0.5);
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST: call {{.*}} i1 @{{.*}}
; SHADERTEST: define internal {{.*}} i1 @{{.*}}(ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}}, ptr{{.*}})
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
