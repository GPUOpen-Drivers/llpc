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

; GLSL: f16mat2 = transpose(f16mat2)
define spir_func [2 x <2 x half>] @_Z9TransposeDv2_Dv2_Dh(
    [2 x <2 x half>] %m) #0
{
    %nm = alloca [2 x <2 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <2 x half>], [2 x <2 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [2 x <2 x half>], [2 x <2 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm1, i32 0, i32 1

    %m0v = extractvalue [2 x <2 x half>] %m, 0
    %m0v0 = extractelement <2 x half> %m0v, i32 0
    %m0v1 = extractelement <2 x half> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x half>] %m, 1
    %m1v0 = extractelement <2 x half> %m1v, i32 0
    %m1v1 = extractelement <2 x half> %m1v, i32 1

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    %nmv = load [2 x <2 x half>], [2 x <2 x half>] addrspace(5)* %nm
    ret [2 x <2 x half>] %nmv
}

; GLSL: f16mat3 = transpose(f16mat3)
define spir_func [3 x <3 x half>] @_Z9TransposeDv3_Dv3_Dh(
    [3 x <3 x half>] %m) #0
{
    %nm = alloca [3 x <3 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <3 x half>], [3 x <3 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [3 x <3 x half>], [3 x <3 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 2

    %nm2 = getelementptr inbounds [3 x <3 x half>], [3 x <3 x half>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm2, i32 0, i32 2

    %m0v = extractvalue [3 x <3 x half>] %m, 0
    %m0v0 = extractelement <3 x half> %m0v, i32 0
    %m0v1 = extractelement <3 x half> %m0v, i32 1
    %m0v2 = extractelement <3 x half> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x half>] %m, 1
    %m1v0 = extractelement <3 x half> %m1v, i32 0
    %m1v1 = extractelement <3 x half> %m1v, i32 1
    %m1v2 = extractelement <3 x half> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x half>] %m, 2
    %m2v0 = extractelement <3 x half> %m2v, i32 0
    %m2v1 = extractelement <3 x half> %m2v, i32 1
    %m2v2 = extractelement <3 x half> %m2v, i32 2

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m2v0, half addrspace(5)* %nm02
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m2v1, half addrspace(5)* %nm12
    store half %m0v2, half addrspace(5)* %nm20
    store half %m1v2, half addrspace(5)* %nm21
    store half %m2v2, half addrspace(5)* %nm22
    %nmv = load [3 x <3 x half>], [3 x <3 x half>] addrspace(5)* %nm
    ret [3 x <3 x half>] %nmv
}

; GLSL: f16mat4 = transpose(f16mat4)
define spir_func [4 x <4 x half>] @_Z9TransposeDv4_Dv4_Dh(
    [4 x <4 x half>] %m) #0
{
    %nm = alloca [4 x <4 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <4 x half>], [4 x <4 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [4 x <4 x half>], [4 x <4 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 3

    %nm2 = getelementptr inbounds [4 x <4 x half>], [4 x <4 x half>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 2
    %nm23 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 3

    %nm3 = getelementptr inbounds [4 x <4 x half>], [4 x <4 x half>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm3, i32 0, i32 1
    %nm32 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm3, i32 0, i32 2
    %nm33 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm3, i32 0, i32 3

    %m0v = extractvalue [4 x <4 x half>] %m, 0
    %m0v0 = extractelement <4 x half> %m0v, i32 0
    %m0v1 = extractelement <4 x half> %m0v, i32 1
    %m0v2 = extractelement <4 x half> %m0v, i32 2
    %m0v3 = extractelement <4 x half> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x half>] %m, 1
    %m1v0 = extractelement <4 x half> %m1v, i32 0
    %m1v1 = extractelement <4 x half> %m1v, i32 1
    %m1v2 = extractelement <4 x half> %m1v, i32 2
    %m1v3 = extractelement <4 x half> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x half>] %m, 2
    %m2v0 = extractelement <4 x half> %m2v, i32 0
    %m2v1 = extractelement <4 x half> %m2v, i32 1
    %m2v2 = extractelement <4 x half> %m2v, i32 2
    %m2v3 = extractelement <4 x half> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x half>] %m, 3
    %m3v0 = extractelement <4 x half> %m3v, i32 0
    %m3v1 = extractelement <4 x half> %m3v, i32 1
    %m3v2 = extractelement <4 x half> %m3v, i32 2
    %m3v3 = extractelement <4 x half> %m3v, i32 3

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m2v0, half addrspace(5)* %nm02
    store half %m3v0, half addrspace(5)* %nm03
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m2v1, half addrspace(5)* %nm12
    store half %m3v1, half addrspace(5)* %nm13
    store half %m0v2, half addrspace(5)* %nm20
    store half %m1v2, half addrspace(5)* %nm21
    store half %m2v2, half addrspace(5)* %nm22
    store half %m3v2, half addrspace(5)* %nm23
    store half %m0v3, half addrspace(5)* %nm30
    store half %m1v3, half addrspace(5)* %nm31
    store half %m2v3, half addrspace(5)* %nm32
    store half %m3v3, half addrspace(5)* %nm33
    %nmv = load [4 x <4 x half>], [4 x <4 x half>] addrspace(5)* %nm
    ret [4 x <4 x half>] %nmv
}

; GLSL: f16mat2x3 = transpose(f16mat3x2)
define spir_func [2 x <3 x half>] @_Z9TransposeDv3_Dv2_Dh(
    [3 x <2 x half>] %m) #0
{
    %nm = alloca [2 x <3 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <3 x half>], [2 x <3 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [2 x <3 x half>], [2 x <3 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 2

    %m0v = extractvalue [3 x <2 x half>] %m, 0
    %m0v0 = extractelement <2 x half> %m0v, i32 0
    %m0v1 = extractelement <2 x half> %m0v, i32 1

    %m1v = extractvalue [3 x <2 x half>] %m, 1
    %m1v0 = extractelement <2 x half> %m1v, i32 0
    %m1v1 = extractelement <2 x half> %m1v, i32 1

    %m2v = extractvalue [3 x <2 x half>] %m, 2
    %m2v0 = extractelement <2 x half> %m2v, i32 0
    %m2v1 = extractelement <2 x half> %m2v, i32 1

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m2v0, half addrspace(5)* %nm02
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m2v1, half addrspace(5)* %nm12
    %nmv = load [2 x <3 x half>], [2 x <3 x half>] addrspace(5)* %nm
    ret [2 x <3 x half>] %nmv
}

; GLSL: f16mat3x2 = transpose(f16mat2x3)
define spir_func [3 x <2 x half>] @_Z9TransposeDv2_Dv3_Dh(
    [2 x <3 x half>] %m) #0
{
    %nm = alloca [3 x <2 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <2 x half>], [3 x <2 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [3 x <2 x half>], [3 x <2 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm1, i32 0, i32 1

    %nm2 = getelementptr inbounds [3 x <2 x half>], [3 x <2 x half>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm2, i32 0, i32 1

    %m0v = extractvalue [2 x <3 x half>] %m, 0
    %m0v0 = extractelement <3 x half> %m0v, i32 0
    %m0v1 = extractelement <3 x half> %m0v, i32 1
    %m0v2 = extractelement <3 x half> %m0v, i32 2

    %m1v = extractvalue [2 x <3 x half>] %m, 1
    %m1v0 = extractelement <3 x half> %m1v, i32 0
    %m1v1 = extractelement <3 x half> %m1v, i32 1
    %m1v2 = extractelement <3 x half> %m1v, i32 2

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m0v2, half addrspace(5)* %nm20
    store half %m1v2, half addrspace(5)* %nm21
    %nmv = load [3 x <2 x half>], [3 x <2 x half>] addrspace(5)* %nm
    ret [3 x <2 x half>] %nmv
}

; GLSL: f16mat2x4 = transpose(f16mat4x2)
define spir_func [2 x <4 x half>] @_Z9TransposeDv4_Dv2_Dh(
    [4 x <2 x half>] %m) #0
{
    %nm = alloca [2 x <4 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <4 x half>], [2 x <4 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [2 x <4 x half>], [2 x <4 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 3

    %m0v = extractvalue [4 x <2 x half>] %m, 0
    %m0v0 = extractelement <2 x half> %m0v, i32 0
    %m0v1 = extractelement <2 x half> %m0v, i32 1

    %m1v = extractvalue [4 x <2 x half>] %m, 1
    %m1v0 = extractelement <2 x half> %m1v, i32 0
    %m1v1 = extractelement <2 x half> %m1v, i32 1

    %m2v = extractvalue [4 x <2 x half>] %m, 2
    %m2v0 = extractelement <2 x half> %m2v, i32 0
    %m2v1 = extractelement <2 x half> %m2v, i32 1

    %m3v = extractvalue [4 x <2 x half>] %m, 3
    %m3v0 = extractelement <2 x half> %m3v, i32 0
    %m3v1 = extractelement <2 x half> %m3v, i32 1

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m2v0, half addrspace(5)* %nm02
    store half %m3v0, half addrspace(5)* %nm03
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m2v1, half addrspace(5)* %nm12
    store half %m3v1, half addrspace(5)* %nm13
    %nmv = load [2 x <4 x half>], [2 x <4 x half>] addrspace(5)* %nm
    ret [2 x <4 x half>] %nmv
}

; GLSL: f16mat4x2 = transpose(f16mat2x4)
define spir_func [4 x <2 x half>] @_Z9TransposeDv2_Dv4_Dh(
    [2 x <4 x half>] %m) #0
{
    %nm = alloca [4 x <2 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <2 x half>], [4 x <2 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [4 x <2 x half>], [4 x <2 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm1, i32 0, i32 1

    %nm2 = getelementptr inbounds [4 x <2 x half>], [4 x <2 x half>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm2, i32 0, i32 1

    %nm3 = getelementptr inbounds [4 x <2 x half>], [4 x <2 x half>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <2 x half>, <2 x half> addrspace(5)* %nm3, i32 0, i32 1

    %m0v = extractvalue [2 x <4 x half>] %m, 0
    %m0v0 = extractelement <4 x half> %m0v, i32 0
    %m0v1 = extractelement <4 x half> %m0v, i32 1
    %m0v2 = extractelement <4 x half> %m0v, i32 2
    %m0v3 = extractelement <4 x half> %m0v, i32 3

    %m1v = extractvalue [2 x <4 x half>] %m, 1
    %m1v0 = extractelement <4 x half> %m1v, i32 0
    %m1v1 = extractelement <4 x half> %m1v, i32 1
    %m1v2 = extractelement <4 x half> %m1v, i32 2
    %m1v3 = extractelement <4 x half> %m1v, i32 3

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m0v2, half addrspace(5)* %nm20
    store half %m1v2, half addrspace(5)* %nm21
    store half %m0v3, half addrspace(5)* %nm30
    store half %m1v3, half addrspace(5)* %nm31
    %nmv = load [4 x <2 x half>], [4 x <2 x half>] addrspace(5)* %nm
    ret [4 x <2 x half>] %nmv
}

; GLSL: f16mat3x4 = transpose(f16mat4x3)
define spir_func [3 x <4 x half>] @_Z9TransposeDv4_Dv3_Dh(
    [4 x <3 x half>] %m) #0
{
    %nm = alloca [3 x <4 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <4 x half>], [3 x <4 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [3 x <4 x half>], [3 x <4 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm1, i32 0, i32 3

    %nm2 = getelementptr inbounds [3 x <4 x half>], [3 x <4 x half>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 2
    %nm23 = getelementptr inbounds <4 x half>, <4 x half> addrspace(5)* %nm2, i32 0, i32 3

    %m0v = extractvalue [4 x <3 x half>] %m, 0
    %m0v0 = extractelement <3 x half> %m0v, i32 0
    %m0v1 = extractelement <3 x half> %m0v, i32 1
    %m0v2 = extractelement <3 x half> %m0v, i32 2

    %m1v = extractvalue [4 x <3 x half>] %m, 1
    %m1v0 = extractelement <3 x half> %m1v, i32 0
    %m1v1 = extractelement <3 x half> %m1v, i32 1
    %m1v2 = extractelement <3 x half> %m1v, i32 2

    %m2v = extractvalue [4 x <3 x half>] %m, 2
    %m2v0 = extractelement <3 x half> %m2v, i32 0
    %m2v1 = extractelement <3 x half> %m2v, i32 1
    %m2v2 = extractelement <3 x half> %m2v, i32 2

    %m3v = extractvalue [4 x <3 x half>] %m, 3
    %m3v0 = extractelement <3 x half> %m3v, i32 0
    %m3v1 = extractelement <3 x half> %m3v, i32 1
    %m3v2 = extractelement <3 x half> %m3v, i32 2

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m2v0, half addrspace(5)* %nm02
    store half %m3v0, half addrspace(5)* %nm03
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m2v1, half addrspace(5)* %nm12
    store half %m3v1, half addrspace(5)* %nm13
    store half %m0v2, half addrspace(5)* %nm20
    store half %m1v2, half addrspace(5)* %nm21
    store half %m2v2, half addrspace(5)* %nm22
    store half %m3v2, half addrspace(5)* %nm23
    %nmv = load [3 x <4 x half>], [3 x <4 x half>] addrspace(5)* %nm
    ret [3 x <4 x half>] %nmv
}

; GLSL: f16mat4x3 = transpose(f16mat3x4)
define spir_func [4 x <3 x half>] @_Z9TransposeDv3_Dv4_Dh(
    [3 x <4 x half>] %m) #0
{
    %nm = alloca [4 x <3 x half>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <3 x half>], [4 x <3 x half>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [4 x <3 x half>], [4 x <3 x half>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm1, i32 0, i32 2

    %nm2 = getelementptr inbounds [4 x <3 x half>], [4 x <3 x half>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm2, i32 0, i32 2

    %nm3 = getelementptr inbounds [4 x <3 x half>], [4 x <3 x half>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm3, i32 0, i32 1
    %nm32 = getelementptr inbounds <3 x half>, <3 x half> addrspace(5)* %nm3, i32 0, i32 2

    %m0v = extractvalue [3 x <4 x half>] %m, 0
    %m0v0 = extractelement <4 x half> %m0v, i32 0
    %m0v1 = extractelement <4 x half> %m0v, i32 1
    %m0v2 = extractelement <4 x half> %m0v, i32 2
    %m0v3 = extractelement <4 x half> %m0v, i32 3

    %m1v = extractvalue [3 x <4 x half>] %m, 1
    %m1v0 = extractelement <4 x half> %m1v, i32 0
    %m1v1 = extractelement <4 x half> %m1v, i32 1
    %m1v2 = extractelement <4 x half> %m1v, i32 2
    %m1v3 = extractelement <4 x half> %m1v, i32 3

    %m2v = extractvalue [3 x <4 x half>] %m, 2
    %m2v0 = extractelement <4 x half> %m2v, i32 0
    %m2v1 = extractelement <4 x half> %m2v, i32 1
    %m2v2 = extractelement <4 x half> %m2v, i32 2
    %m2v3 = extractelement <4 x half> %m2v, i32 3

    store half %m0v0, half addrspace(5)* %nm00
    store half %m1v0, half addrspace(5)* %nm01
    store half %m2v0, half addrspace(5)* %nm02
    store half %m0v1, half addrspace(5)* %nm10
    store half %m1v1, half addrspace(5)* %nm11
    store half %m2v1, half addrspace(5)* %nm12
    store half %m0v2, half addrspace(5)* %nm20
    store half %m1v2, half addrspace(5)* %nm21
    store half %m2v2, half addrspace(5)* %nm22
    store half %m0v3, half addrspace(5)* %nm30
    store half %m1v3, half addrspace(5)* %nm31
    store half %m2v3, half addrspace(5)* %nm32
    %nmv = load [4 x <3 x half>], [4 x <3 x half>] addrspace(5)* %nm
    ret [4 x <3 x half>] %nmv
}

; GLSL helper: float16_t = determinant2(f16vec2(float16_t, float16_t), f16vec2(float16_t, float16_t))
define spir_func half @llpc.determinant2(
    half %x0, half %y0, half %x1, half %y1)
{
    ; | x0   x1 |
    ; |         | = x0 * y1 - y0 * x1
    ; | y0   y1 |

    %1 = fmul half %x0, %y1
    %2 = fmul half %y0, %x1
    %3 = fsub half %1, %2
    ret half %3
}

; GLSL helper: float16_t = determinant3(f16vec3(float16_t, float16_t, float16_t), f16vec3(float16_t, float16_t, float16_t))
define spir_func half @llpc.determinant3.f16(
    half %x0, half %y0, half %z0,
    half %x1, half %y1, half %z1,
    half %x2, half %y2, half %z2)
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

    %1 = call half @llpc.determinant2(half %y1, half %z1, half %y2, half %z2)
    %2 = fmul half %1, %x0
    %3 = call half @llpc.determinant2(half %z1, half %x1, half %z2, half %x2)
    %4 = fmul half %3, %y0
    %5 = fadd half %2, %4
    %6 = call half @llpc.determinant2(half %x1, half %y1, half %x2, half %y2)
    %7 = fmul half %6, %z0
    %8 = fadd half %7, %5
    ret half %8
}

; GLSL helper: float16_t = determinant4(f16vec4(float16_t, float16_t, float16_t, float16_t), f16vec4(float16_t, float16_t, float16_t, float16_t))
define spir_func half @llpc.determinant4.f16(
    half %x0, half %y0, half %z0, half %w0,
    half %x1, half %y1, half %z1, half %w1,
    half %x2, half %y2, half %z2, half %w2,
    half %x3, half %y3, half %z3, half %w3)

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

    %1 = call half @llpc.determinant3.f16(half %y1, half %z1, half %w1, half %y2, half %z2, half %w2, half %y3, half %z3, half %w3)
    %2 = fmul half %1, %x0
    %3 = call half @llpc.determinant3.f16(half %z1, half %x1, half %w1, half %z2, half %x2, half %w2, half %z3, half %x3, half %w3)
    %4 = fmul half %3, %y0
    %5 = fadd half %2, %4
    %6 = call half @llpc.determinant3.f16(half %x1, half %y1, half %w1, half %x2, half %y2, half %w2, half %x3, half %y3, half %w3)
    %7 = fmul half %6, %z0
    %8 = fadd half %5, %7
    %9 = call half @llpc.determinant3.f16(half %y1, half %x1, half %z1, half %y2, half %x2, half %z2, half %y3, half %x3, half %z3)
    %10 = fmul half %9, %w0
    %11 = fadd half %8, %10
    ret half %11
}

; GLSL: half = determinant(f16mat2)
define spir_func half @_Z11determinantDv2_Dv2_Dh(
    [2 x <2 x half>] %m) #0
{
    %m0v = extractvalue [2 x <2 x half>] %m, 0
    %m0v0 = extractelement <2 x half> %m0v, i32 0
    %m0v1 = extractelement <2 x half> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x half>] %m, 1
    %m1v0 = extractelement <2 x half> %m1v, i32 0
    %m1v1 = extractelement <2 x half> %m1v, i32 1

    %d = call half @llpc.determinant2(half %m0v0, half %m0v1, half %m1v0, half %m1v1)
    ret half %d
}

; GLSL: half = determinant(f16mat3)
define spir_func half @_Z11determinantDv3_Dv3_Dh(
    [3 x <3 x half>] %m) #0
{
    %m0v = extractvalue [3 x <3 x half>] %m, 0
    %m0v0 = extractelement <3 x half> %m0v, i32 0
    %m0v1 = extractelement <3 x half> %m0v, i32 1
    %m0v2 = extractelement <3 x half> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x half>] %m, 1
    %m1v0 = extractelement <3 x half> %m1v, i32 0
    %m1v1 = extractelement <3 x half> %m1v, i32 1
    %m1v2 = extractelement <3 x half> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x half>] %m, 2
    %m2v0 = extractelement <3 x half> %m2v, i32 0
    %m2v1 = extractelement <3 x half> %m2v, i32 1
    %m2v2 = extractelement <3 x half> %m2v, i32 2

    %d = call half @llpc.determinant3.f16(
        half %m0v0, half %m0v1, half %m0v2,
        half %m1v0, half %m1v1, half %m1v2,
        half %m2v0, half %m2v1, half %m2v2)
    ret half %d
}

; GLSL: half = determinant(f16mat4)
define spir_func half @_Z11determinantDv4_Dv4_Dh(
    [4 x <4 x half>] %m) #0
{
    %m0v = extractvalue [4 x <4 x half>] %m, 0
    %m0v0 = extractelement <4 x half> %m0v, i32 0
    %m0v1 = extractelement <4 x half> %m0v, i32 1
    %m0v2 = extractelement <4 x half> %m0v, i32 2
    %m0v3 = extractelement <4 x half> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x half>] %m, 1
    %m1v0 = extractelement <4 x half> %m1v, i32 0
    %m1v1 = extractelement <4 x half> %m1v, i32 1
    %m1v2 = extractelement <4 x half> %m1v, i32 2
    %m1v3 = extractelement <4 x half> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x half>] %m, 2
    %m2v0 = extractelement <4 x half> %m2v, i32 0
    %m2v1 = extractelement <4 x half> %m2v, i32 1
    %m2v2 = extractelement <4 x half> %m2v, i32 2
    %m2v3 = extractelement <4 x half> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x half>] %m, 3
    %m3v0 = extractelement <4 x half> %m3v, i32 0
    %m3v1 = extractelement <4 x half> %m3v, i32 1
    %m3v2 = extractelement <4 x half> %m3v, i32 2
    %m3v3 = extractelement <4 x half> %m3v, i32 3

    %d = call half @llpc.determinant4.f16(
        half %m0v0, half %m0v1, half %m0v2, half %m0v3,
        half %m1v0, half %m1v1, half %m1v2, half %m1v3,
        half %m2v0, half %m2v1, half %m2v2, half %m2v3,
        half %m3v0, half %m3v1, half %m3v2, half %m3v3)
    ret half %d
}

; GLSL helper: float16_t = dot3(f16vec3(float16_t, float16_t, float16_t), f16vec3(float16_t, float16_t, float16_t))
define spir_func half @llpc.dot3.f16(
    half %x0, half %y0, half %z0,
    half %x1, half %y1, half %z1)
{
    %1 = fmul half %x1, %x0
    %2 = fmul half %y1, %y0
    %3 = fadd half %1, %2
    %4 = fmul half %z1, %z0
    %5 = fadd half %3, %4
    ret half %5
}

; GLSL helper: float16_t = dot4(f16vec4(float16_t, float16_t, float16_t, float16_t), f16vec4(float16_t, float16_t, float16_t, float16_t))
define spir_func half @llpc.dot4.f16(
    half %x0, half %y0, half %z0, half %w0,
    half %x1, half %y1, half %z1, half %w1)
{
    %1 = fmul half %x1, %x0
    %2 = fmul half %y1, %y0
    %3 = fadd half %1, %2
    %4 = fmul half %z1, %z0
    %5 = fadd half %3, %4
    %6 = fmul half %w1, %w0
    %7 = fadd half %5, %6
    ret half %7
}

; GLSL: f16mat2 = inverse(f16mat2)
define spir_func [2 x <2 x half>] @_Z13matrixInverseDv2_Dv2_Dh(
    [2 x <2 x half>] %m) #0
{
    ; [ x0   x1 ]                    [  y1 -x1 ]
    ; [         ]  = (1 / det(M))) * [         ]
    ; [ y0   y1 ]                    [ -y0  x0 ]
    %m0v = extractvalue [2 x <2 x half>] %m, 0
    %x0 = extractelement <2 x half> %m0v, i32 0
    %y0 = extractelement <2 x half> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x half>] %m, 1
    %x1 = extractelement <2 x half> %m1v, i32 0
    %y1 = extractelement <2 x half> %m1v, i32 1

    %1 = call half @llpc.determinant2(half %x0, half %y0, half %x1, half %y1)
    %2 = fdiv half 1.0, %1
    %3 = fsub half 0.0, %2
    %4 = fmul half %2, %y1
    %5 = fmul half %3, %y0
    %6 = fmul half %3, %x1
    %7 = fmul half %2, %x0
    %8 = insertelement <2 x half> undef, half %4, i32 0
    %9 = insertelement <2 x half> %8, half %5, i32 1
    %10 = insertvalue [2 x <2 x half>] undef, <2 x half> %9, 0
    %11 = insertelement <2 x half> undef, half %6, i32 0
    %12 = insertelement <2 x half> %11, half %7, i32 1
    %13 = insertvalue [2 x <2 x half>] %10 , <2 x half> %12, 1

    ret [2 x <2 x half>]  %13
}

; GLSL: f16mat3 = inverse(f16mat3)
define spir_func [3 x <3 x half>] @_Z13matrixInverseDv3_Dv3_Dh(
    [3 x <3 x half>] %m) #0
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

    %m0v = extractvalue [3 x <3 x half>] %m, 0
    %x0 = extractelement <3 x half> %m0v, i32 0
    %y0 = extractelement <3 x half> %m0v, i32 1
    %z0 = extractelement <3 x half> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x half>] %m, 1
    %x1 = extractelement <3 x half> %m1v, i32 0
    %y1 = extractelement <3 x half> %m1v, i32 1
    %z1 = extractelement <3 x half> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x half>] %m, 2
    %x2 = extractelement <3 x half> %m2v, i32 0
    %y2 = extractelement <3 x half> %m2v, i32 1
    %z2 = extractelement <3 x half> %m2v, i32 2

    %adjx0 = call half @llpc.determinant2(half %y1, half %z1, half %y2, half %z2)
    %adjx1 = call half @llpc.determinant2(half %y2, half %z2, half %y0, half %z0)
    %adjx2 = call half @llpc.determinant2(half %y0, half %z0, half %y1, half %z1)

    %det = call half @llpc.dot3.f16(half %x0, half %x1, half %x2,
                    half %adjx0, half %adjx1, half %adjx2)
    %rdet = fdiv half 1.0, %det

    %nx0 = fmul half %rdet, %adjx0
    %nx1 = fmul half %rdet, %adjx1
    %nx2 = fmul half %rdet, %adjx2

    %m00 = insertelement <3 x half> undef, half %nx0, i32 0
    %m01 = insertelement <3 x half> %m00, half %nx1, i32 1
    %m02 = insertelement <3 x half> %m01, half %nx2, i32 2
    %m0 = insertvalue [3 x <3 x half>] undef, <3 x half> %m02, 0

    %adjy0 = call half @llpc.determinant2(half %z1, half %x1, half %z2, half %x2)
    %adjy1 = call half @llpc.determinant2(half %z2, half %x2, half %z0, half %x0)
    %adjy2 = call half @llpc.determinant2(half %z0, half %x0, half %z1, half %x1)


    %ny0 = fmul half %rdet, %adjy0
    %ny1 = fmul half %rdet, %adjy1
    %ny2 = fmul half %rdet, %adjy2

    %m10 = insertelement <3 x half> undef, half %ny0, i32 0
    %m11 = insertelement <3 x half> %m10, half %ny1, i32 1
    %m12 = insertelement <3 x half> %m11, half %ny2, i32 2
    %m1 = insertvalue [3 x <3 x half>] %m0, <3 x half> %m12, 1

    %adjz0 = call half @llpc.determinant2(half %x1, half %y1, half %x2, half %y2)
    %adjz1 = call half @llpc.determinant2(half %x2, half %y2, half %x0, half %y0)
    %adjz2 = call half @llpc.determinant2(half %x0, half %y0, half %x1, half %y1)

    %nz0 = fmul half %rdet, %adjz0
    %nz1 = fmul half %rdet, %adjz1
    %nz2 = fmul half %rdet, %adjz2

    %m20 = insertelement <3 x half> undef, half %nz0, i32 0
    %m21 = insertelement <3 x half> %m20, half %nz1, i32 1
    %m22 = insertelement <3 x half> %m21, half %nz2, i32 2
    %m2 = insertvalue [3 x <3 x half>] %m1, <3 x half> %m22, 2

    ret [3 x <3 x half>] %m2
}

; GLSL: f16mat4 = inverse(f16mat4)
define spir_func [4 x <4 x half>] @_Z13matrixInverseDv4_Dv4_Dh(
    [4 x <4 x half>] %m) #0
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

    %m0v = extractvalue [4 x <4 x half>] %m, 0
    %x0 = extractelement <4 x half> %m0v, i32 0
    %y0 = extractelement <4 x half> %m0v, i32 1
    %z0 = extractelement <4 x half> %m0v, i32 2
    %w0 = extractelement <4 x half> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x half>] %m, 1
    %x1 = extractelement <4 x half> %m1v, i32 0
    %y1 = extractelement <4 x half> %m1v, i32 1
    %z1 = extractelement <4 x half> %m1v, i32 2
    %w1 = extractelement <4 x half> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x half>] %m, 2
    %x2 = extractelement <4 x half> %m2v, i32 0
    %y2 = extractelement <4 x half> %m2v, i32 1
    %z2 = extractelement <4 x half> %m2v, i32 2
    %w2 = extractelement <4 x half> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x half>] %m, 3
    %x3 = extractelement <4 x half> %m3v, i32 0
    %y3 = extractelement <4 x half> %m3v, i32 1
    %z3 = extractelement <4 x half> %m3v, i32 2
    %w3 = extractelement <4 x half> %m3v, i32 3

    %adjx0 = call half @llpc.determinant3.f16(
            half %y1, half %z1, half %w1,
            half %y2, half %z2, half %w2,
            half %y3, half %z3, half %w3)
    %adjx1 = call half @llpc.determinant3.f16(
            half %y2, half %z2, half %w2,
            half %y0, half %z0, half %w0,
            half %y3, half %z3, half %w3)
    %adjx2 = call half @llpc.determinant3.f16(
            half %y3, half %z3, half %w3,
            half %y0, half %z0, half %w0,
            half %y1, half %z1, half %w1)
    %adjx3 = call half @llpc.determinant3.f16(
            half %y0, half %z0, half %w0,
            half %y2, half %z2, half %w2,
            half %y1, half %z1, half %w1)

    %det = call half @llpc.dot4.f16(half %x0, half %x1, half %x2, half %x3,
            half %adjx0, half %adjx1, half %adjx2, half %adjx3)
    %rdet = fdiv half 1.0, %det

    %nx0 = fmul half %rdet, %adjx0
    %nx1 = fmul half %rdet, %adjx1
    %nx2 = fmul half %rdet, %adjx2
    %nx3 = fmul half %rdet, %adjx3

    %m00 = insertelement <4 x half> undef, half %nx0, i32 0
    %m01 = insertelement <4 x half> %m00, half %nx1, i32 1
    %m02 = insertelement <4 x half> %m01, half %nx2, i32 2
    %m03 = insertelement <4 x half> %m02, half %nx3, i32 3
    %m0 = insertvalue [4 x <4 x half>] undef, <4 x half> %m03, 0

    %adjy0 = call half @llpc.determinant3.f16(
            half %z2, half %w2, half %x2,
            half %z1, half %w1, half %x1,
            half %z3, half %w3, half %x3)
    %adjy1 = call half @llpc.determinant3.f16(
             half %z2, half %w2, half %x2,
             half %z3, half %w3, half %x3,
             half %z0, half %w0, half %x0)
    %adjy2 = call half @llpc.determinant3.f16(
            half %z0, half %w0, half %x0,
            half %z3, half %w3, half %x3,
            half %z1, half %w1, half %x1)
    %adjy3 = call half @llpc.determinant3.f16(
            half %z0, half %w0, half %x0,
            half %z1, half %w1, half %x1,
            half %z2, half %w2, half %x2)

    %ny0 = fmul half %rdet, %adjy0
    %ny1 = fmul half %rdet, %adjy1
    %ny2 = fmul half %rdet, %adjy2
    %ny3 = fmul half %rdet, %adjy3

    %m10 = insertelement <4 x half> undef, half %ny0, i32 0
    %m11 = insertelement <4 x half> %m10, half %ny1, i32 1
    %m12 = insertelement <4 x half> %m11, half %ny2, i32 2
    %m13 = insertelement <4 x half> %m12, half %ny3, i32 3
    %m1 = insertvalue [4 x <4 x half>] %m0, <4 x half> %m13, 1

    %adjz0 = call half @llpc.determinant3.f16(
            half %w1, half %x1, half %y1,
            half %w2, half %x2, half %y2,
            half %w3, half %x3, half %y3)
    %adjz1 = call half @llpc.determinant3.f16(
            half %w3, half %x3, half %y3,
            half %w2, half %x2, half %y2,
            half %w0, half %x0, half %y0)
    %adjz2 = call half @llpc.determinant3.f16(
            half %w3, half %x3, half %y3,
            half %w0, half %x0, half %y0,
            half %w1, half %x1, half %y1)
    %adjz3 = call half @llpc.determinant3.f16(
            half %w1, half %x1, half %y1,
            half %w0, half %x0, half %y0,
            half %w2, half %x2, half %y2)

    %nz0 = fmul half %rdet, %adjz0
    %nz1 = fmul half %rdet, %adjz1
    %nz2 = fmul half %rdet, %adjz2
    %nz3 = fmul half %rdet, %adjz3

    %m20 = insertelement <4 x half> undef, half %nz0, i32 0
    %m21 = insertelement <4 x half> %m20, half %nz1, i32 1
    %m22 = insertelement <4 x half> %m21, half %nz2, i32 2
    %m23 = insertelement <4 x half> %m22, half %nz3, i32 3
    %m2 = insertvalue [4 x <4 x half>] %m1, <4 x half> %m23, 2

    %adjw0 = call half @llpc.determinant3.f16(
            half %x2, half %y2, half %z2,
            half %x1, half %y1, half %z1,
            half %x3, half %y3, half %z3)
    %adjw1 = call half @llpc.determinant3.f16(
            half %x2, half %y2, half %z2,
            half %x3, half %y3, half %z3,
            half %x0, half %y0, half %z0)
    %adjw2 = call half @llpc.determinant3.f16(
            half %x0, half %y0, half %z0,
            half %x3, half %y3, half %z3,
            half %x1, half %y1, half %z1)
    %adjw3 = call half @llpc.determinant3.f16(
            half %x0, half %y0, half %z0,
            half %x1, half %y1, half %z1,
            half %x2, half %y2, half %z2)

    %nw0 = fmul half %rdet, %adjw0
    %nw1 = fmul half %rdet, %adjw1
    %nw2 = fmul half %rdet, %adjw2
    %nw3 = fmul half %rdet, %adjw3

    %m30 = insertelement <4 x half> undef, half %nw0, i32 0
    %m31 = insertelement <4 x half> %m30, half %nw1, i32 1
    %m32 = insertelement <4 x half> %m31, half %nw2, i32 2
    %m33 = insertelement <4 x half> %m32, half %nw3, i32 3
    %m3 = insertvalue [4 x <4 x half>] %m2, <4 x half> %m33, 3

    ret [4 x <4 x half>] %m3
}

attributes #0 = { nounwind }
