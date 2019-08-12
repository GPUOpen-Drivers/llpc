;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 ;
 ;  Permission is hereby granted, free of charge, to any person obtaining a copy
 ;  of this software and associated documentation files (the "Software"), to deal
 ;  in the Software without restriction, including without limitation the rights
 ;  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 ;  copies of the Software, and to permit persons to whom the Software is
 ;  furnished to do so, subject to the following conditions:
 ;
 ;  The above copyright notice and this permission notice shall be included in all
 ;  copies or substantial portions of the Software.
 ;
 ;  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ;  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ;  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ;  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ;  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 ;  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 ;  SOFTWARE.
 ;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

target datalayout = "e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5"

; GLSL: mat2 = transpose(mat2)
define spir_func [2 x <2 x float>] @_Z9TransposeDv2_Dv2_f(
    [2 x <2 x float>] %m) #0
{
    %nm = alloca [2 x <2 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <2 x float>], [2 x <2 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [2 x <2 x float>], [2 x <2 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm1, i32 0, i32 1

    %m0v = extractvalue [2 x <2 x float>] %m, 0
    %m0v0 = extractelement <2 x float> %m0v, i32 0
    %m0v1 = extractelement <2 x float> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x float>] %m, 1
    %m1v0 = extractelement <2 x float> %m1v, i32 0
    %m1v1 = extractelement <2 x float> %m1v, i32 1

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    %nmv = load [2 x <2 x float>], [2 x <2 x float>] addrspace(5)* %nm
    ret [2 x <2 x float>] %nmv
}

; GLSL: mat3 = transpose(mat3)
define spir_func [3 x <3 x float>] @_Z9TransposeDv3_Dv3_f(
    [3 x <3 x float>] %m) #0
{
    %nm = alloca [3 x <3 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <3 x float>], [3 x <3 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [3 x <3 x float>], [3 x <3 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 2

    %nm2 = getelementptr inbounds [3 x <3 x float>], [3 x <3 x float>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm2, i32 0, i32 2

    %m0v = extractvalue [3 x <3 x float>] %m, 0
    %m0v0 = extractelement <3 x float> %m0v, i32 0
    %m0v1 = extractelement <3 x float> %m0v, i32 1
    %m0v2 = extractelement <3 x float> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x float>] %m, 1
    %m1v0 = extractelement <3 x float> %m1v, i32 0
    %m1v1 = extractelement <3 x float> %m1v, i32 1
    %m1v2 = extractelement <3 x float> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x float>] %m, 2
    %m2v0 = extractelement <3 x float> %m2v, i32 0
    %m2v1 = extractelement <3 x float> %m2v, i32 1
    %m2v2 = extractelement <3 x float> %m2v, i32 2

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m2v0, float addrspace(5)* %nm02
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m2v1, float addrspace(5)* %nm12
    store float %m0v2, float addrspace(5)* %nm20
    store float %m1v2, float addrspace(5)* %nm21
    store float %m2v2, float addrspace(5)* %nm22
    %nmv = load [3 x <3 x float>], [3 x <3 x float>] addrspace(5)* %nm
    ret [3 x <3 x float>] %nmv
}

; GLSL: mat4 = transpose(mat4)
define spir_func [4 x <4 x float>] @_Z9TransposeDv4_Dv4_f(
    [4 x <4 x float>] %m) #0
{
    %nm = alloca [4 x <4 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <4 x float>], [4 x <4 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [4 x <4 x float>], [4 x <4 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 3

    %nm2 = getelementptr inbounds [4 x <4 x float>], [4 x <4 x float>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 2
    %nm23 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 3

    %nm3 = getelementptr inbounds [4 x <4 x float>], [4 x <4 x float>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm3, i32 0, i32 1
    %nm32 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm3, i32 0, i32 2
    %nm33 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm3, i32 0, i32 3

    %m0v = extractvalue [4 x <4 x float>] %m, 0
    %m0v0 = extractelement <4 x float> %m0v, i32 0
    %m0v1 = extractelement <4 x float> %m0v, i32 1
    %m0v2 = extractelement <4 x float> %m0v, i32 2
    %m0v3 = extractelement <4 x float> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x float>] %m, 1
    %m1v0 = extractelement <4 x float> %m1v, i32 0
    %m1v1 = extractelement <4 x float> %m1v, i32 1
    %m1v2 = extractelement <4 x float> %m1v, i32 2
    %m1v3 = extractelement <4 x float> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x float>] %m, 2
    %m2v0 = extractelement <4 x float> %m2v, i32 0
    %m2v1 = extractelement <4 x float> %m2v, i32 1
    %m2v2 = extractelement <4 x float> %m2v, i32 2
    %m2v3 = extractelement <4 x float> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x float>] %m, 3
    %m3v0 = extractelement <4 x float> %m3v, i32 0
    %m3v1 = extractelement <4 x float> %m3v, i32 1
    %m3v2 = extractelement <4 x float> %m3v, i32 2
    %m3v3 = extractelement <4 x float> %m3v, i32 3

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m2v0, float addrspace(5)* %nm02
    store float %m3v0, float addrspace(5)* %nm03
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m2v1, float addrspace(5)* %nm12
    store float %m3v1, float addrspace(5)* %nm13
    store float %m0v2, float addrspace(5)* %nm20
    store float %m1v2, float addrspace(5)* %nm21
    store float %m2v2, float addrspace(5)* %nm22
    store float %m3v2, float addrspace(5)* %nm23
    store float %m0v3, float addrspace(5)* %nm30
    store float %m1v3, float addrspace(5)* %nm31
    store float %m2v3, float addrspace(5)* %nm32
    store float %m3v3, float addrspace(5)* %nm33
    %nmv = load [4 x <4 x float>], [4 x <4 x float>] addrspace(5)* %nm
    ret [4 x <4 x float>] %nmv
}

; GLSL: mat2x3 = transpose(mat3x2)
define spir_func [2 x <3 x float>] @_Z9TransposeDv3_Dv2_f(
    [3 x <2 x float>] %m) #0
{
    %nm = alloca [2 x <3 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <3 x float>], [2 x <3 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [2 x <3 x float>], [2 x <3 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 2

    %m0v = extractvalue [3 x <2 x float>] %m, 0
    %m0v0 = extractelement <2 x float> %m0v, i32 0
    %m0v1 = extractelement <2 x float> %m0v, i32 1

    %m1v = extractvalue [3 x <2 x float>] %m, 1
    %m1v0 = extractelement <2 x float> %m1v, i32 0
    %m1v1 = extractelement <2 x float> %m1v, i32 1

    %m2v = extractvalue [3 x <2 x float>] %m, 2
    %m2v0 = extractelement <2 x float> %m2v, i32 0
    %m2v1 = extractelement <2 x float> %m2v, i32 1

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m2v0, float addrspace(5)* %nm02
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m2v1, float addrspace(5)* %nm12
    %nmv = load [2 x <3 x float>], [2 x <3 x float>] addrspace(5)* %nm
    ret [2 x <3 x float>] %nmv
}

; GLSL: mat3x2 = transpose(mat2x3)
define spir_func [3 x <2 x float>] @_Z9TransposeDv2_Dv3_f(
    [2 x <3 x float>] %m) #0
{
    %nm = alloca [3 x <2 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <2 x float>], [3 x <2 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [3 x <2 x float>], [3 x <2 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm1, i32 0, i32 1

    %nm2 = getelementptr inbounds [3 x <2 x float>], [3 x <2 x float>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm2, i32 0, i32 1

    %m0v = extractvalue [2 x <3 x float>] %m, 0
    %m0v0 = extractelement <3 x float> %m0v, i32 0
    %m0v1 = extractelement <3 x float> %m0v, i32 1
    %m0v2 = extractelement <3 x float> %m0v, i32 2

    %m1v = extractvalue [2 x <3 x float>] %m, 1
    %m1v0 = extractelement <3 x float> %m1v, i32 0
    %m1v1 = extractelement <3 x float> %m1v, i32 1
    %m1v2 = extractelement <3 x float> %m1v, i32 2

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m0v2, float addrspace(5)* %nm20
    store float %m1v2, float addrspace(5)* %nm21
    %nmv = load [3 x <2 x float>], [3 x <2 x float>] addrspace(5)* %nm
    ret [3 x <2 x float>] %nmv
}

; GLSL: mat2x4 = transpose(mat4x2)
define spir_func [2 x <4 x float>] @_Z9TransposeDv4_Dv2_f(
    [4 x <2 x float>] %m) #0
{
    %nm = alloca [2 x <4 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <4 x float>], [2 x <4 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [2 x <4 x float>], [2 x <4 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 3

    %m0v = extractvalue [4 x <2 x float>] %m, 0
    %m0v0 = extractelement <2 x float> %m0v, i32 0
    %m0v1 = extractelement <2 x float> %m0v, i32 1

    %m1v = extractvalue [4 x <2 x float>] %m, 1
    %m1v0 = extractelement <2 x float> %m1v, i32 0
    %m1v1 = extractelement <2 x float> %m1v, i32 1

    %m2v = extractvalue [4 x <2 x float>] %m, 2
    %m2v0 = extractelement <2 x float> %m2v, i32 0
    %m2v1 = extractelement <2 x float> %m2v, i32 1

    %m3v = extractvalue [4 x <2 x float>] %m, 3
    %m3v0 = extractelement <2 x float> %m3v, i32 0
    %m3v1 = extractelement <2 x float> %m3v, i32 1

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m2v0, float addrspace(5)* %nm02
    store float %m3v0, float addrspace(5)* %nm03
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m2v1, float addrspace(5)* %nm12
    store float %m3v1, float addrspace(5)* %nm13
    %nmv = load [2 x <4 x float>], [2 x <4 x float>] addrspace(5)* %nm
    ret [2 x <4 x float>] %nmv
}

; GLSL: mat4x2 = transpose(mat2x4)
define spir_func [4 x <2 x float>] @_Z9TransposeDv2_Dv4_f(
    [2 x <4 x float>] %m) #0
{
    %nm = alloca [4 x <2 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <2 x float>], [4 x <2 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [4 x <2 x float>], [4 x <2 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm1, i32 0, i32 1

    %nm2 = getelementptr inbounds [4 x <2 x float>], [4 x <2 x float>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm2, i32 0, i32 1

    %nm3 = getelementptr inbounds [4 x <2 x float>], [4 x <2 x float>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <2 x float>, <2 x float> addrspace(5)* %nm3, i32 0, i32 1

    %m0v = extractvalue [2 x <4 x float>] %m, 0
    %m0v0 = extractelement <4 x float> %m0v, i32 0
    %m0v1 = extractelement <4 x float> %m0v, i32 1
    %m0v2 = extractelement <4 x float> %m0v, i32 2
    %m0v3 = extractelement <4 x float> %m0v, i32 3

    %m1v = extractvalue [2 x <4 x float>] %m, 1
    %m1v0 = extractelement <4 x float> %m1v, i32 0
    %m1v1 = extractelement <4 x float> %m1v, i32 1
    %m1v2 = extractelement <4 x float> %m1v, i32 2
    %m1v3 = extractelement <4 x float> %m1v, i32 3

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m0v2, float addrspace(5)* %nm20
    store float %m1v2, float addrspace(5)* %nm21
    store float %m0v3, float addrspace(5)* %nm30
    store float %m1v3, float addrspace(5)* %nm31
    %nmv = load [4 x <2 x float>], [4 x <2 x float>] addrspace(5)* %nm
    ret [4 x <2 x float>] %nmv
}

; GLSL: mat3x4 = transpose(mat4x3)
define spir_func [3 x <4 x float>] @_Z9TransposeDv4_Dv3_f(
    [4 x <3 x float>] %m) #0
{
    %nm = alloca [3 x <4 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <4 x float>], [3 x <4 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [3 x <4 x float>], [3 x <4 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm1, i32 0, i32 3

    %nm2 = getelementptr inbounds [3 x <4 x float>], [3 x <4 x float>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 2
    %nm23 = getelementptr inbounds <4 x float>, <4 x float> addrspace(5)* %nm2, i32 0, i32 3

    %m0v = extractvalue [4 x <3 x float>] %m, 0
    %m0v0 = extractelement <3 x float> %m0v, i32 0
    %m0v1 = extractelement <3 x float> %m0v, i32 1
    %m0v2 = extractelement <3 x float> %m0v, i32 2

    %m1v = extractvalue [4 x <3 x float>] %m, 1
    %m1v0 = extractelement <3 x float> %m1v, i32 0
    %m1v1 = extractelement <3 x float> %m1v, i32 1
    %m1v2 = extractelement <3 x float> %m1v, i32 2

    %m2v = extractvalue [4 x <3 x float>] %m, 2
    %m2v0 = extractelement <3 x float> %m2v, i32 0
    %m2v1 = extractelement <3 x float> %m2v, i32 1
    %m2v2 = extractelement <3 x float> %m2v, i32 2

    %m3v = extractvalue [4 x <3 x float>] %m, 3
    %m3v0 = extractelement <3 x float> %m3v, i32 0
    %m3v1 = extractelement <3 x float> %m3v, i32 1
    %m3v2 = extractelement <3 x float> %m3v, i32 2

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m2v0, float addrspace(5)* %nm02
    store float %m3v0, float addrspace(5)* %nm03
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m2v1, float addrspace(5)* %nm12
    store float %m3v1, float addrspace(5)* %nm13
    store float %m0v2, float addrspace(5)* %nm20
    store float %m1v2, float addrspace(5)* %nm21
    store float %m2v2, float addrspace(5)* %nm22
    store float %m3v2, float addrspace(5)* %nm23
    %nmv = load [3 x <4 x float>], [3 x <4 x float>] addrspace(5)* %nm
    ret [3 x <4 x float>] %nmv
}

; GLSL: mat4x3 = transpose(mat3x4)
define spir_func [4 x <3 x float>] @_Z9TransposeDv3_Dv4_f(
    [3 x <4 x float>] %m) #0
{
    %nm = alloca [4 x <3 x float>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <3 x float>], [4 x <3 x float>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [4 x <3 x float>], [4 x <3 x float>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm1, i32 0, i32 2

    %nm2 = getelementptr inbounds [4 x <3 x float>], [4 x <3 x float>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm2, i32 0, i32 2

    %nm3 = getelementptr inbounds [4 x <3 x float>], [4 x <3 x float>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm3, i32 0, i32 1
    %nm32 = getelementptr inbounds <3 x float>, <3 x float> addrspace(5)* %nm3, i32 0, i32 2

    %m0v = extractvalue [3 x <4 x float>] %m, 0
    %m0v0 = extractelement <4 x float> %m0v, i32 0
    %m0v1 = extractelement <4 x float> %m0v, i32 1
    %m0v2 = extractelement <4 x float> %m0v, i32 2
    %m0v3 = extractelement <4 x float> %m0v, i32 3

    %m1v = extractvalue [3 x <4 x float>] %m, 1
    %m1v0 = extractelement <4 x float> %m1v, i32 0
    %m1v1 = extractelement <4 x float> %m1v, i32 1
    %m1v2 = extractelement <4 x float> %m1v, i32 2
    %m1v3 = extractelement <4 x float> %m1v, i32 3

    %m2v = extractvalue [3 x <4 x float>] %m, 2
    %m2v0 = extractelement <4 x float> %m2v, i32 0
    %m2v1 = extractelement <4 x float> %m2v, i32 1
    %m2v2 = extractelement <4 x float> %m2v, i32 2
    %m2v3 = extractelement <4 x float> %m2v, i32 3

    store float %m0v0, float addrspace(5)* %nm00
    store float %m1v0, float addrspace(5)* %nm01
    store float %m2v0, float addrspace(5)* %nm02
    store float %m0v1, float addrspace(5)* %nm10
    store float %m1v1, float addrspace(5)* %nm11
    store float %m2v1, float addrspace(5)* %nm12
    store float %m0v2, float addrspace(5)* %nm20
    store float %m1v2, float addrspace(5)* %nm21
    store float %m2v2, float addrspace(5)* %nm22
    store float %m0v3, float addrspace(5)* %nm30
    store float %m1v3, float addrspace(5)* %nm31
    store float %m2v3, float addrspace(5)* %nm32
    %nmv = load [4 x <3 x float>], [4 x <3 x float>] addrspace(5)* %nm
    ret [4 x <3 x float>] %nmv
}

; GLSL helper: float = determinant2(vec2(float, float), vec2(float, float))
define spir_func float @llpc.determinant2.f32(
    float %x0, float %y0, float %x1, float %y1)
{
    ; | x0   x1 |
    ; |         | = x0 * y1 - y0 * x1
    ; | y0   y1 |

    %1 = fmul float %x0, %y1
    %2 = fmul float %y0, %x1
    %3 = fsub float %1, %2
    ret float %3
}

; GLSL helper: float = determinant3(vec3(float, float, float), vec3(float, float, float))
define spir_func float @llpc.determinant3.f32(
    float %x0, float %y0, float %z0,
    float %x1, float %y1, float %z1,
    float %x2, float %y2, float %z2)
{
    ; | x0   x1   x2 |
    ; |              |        | y1 y2 |        | x1 x2 |        | x1 x2 |
    ; | y0   y1   y2 | = x0 * |       | - y0 * |       | + z0 * |       |
    ; |              |        | z1 z2 |        | z1 z2 |        | y1 y2 |
    ; | z0   z1   z2 |
    ;
    ;                         | y1 y2 |        | z1 z2 |        | x1 x2 |
    ;                  = x0 * |       | + y0 * |       | + z0 * |       |
    ;                         | z1 z2 |        | x1 x2 |        | y1 y2 |

    %1 = call float @llpc.determinant2.f32(float %y1, float %z1, float %y2, float %z2)
    %2 = fmul float %1, %x0
    %3 = call float @llpc.determinant2.f32(float %z1, float %x1, float %z2, float %x2)
    %4 = fmul float %3, %y0
    %5 = fadd float %2, %4
    %6 = call float @llpc.determinant2.f32(float %x1, float %y1, float %x2, float %y2)
    %7 = fmul float %6, %z0
    %8 = fadd float %7, %5
    ret float %8
}

; GLSL helper: float = determinant4(vec4(float, float, float, float), vec4(float, float, float, float))
define spir_func float @llpc.determinant4.f32(
    float %x0, float %y0, float %z0, float %w0,
    float %x1, float %y1, float %z1, float %w1,
    float %x2, float %y2, float %z2, float %w2,
    float %x3, float %y3, float %z3, float %w3)

{
    ; | x0   x1   x2   x3 |
    ; |                   |        | y1 y2 y3 |        | x1 x2 x3 |
    ; | y0   y1   y2   y3 |        |          |        |          |
    ; |                   | = x0 * | z1 z2 z3 | - y0 * | z1 z2 z3 | +
    ; | z0   z1   z2   z3 |        |          |        |          |
    ; |                   |        | w1 w2 w3 |        | w1 w2 w3 |
    ; | w0   w1   w2   w3 |
    ;
    ;                              | x1 x2 x3 |        | x1 x2 x3 |
    ;                              |          |        |          |
    ;                         z0 * | y1 y2 y3 | - w0 * | y1 y2 y3 |
    ;                              |          |        |          |
    ;                              | w1 w2 w3 |        | z1 z2 z3 |
    ;
    ;
    ;                              | y1 y2 y3 |        | z1 z2 z3 |
    ;                              |          |        |          |
    ;                       = x0 * | z1 z2 z3 | + y0 * | x1 x2 x3 | +
    ;                              |          |        |          |
    ;                              | w1 w2 w3 |        | w1 w2 w3 |
    ;
    ;                              | x1 x2 x3 |        | y1 y2 y3 |
    ;                              |          |        |          |
    ;                         z0 * | y1 y2 y3 | + w0 * | x1 x2 x3 |
    ;                              |          |        |          |
    ;                              | w1 w2 w3 |        | z1 z2 z3 |

    %1 = call float @llpc.determinant3.f32(float %y1, float %z1, float %w1, float %y2, float %z2, float %w2, float %y3, float %z3, float %w3)
    %2 = fmul float %1, %x0
    %3 = call float @llpc.determinant3.f32(float %z1, float %x1, float %w1, float %z2, float %x2, float %w2, float %z3, float %x3, float %w3)
    %4 = fmul float %3, %y0
    %5 = fadd float %2, %4
    %6 = call float @llpc.determinant3.f32(float %x1, float %y1, float %w1, float %x2, float %y2, float %w2, float %x3, float %y3, float %w3)
    %7 = fmul float %6, %z0
    %8 = fadd float %5, %7
    %9 = call float @llpc.determinant3.f32(float %y1, float %x1, float %z1, float %y2, float %x2, float %z2, float %y3, float %x3, float %z3)
    %10 = fmul float %9, %w0
    %11 = fadd float %8, %10
    ret float %11
}

; GLSL: float = determinant(mat2)
define spir_func float @_Z11determinantDv2_Dv2_f(
    [2 x <2 x float>] %m) #0
{
    %m0v = extractvalue [2 x <2 x float>] %m, 0
    %m0v0 = extractelement <2 x float> %m0v, i32 0
    %m0v1 = extractelement <2 x float> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x float>] %m, 1
    %m1v0 = extractelement <2 x float> %m1v, i32 0
    %m1v1 = extractelement <2 x float> %m1v, i32 1

    %d = call float @llpc.determinant2.f32(float %m0v0, float %m0v1, float %m1v0, float %m1v1)
    ret float %d
}

; GLSL: float = determinant(mat3)
define spir_func float @_Z11determinantDv3_Dv3_f(
    [3 x <3 x float>] %m) #0
{
    %m0v = extractvalue [3 x <3 x float>] %m, 0
    %m0v0 = extractelement <3 x float> %m0v, i32 0
    %m0v1 = extractelement <3 x float> %m0v, i32 1
    %m0v2 = extractelement <3 x float> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x float>] %m, 1
    %m1v0 = extractelement <3 x float> %m1v, i32 0
    %m1v1 = extractelement <3 x float> %m1v, i32 1
    %m1v2 = extractelement <3 x float> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x float>] %m, 2
    %m2v0 = extractelement <3 x float> %m2v, i32 0
    %m2v1 = extractelement <3 x float> %m2v, i32 1
    %m2v2 = extractelement <3 x float> %m2v, i32 2

    %d = call float @llpc.determinant3.f32(
        float %m0v0, float %m0v1, float %m0v2,
        float %m1v0, float %m1v1, float %m1v2,
        float %m2v0, float %m2v1, float %m2v2)
    ret float %d
}

; GLSL: float = determinant(mat4)
define spir_func float @_Z11determinantDv4_Dv4_f(
    [4 x <4 x float>] %m) #0
{
    %m0v = extractvalue [4 x <4 x float>] %m, 0
    %m0v0 = extractelement <4 x float> %m0v, i32 0
    %m0v1 = extractelement <4 x float> %m0v, i32 1
    %m0v2 = extractelement <4 x float> %m0v, i32 2
    %m0v3 = extractelement <4 x float> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x float>] %m, 1
    %m1v0 = extractelement <4 x float> %m1v, i32 0
    %m1v1 = extractelement <4 x float> %m1v, i32 1
    %m1v2 = extractelement <4 x float> %m1v, i32 2
    %m1v3 = extractelement <4 x float> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x float>] %m, 2
    %m2v0 = extractelement <4 x float> %m2v, i32 0
    %m2v1 = extractelement <4 x float> %m2v, i32 1
    %m2v2 = extractelement <4 x float> %m2v, i32 2
    %m2v3 = extractelement <4 x float> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x float>] %m, 3
    %m3v0 = extractelement <4 x float> %m3v, i32 0
    %m3v1 = extractelement <4 x float> %m3v, i32 1
    %m3v2 = extractelement <4 x float> %m3v, i32 2
    %m3v3 = extractelement <4 x float> %m3v, i32 3

    %d = call float @llpc.determinant4.f32(
        float %m0v0, float %m0v1, float %m0v2, float %m0v3,
        float %m1v0, float %m1v1, float %m1v2, float %m1v3,
        float %m2v0, float %m2v1, float %m2v2, float %m2v3,
        float %m3v0, float %m3v1, float %m3v2, float %m3v3)
    ret float %d
}

; GLSL helper: float = dot3(vec3(float, float, float), vec3(float, float, float))
define spir_func float @llpc.dot3.f32(
    float %x0, float %y0, float %z0,
    float %x1, float %y1, float %z1)
{
    %1 = fmul float %x1, %x0
    %2 = fmul float %y1, %y0
    %3 = fadd float %1, %2
    %4 = fmul float %z1, %z0
    %5 = fadd float %3, %4
    ret float %5
}

; GLSL helper: float = dot4(vec4(float, float, float, float), vec4(float, float, float, float))
define spir_func float @llpc.dot4.f32(
    float %x0, float %y0, float %z0, float %w0,
    float %x1, float %y1, float %z1, float %w1)
{
    %1 = fmul float %x1, %x0
    %2 = fmul float %y1, %y0
    %3 = fadd float %1, %2
    %4 = fmul float %z1, %z0
    %5 = fadd float %3, %4
    %6 = fmul float %w1, %w0
    %7 = fadd float %5, %6
    ret float %7
}

; GLSL: mat2 = inverse(mat2)
define spir_func [2 x <2 x float>] @_Z13matrixInverseDv2_Dv2_f(
    [2 x <2 x float>] %m) #0
{
    ; [ x0   x1 ]                    [  y1 -x1 ]
    ; [         ]  = (1 / det(M))) * [         ]
    ; [ y0   y1 ]                    [ -y0  x0 ]
    %m0v = extractvalue [2 x <2 x float>] %m, 0
    %x0 = extractelement <2 x float> %m0v, i32 0
    %y0 = extractelement <2 x float> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x float>] %m, 1
    %x1 = extractelement <2 x float> %m1v, i32 0
    %y1 = extractelement <2 x float> %m1v, i32 1

    %1 = call float @llpc.determinant2.f32(float %x0, float %y0, float %x1, float %y1)
    %2 = fdiv float 1.0, %1
    %3 = fsub float 0.0, %2
    %4 = fmul float %2, %y1
    %5 = fmul float %3, %y0
    %6 = fmul float %3, %x1
    %7 = fmul float %2, %x0
    %8 = insertelement <2 x float> undef, float %4, i32 0
    %9 = insertelement <2 x float> %8, float %5, i32 1
    %10 = insertvalue [2 x <2 x float>] undef, <2 x float> %9, 0
    %11 = insertelement <2 x float> undef, float %6, i32 0
    %12 = insertelement <2 x float> %11, float %7, i32 1
    %13 = insertvalue [2 x <2 x float>] %10 , <2 x float> %12, 1

    ret [2 x <2 x float>]  %13
}

; GLSL: mat3 = inverse(mat3)
define spir_func [3 x <3 x float>] @_Z13matrixInverseDv3_Dv3_f(
    [3 x <3 x float>] %m) #0
{
    ; [ x0   x1   x2 ]                   [ Adj(x0) Adj(x1) Adj(x2) ] T
    ; [              ]                   [                         ]
    ; [ y0   y1   y2 ]  = (1 / det(M)) * [ Adj(y0) Adj(y1) Adj(y2) ]
    ; [              ]                   [                         ]
    ; [ z0   z1   z2 ]                   [ Adj(z0) Adj(z1) Adj(z2) ]
    ;
    ;
    ;                     [ Adj(x0) Adj(y0) Adj(z0) ]
    ;                     [                         ]
    ;  = (1 / det(M)) *   [ Adj(x1) Adj(y1) Adj(y1) ]
    ;                     [                         ]
    ;                     [ Adj(x2) Adj(y2) Adj(z2) ]
    ;

    %m0v = extractvalue [3 x <3 x float>] %m, 0
    %x0 = extractelement <3 x float> %m0v, i32 0
    %y0 = extractelement <3 x float> %m0v, i32 1
    %z0 = extractelement <3 x float> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x float>] %m, 1
    %x1 = extractelement <3 x float> %m1v, i32 0
    %y1 = extractelement <3 x float> %m1v, i32 1
    %z1 = extractelement <3 x float> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x float>] %m, 2
    %x2 = extractelement <3 x float> %m2v, i32 0
    %y2 = extractelement <3 x float> %m2v, i32 1
    %z2 = extractelement <3 x float> %m2v, i32 2

    %adjx0 = call float @llpc.determinant2.f32(float %y1, float %z1, float %y2, float %z2)
    %adjx1 = call float @llpc.determinant2.f32(float %y2, float %z2, float %y0, float %z0)
    %adjx2 = call float @llpc.determinant2.f32(float %y0, float %z0, float %y1, float %z1)

    %det = call float @llpc.dot3.f32(float %x0, float %x1, float %x2,
                    float %adjx0, float %adjx1, float %adjx2)
    %rdet = fdiv float 1.0, %det

    %nx0 = fmul float %rdet, %adjx0
    %nx1 = fmul float %rdet, %adjx1
    %nx2 = fmul float %rdet, %adjx2

    %m00 = insertelement <3 x float> undef, float %nx0, i32 0
    %m01 = insertelement <3 x float> %m00, float %nx1, i32 1
    %m02 = insertelement <3 x float> %m01, float %nx2, i32 2
    %m0 = insertvalue [3 x <3 x float>] undef, <3 x float> %m02, 0

    %adjy0 = call float @llpc.determinant2.f32(float %z1, float %x1, float %z2, float %x2)
    %adjy1 = call float @llpc.determinant2.f32(float %z2, float %x2, float %z0, float %x0)
    %adjy2 = call float @llpc.determinant2.f32(float %z0, float %x0, float %z1, float %x1)


    %ny0 = fmul float %rdet, %adjy0
    %ny1 = fmul float %rdet, %adjy1
    %ny2 = fmul float %rdet, %adjy2

    %m10 = insertelement <3 x float> undef, float %ny0, i32 0
    %m11 = insertelement <3 x float> %m10, float %ny1, i32 1
    %m12 = insertelement <3 x float> %m11, float %ny2, i32 2
    %m1 = insertvalue [3 x <3 x float>] %m0, <3 x float> %m12, 1

    %adjz0 = call float @llpc.determinant2.f32(float %x1, float %y1, float %x2, float %y2)
    %adjz1 = call float @llpc.determinant2.f32(float %x2, float %y2, float %x0, float %y0)
    %adjz2 = call float @llpc.determinant2.f32(float %x0, float %y0, float %x1, float %y1)

    %nz0 = fmul float %rdet, %adjz0
    %nz1 = fmul float %rdet, %adjz1
    %nz2 = fmul float %rdet, %adjz2

    %m20 = insertelement <3 x float> undef, float %nz0, i32 0
    %m21 = insertelement <3 x float> %m20, float %nz1, i32 1
    %m22 = insertelement <3 x float> %m21, float %nz2, i32 2
    %m2 = insertvalue [3 x <3 x float>] %m1, <3 x float> %m22, 2

    ret [3 x <3 x float>] %m2
}

; GLSL: mat4 = inverse(mat4)
define spir_func [4 x <4 x float>] @_Z13matrixInverseDv4_Dv4_f(
    [4 x <4 x float>] %m) #0
{
    ; [ x0   x1   x2   x3 ]                   [ Adj(x0) Adj(x1) Adj(x2) Adj(x3) ] T
    ; [                   ]                   [                                 ]
    ; [ y0   y1   y2   y3 ]                   [ Adj(y0) Adj(y1) Adj(y2) Adj(y3) ]
    ; [                   ]  = (1 / det(M)) * [                                 ]
    ; [ z0   z1   z2   z3 ]                   [ Adj(z0) Adj(z1) Adj(z2) Adj(z3) ]
    ; [                   ]                   [                                 ]
    ; [ w0   w1   w2   w3 ]                   [ Adj(w0) Adj(w1) Adj(w2) Adj(w3) ]
    ;
    ;                  [ Adj(x0) Adj(y0) Adj(z0) Adj(w0) ]
    ;                  [                                 ]
    ;                  [ Adj(x1) Adj(y1) Adj(z2) Adj(w1) ]
    ; = (1 / det(M)) * [                                 ]
    ;                  [ Adj(x2) Adj(y2) Adj(z3) Adj(w2) ]
    ;                  [                                 ]
    ;                  [ Adj(x3) Adj(y3) Adj(z4) Adj(w3) ]

    %m0v = extractvalue [4 x <4 x float>] %m, 0
    %x0 = extractelement <4 x float> %m0v, i32 0
    %y0 = extractelement <4 x float> %m0v, i32 1
    %z0 = extractelement <4 x float> %m0v, i32 2
    %w0 = extractelement <4 x float> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x float>] %m, 1
    %x1 = extractelement <4 x float> %m1v, i32 0
    %y1 = extractelement <4 x float> %m1v, i32 1
    %z1 = extractelement <4 x float> %m1v, i32 2
    %w1 = extractelement <4 x float> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x float>] %m, 2
    %x2 = extractelement <4 x float> %m2v, i32 0
    %y2 = extractelement <4 x float> %m2v, i32 1
    %z2 = extractelement <4 x float> %m2v, i32 2
    %w2 = extractelement <4 x float> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x float>] %m, 3
    %x3 = extractelement <4 x float> %m3v, i32 0
    %y3 = extractelement <4 x float> %m3v, i32 1
    %z3 = extractelement <4 x float> %m3v, i32 2
    %w3 = extractelement <4 x float> %m3v, i32 3

    %adjx0 = call float @llpc.determinant3.f32(
            float %y1, float %z1, float %w1,
            float %y2, float %z2, float %w2,
            float %y3, float %z3, float %w3)
    %adjx1 = call float @llpc.determinant3.f32(
            float %y2, float %z2, float %w2,
            float %y0, float %z0, float %w0,
            float %y3, float %z3, float %w3)
    %adjx2 = call float @llpc.determinant3.f32(
            float %y3, float %z3, float %w3,
            float %y0, float %z0, float %w0,
            float %y1, float %z1, float %w1)
    %adjx3 = call float @llpc.determinant3.f32(
            float %y0, float %z0, float %w0,
            float %y2, float %z2, float %w2,
            float %y1, float %z1, float %w1)

    %det = call float @llpc.dot4.f32(float %x0, float %x1, float %x2, float %x3,
            float %adjx0, float %adjx1, float %adjx2, float %adjx3)
    %rdet = fdiv float 1.0, %det

    %nx0 = fmul float %rdet, %adjx0
    %nx1 = fmul float %rdet, %adjx1
    %nx2 = fmul float %rdet, %adjx2
    %nx3 = fmul float %rdet, %adjx3

    %m00 = insertelement <4 x float> undef, float %nx0, i32 0
    %m01 = insertelement <4 x float> %m00, float %nx1, i32 1
    %m02 = insertelement <4 x float> %m01, float %nx2, i32 2
    %m03 = insertelement <4 x float> %m02, float %nx3, i32 3
    %m0 = insertvalue [4 x <4 x float>] undef, <4 x float> %m03, 0

    %adjy0 = call float @llpc.determinant3.f32(
            float %z2, float %w2, float %x2,
            float %z1, float %w1, float %x1,
            float %z3, float %w3, float %x3)
    %adjy1 = call float @llpc.determinant3.f32(
             float %z2, float %w2, float %x2,
             float %z3, float %w3, float %x3,
             float %z0, float %w0, float %x0)
    %adjy2 = call float @llpc.determinant3.f32(
            float %z0, float %w0, float %x0,
            float %z3, float %w3, float %x3,
            float %z1, float %w1, float %x1)
    %adjy3 = call float @llpc.determinant3.f32(
            float %z0, float %w0, float %x0,
            float %z1, float %w1, float %x1,
            float %z2, float %w2, float %x2)

    %ny0 = fmul float %rdet, %adjy0
    %ny1 = fmul float %rdet, %adjy1
    %ny2 = fmul float %rdet, %adjy2
    %ny3 = fmul float %rdet, %adjy3

    %m10 = insertelement <4 x float> undef, float %ny0, i32 0
    %m11 = insertelement <4 x float> %m10, float %ny1, i32 1
    %m12 = insertelement <4 x float> %m11, float %ny2, i32 2
    %m13 = insertelement <4 x float> %m12, float %ny3, i32 3
    %m1 = insertvalue [4 x <4 x float>] %m0, <4 x float> %m13, 1

    %adjz0 = call float @llpc.determinant3.f32(
            float %w1, float %x1, float %y1,
            float %w2, float %x2, float %y2,
            float %w3, float %x3, float %y3)
    %adjz1 = call float @llpc.determinant3.f32(
            float %w3, float %x3, float %y3,
            float %w2, float %x2, float %y2,
            float %w0, float %x0, float %y0)
    %adjz2 = call float @llpc.determinant3.f32(
            float %w3, float %x3, float %y3,
            float %w0, float %x0, float %y0,
            float %w1, float %x1, float %y1)
    %adjz3 = call float @llpc.determinant3.f32(
            float %w1, float %x1, float %y1,
            float %w0, float %x0, float %y0,
            float %w2, float %x2, float %y2)

    %nz0 = fmul float %rdet, %adjz0
    %nz1 = fmul float %rdet, %adjz1
    %nz2 = fmul float %rdet, %adjz2
    %nz3 = fmul float %rdet, %adjz3

    %m20 = insertelement <4 x float> undef, float %nz0, i32 0
    %m21 = insertelement <4 x float> %m20, float %nz1, i32 1
    %m22 = insertelement <4 x float> %m21, float %nz2, i32 2
    %m23 = insertelement <4 x float> %m22, float %nz3, i32 3
    %m2 = insertvalue [4 x <4 x float>] %m1, <4 x float> %m23, 2

    %adjw0 = call float @llpc.determinant3.f32(
            float %x2, float %y2, float %z2,
            float %x1, float %y1, float %z1,
            float %x3, float %y3, float %z3)
    %adjw1 = call float @llpc.determinant3.f32(
            float %x2, float %y2, float %z2,
            float %x3, float %y3, float %z3,
            float %x0, float %y0, float %z0)
    %adjw2 = call float @llpc.determinant3.f32(
            float %x0, float %y0, float %z0,
            float %x3, float %y3, float %z3,
            float %x1, float %y1, float %z1)
    %adjw3 = call float @llpc.determinant3.f32(
            float %x0, float %y0, float %z0,
            float %x1, float %y1, float %z1,
            float %x2, float %y2, float %z2)

    %nw0 = fmul float %rdet, %adjw0
    %nw1 = fmul float %rdet, %adjw1
    %nw2 = fmul float %rdet, %adjw2
    %nw3 = fmul float %rdet, %adjw3

    %m30 = insertelement <4 x float> undef, float %nw0, i32 0
    %m31 = insertelement <4 x float> %m30, float %nw1, i32 1
    %m32 = insertelement <4 x float> %m31, float %nw2, i32 2
    %m33 = insertelement <4 x float> %m32, float %nw3, i32 3
    %m3 = insertvalue [4 x <4 x float>] %m2, <4 x float> %m33, 3

    ret [4 x <4 x float>] %m3
}

attributes #0 = { nounwind }
