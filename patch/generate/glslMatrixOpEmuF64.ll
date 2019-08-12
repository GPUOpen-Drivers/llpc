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

; GLSL: dmat2 = transpose(dmat2)
define spir_func [2 x <2 x double>] @_Z9TransposeDv2_Dv2_d(
    [2 x <2 x double>] %m) #0
{
    %nm = alloca [2 x <2 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <2 x double>], [2 x <2 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [2 x <2 x double>], [2 x <2 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm1, i32 0, i32 1

    %m0v = extractvalue [2 x <2 x double>] %m, 0
    %m0v0 = extractelement <2 x double> %m0v, i32 0
    %m0v1 = extractelement <2 x double> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x double>] %m, 1
    %m1v0 = extractelement <2 x double> %m1v, i32 0
    %m1v1 = extractelement <2 x double> %m1v, i32 1

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    %nmv = load [2 x <2 x double>], [2 x <2 x double>] addrspace(5)* %nm
    ret [2 x <2 x double>] %nmv
}

; GLSL: dmat3 = transpose(dmat3)
define spir_func [3 x <3 x double>] @_Z9TransposeDv3_Dv3_d(
    [3 x <3 x double>] %m) #0
{
    %nm = alloca [3 x <3 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <3 x double>], [3 x <3 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [3 x <3 x double>], [3 x <3 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 2

    %nm2 = getelementptr inbounds [3 x <3 x double>], [3 x <3 x double>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm2, i32 0, i32 2

    %m0v = extractvalue [3 x <3 x double>] %m, 0
    %m0v0 = extractelement <3 x double> %m0v, i32 0
    %m0v1 = extractelement <3 x double> %m0v, i32 1
    %m0v2 = extractelement <3 x double> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x double>] %m, 1
    %m1v0 = extractelement <3 x double> %m1v, i32 0
    %m1v1 = extractelement <3 x double> %m1v, i32 1
    %m1v2 = extractelement <3 x double> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x double>] %m, 2
    %m2v0 = extractelement <3 x double> %m2v, i32 0
    %m2v1 = extractelement <3 x double> %m2v, i32 1
    %m2v2 = extractelement <3 x double> %m2v, i32 2

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m2v0, double addrspace(5)* %nm02
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m2v1, double addrspace(5)* %nm12
    store double %m0v2, double addrspace(5)* %nm20
    store double %m1v2, double addrspace(5)* %nm21
    store double %m2v2, double addrspace(5)* %nm22
    %nmv = load [3 x <3 x double>], [3 x <3 x double>] addrspace(5)* %nm
    ret [3 x <3 x double>] %nmv
}

; GLSL: dmat4 = transpose(dmat4)
define spir_func [4 x <4 x double>] @_Z9TransposeDv4_Dv4_d(
    [4 x <4 x double>] %m) #0
{
    %nm = alloca [4 x <4 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <4 x double>], [4 x <4 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [4 x <4 x double>], [4 x <4 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 3

    %nm2 = getelementptr inbounds [4 x <4 x double>], [4 x <4 x double>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 2
    %nm23 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 3

    %nm3 = getelementptr inbounds [4 x <4 x double>], [4 x <4 x double>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm3, i32 0, i32 1
    %nm32 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm3, i32 0, i32 2
    %nm33 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm3, i32 0, i32 3

    %m0v = extractvalue [4 x <4 x double>] %m, 0
    %m0v0 = extractelement <4 x double> %m0v, i32 0
    %m0v1 = extractelement <4 x double> %m0v, i32 1
    %m0v2 = extractelement <4 x double> %m0v, i32 2
    %m0v3 = extractelement <4 x double> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x double>] %m, 1
    %m1v0 = extractelement <4 x double> %m1v, i32 0
    %m1v1 = extractelement <4 x double> %m1v, i32 1
    %m1v2 = extractelement <4 x double> %m1v, i32 2
    %m1v3 = extractelement <4 x double> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x double>] %m, 2
    %m2v0 = extractelement <4 x double> %m2v, i32 0
    %m2v1 = extractelement <4 x double> %m2v, i32 1
    %m2v2 = extractelement <4 x double> %m2v, i32 2
    %m2v3 = extractelement <4 x double> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x double>] %m, 3
    %m3v0 = extractelement <4 x double> %m3v, i32 0
    %m3v1 = extractelement <4 x double> %m3v, i32 1
    %m3v2 = extractelement <4 x double> %m3v, i32 2
    %m3v3 = extractelement <4 x double> %m3v, i32 3

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m2v0, double addrspace(5)* %nm02
    store double %m3v0, double addrspace(5)* %nm03
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m2v1, double addrspace(5)* %nm12
    store double %m3v1, double addrspace(5)* %nm13
    store double %m0v2, double addrspace(5)* %nm20
    store double %m1v2, double addrspace(5)* %nm21
    store double %m2v2, double addrspace(5)* %nm22
    store double %m3v2, double addrspace(5)* %nm23
    store double %m0v3, double addrspace(5)* %nm30
    store double %m1v3, double addrspace(5)* %nm31
    store double %m2v3, double addrspace(5)* %nm32
    store double %m3v3, double addrspace(5)* %nm33
    %nmv = load [4 x <4 x double>], [4 x <4 x double>] addrspace(5)* %nm
    ret [4 x <4 x double>] %nmv
}

; GLSL: dmat2x3 = transpose(dmat3x2)
define spir_func [2 x <3 x double>] @_Z9TransposeDv3_Dv2_d(
    [3 x <2 x double>] %m) #0
{
    %nm = alloca [2 x <3 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <3 x double>], [2 x <3 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [2 x <3 x double>], [2 x <3 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 2

    %m0v = extractvalue [3 x <2 x double>] %m, 0
    %m0v0 = extractelement <2 x double> %m0v, i32 0
    %m0v1 = extractelement <2 x double> %m0v, i32 1

    %m1v = extractvalue [3 x <2 x double>] %m, 1
    %m1v0 = extractelement <2 x double> %m1v, i32 0
    %m1v1 = extractelement <2 x double> %m1v, i32 1

    %m2v = extractvalue [3 x <2 x double>] %m, 2
    %m2v0 = extractelement <2 x double> %m2v, i32 0
    %m2v1 = extractelement <2 x double> %m2v, i32 1

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m2v0, double addrspace(5)* %nm02
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m2v1, double addrspace(5)* %nm12
    %nmv = load [2 x <3 x double>], [2 x <3 x double>] addrspace(5)* %nm
    ret [2 x <3 x double>] %nmv
}

; GLSL: dmat3x2 = transpose(dmat2x3)
define spir_func [3 x <2 x double>] @_Z9TransposeDv2_Dv3_d(
    [2 x <3 x double>] %m) #0
{
    %nm = alloca [3 x <2 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <2 x double>], [3 x <2 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [3 x <2 x double>], [3 x <2 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm1, i32 0, i32 1

    %nm2 = getelementptr inbounds [3 x <2 x double>], [3 x <2 x double>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm2, i32 0, i32 1

    %m0v = extractvalue [2 x <3 x double>] %m, 0
    %m0v0 = extractelement <3 x double> %m0v, i32 0
    %m0v1 = extractelement <3 x double> %m0v, i32 1
    %m0v2 = extractelement <3 x double> %m0v, i32 2

    %m1v = extractvalue [2 x <3 x double>] %m, 1
    %m1v0 = extractelement <3 x double> %m1v, i32 0
    %m1v1 = extractelement <3 x double> %m1v, i32 1
    %m1v2 = extractelement <3 x double> %m1v, i32 2

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m0v2, double addrspace(5)* %nm20
    store double %m1v2, double addrspace(5)* %nm21
    %nmv = load [3 x <2 x double>], [3 x <2 x double>] addrspace(5)* %nm
    ret [3 x <2 x double>] %nmv
}

; GLSL: dmat2x4 = transpose(dmat4x2)
define spir_func [2 x <4 x double>] @_Z9TransposeDv4_Dv2_d(
    [4 x <2 x double>] %m) #0
{
    %nm = alloca [2 x <4 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [2 x <4 x double>], [2 x <4 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [2 x <4 x double>], [2 x <4 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 3

    %m0v = extractvalue [4 x <2 x double>] %m, 0
    %m0v0 = extractelement <2 x double> %m0v, i32 0
    %m0v1 = extractelement <2 x double> %m0v, i32 1

    %m1v = extractvalue [4 x <2 x double>] %m, 1
    %m1v0 = extractelement <2 x double> %m1v, i32 0
    %m1v1 = extractelement <2 x double> %m1v, i32 1

    %m2v = extractvalue [4 x <2 x double>] %m, 2
    %m2v0 = extractelement <2 x double> %m2v, i32 0
    %m2v1 = extractelement <2 x double> %m2v, i32 1

    %m3v = extractvalue [4 x <2 x double>] %m, 3
    %m3v0 = extractelement <2 x double> %m3v, i32 0
    %m3v1 = extractelement <2 x double> %m3v, i32 1

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m2v0, double addrspace(5)* %nm02
    store double %m3v0, double addrspace(5)* %nm03
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m2v1, double addrspace(5)* %nm12
    store double %m3v1, double addrspace(5)* %nm13
    %nmv = load [2 x <4 x double>], [2 x <4 x double>] addrspace(5)* %nm
    ret [2 x <4 x double>] %nmv
}

; GLSL: dmat4x2 = transpose(dmat2x4)
define spir_func [4 x <2 x double>] @_Z9TransposeDv2_Dv4_d(
    [2 x <4 x double>] %m) #0
{
    %nm = alloca [4 x <2 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <2 x double>], [4 x <2 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm0, i32 0, i32 1

    %nm1 = getelementptr inbounds [4 x <2 x double>], [4 x <2 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm1, i32 0, i32 1

    %nm2 = getelementptr inbounds [4 x <2 x double>], [4 x <2 x double>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm2, i32 0, i32 1

    %nm3 = getelementptr inbounds [4 x <2 x double>], [4 x <2 x double>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <2 x double>, <2 x double> addrspace(5)* %nm3, i32 0, i32 1

    %m0v = extractvalue [2 x <4 x double>] %m, 0
    %m0v0 = extractelement <4 x double> %m0v, i32 0
    %m0v1 = extractelement <4 x double> %m0v, i32 1
    %m0v2 = extractelement <4 x double> %m0v, i32 2
    %m0v3 = extractelement <4 x double> %m0v, i32 3

    %m1v = extractvalue [2 x <4 x double>] %m, 1
    %m1v0 = extractelement <4 x double> %m1v, i32 0
    %m1v1 = extractelement <4 x double> %m1v, i32 1
    %m1v2 = extractelement <4 x double> %m1v, i32 2
    %m1v3 = extractelement <4 x double> %m1v, i32 3

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m0v2, double addrspace(5)* %nm20
    store double %m1v2, double addrspace(5)* %nm21
    store double %m0v3, double addrspace(5)* %nm30
    store double %m1v3, double addrspace(5)* %nm31
    %nmv = load [4 x <2 x double>], [4 x <2 x double>] addrspace(5)* %nm
    ret [4 x <2 x double>] %nmv
}

; GLSL: dmat3x4 = transpose(dmat4x3)
define spir_func [3 x <4 x double>] @_Z9TransposeDv4_Dv3_d(
    [4 x <3 x double>] %m) #0
{
    %nm = alloca [3 x <4 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [3 x <4 x double>], [3 x <4 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 2
    %nm03 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm0, i32 0, i32 3

    %nm1 = getelementptr inbounds [3 x <4 x double>], [3 x <4 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 2
    %nm13 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm1, i32 0, i32 3

    %nm2 = getelementptr inbounds [3 x <4 x double>], [3 x <4 x double>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 2
    %nm23 = getelementptr inbounds <4 x double>, <4 x double> addrspace(5)* %nm2, i32 0, i32 3

    %m0v = extractvalue [4 x <3 x double>] %m, 0
    %m0v0 = extractelement <3 x double> %m0v, i32 0
    %m0v1 = extractelement <3 x double> %m0v, i32 1
    %m0v2 = extractelement <3 x double> %m0v, i32 2

    %m1v = extractvalue [4 x <3 x double>] %m, 1
    %m1v0 = extractelement <3 x double> %m1v, i32 0
    %m1v1 = extractelement <3 x double> %m1v, i32 1
    %m1v2 = extractelement <3 x double> %m1v, i32 2

    %m2v = extractvalue [4 x <3 x double>] %m, 2
    %m2v0 = extractelement <3 x double> %m2v, i32 0
    %m2v1 = extractelement <3 x double> %m2v, i32 1
    %m2v2 = extractelement <3 x double> %m2v, i32 2

    %m3v = extractvalue [4 x <3 x double>] %m, 3
    %m3v0 = extractelement <3 x double> %m3v, i32 0
    %m3v1 = extractelement <3 x double> %m3v, i32 1
    %m3v2 = extractelement <3 x double> %m3v, i32 2

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m2v0, double addrspace(5)* %nm02
    store double %m3v0, double addrspace(5)* %nm03
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m2v1, double addrspace(5)* %nm12
    store double %m3v1, double addrspace(5)* %nm13
    store double %m0v2, double addrspace(5)* %nm20
    store double %m1v2, double addrspace(5)* %nm21
    store double %m2v2, double addrspace(5)* %nm22
    store double %m3v2, double addrspace(5)* %nm23
    %nmv = load [3 x <4 x double>], [3 x <4 x double>] addrspace(5)* %nm
    ret [3 x <4 x double>] %nmv
}

; GLSL: dmat4x3 = transpose(dmat3x4)
define spir_func [4 x <3 x double>] @_Z9TransposeDv3_Dv4_d(
    [3 x <4 x double>] %m) #0
{
    %nm = alloca [4 x <3 x double>], addrspace(5)
    %nm0 = getelementptr inbounds [4 x <3 x double>], [4 x <3 x double>] addrspace(5)* %nm, i32 0, i32 0
    %nm00 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 0
    %nm01 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 1
    %nm02 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm0, i32 0, i32 2

    %nm1 = getelementptr inbounds [4 x <3 x double>], [4 x <3 x double>] addrspace(5)* %nm, i32 0, i32 1
    %nm10 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 0
    %nm11 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 1
    %nm12 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm1, i32 0, i32 2

    %nm2 = getelementptr inbounds [4 x <3 x double>], [4 x <3 x double>] addrspace(5)* %nm, i32 0, i32 2
    %nm20 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm2, i32 0, i32 0
    %nm21 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm2, i32 0, i32 1
    %nm22 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm2, i32 0, i32 2

    %nm3 = getelementptr inbounds [4 x <3 x double>], [4 x <3 x double>] addrspace(5)* %nm, i32 0, i32 3
    %nm30 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm3, i32 0, i32 0
    %nm31 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm3, i32 0, i32 1
    %nm32 = getelementptr inbounds <3 x double>, <3 x double> addrspace(5)* %nm3, i32 0, i32 2

    %m0v = extractvalue [3 x <4 x double>] %m, 0
    %m0v0 = extractelement <4 x double> %m0v, i32 0
    %m0v1 = extractelement <4 x double> %m0v, i32 1
    %m0v2 = extractelement <4 x double> %m0v, i32 2
    %m0v3 = extractelement <4 x double> %m0v, i32 3

    %m1v = extractvalue [3 x <4 x double>] %m, 1
    %m1v0 = extractelement <4 x double> %m1v, i32 0
    %m1v1 = extractelement <4 x double> %m1v, i32 1
    %m1v2 = extractelement <4 x double> %m1v, i32 2
    %m1v3 = extractelement <4 x double> %m1v, i32 3

    %m2v = extractvalue [3 x <4 x double>] %m, 2
    %m2v0 = extractelement <4 x double> %m2v, i32 0
    %m2v1 = extractelement <4 x double> %m2v, i32 1
    %m2v2 = extractelement <4 x double> %m2v, i32 2
    %m2v3 = extractelement <4 x double> %m2v, i32 3

    store double %m0v0, double addrspace(5)* %nm00
    store double %m1v0, double addrspace(5)* %nm01
    store double %m2v0, double addrspace(5)* %nm02
    store double %m0v1, double addrspace(5)* %nm10
    store double %m1v1, double addrspace(5)* %nm11
    store double %m2v1, double addrspace(5)* %nm12
    store double %m0v2, double addrspace(5)* %nm20
    store double %m1v2, double addrspace(5)* %nm21
    store double %m2v2, double addrspace(5)* %nm22
    store double %m0v3, double addrspace(5)* %nm30
    store double %m1v3, double addrspace(5)* %nm31
    store double %m2v3, double addrspace(5)* %nm32
    %nmv = load [4 x <3 x double>], [4 x <3 x double>] addrspace(5)* %nm
    ret [4 x <3 x double>] %nmv
}

; GLSL helper: double = determinant2(dvec2(double, double), dvec2(double, double))
define spir_func double @llpc.determinant2.f64(
    double %x0, double %y0, double %x1, double %y1)
{
    ; | x0   x1 |
    ; |         | = x0 * y1 - y0 * x1
    ; | y0   y1 |

    %1 = fmul double %x0, %y1
    %2 = fmul double %y0, %x1
    %3 = fsub double %1, %2
    ret double %3
}

; GLSL helper: double = determinant3(dvec3(double, double, double), dvec3(double, double, double))
define spir_func double @llpc.determinant3.f64(
    double %x0, double %y0, double %z0,
    double %x1, double %y1, double %z1,
    double %x2, double %y2, double %z2)
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

    %1 = call double @llpc.determinant2.f64(double %y1, double %z1, double %y2, double %z2)
    %2 = fmul double %1, %x0
    %3 = call double @llpc.determinant2.f64(double %z1, double %x1, double %z2, double %x2)
    %4 = fmul double %3, %y0
    %5 = fadd double %2, %4
    %6 = call double @llpc.determinant2.f64(double %x1, double %y1, double %x2, double %y2)
    %7 = fmul double %6, %z0
    %8 = fadd double %7, %5
    ret double %8
}

; GLSL helper: double = determinant4(dvec4(double, double, double, double), dvec4(double, double, double, double))
define spir_func double @llpc.determinant4.f64(
    double %x0, double %y0, double %z0, double %w0,
    double %x1, double %y1, double %z1, double %w1,
    double %x2, double %y2, double %z2, double %w2,
    double %x3, double %y3, double %z3, double %w3)

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

    %1 = call double @llpc.determinant3.f64(double %y1, double %z1, double %w1, double %y2, double %z2, double %w2, double %y3, double %z3, double %w3)
    %2 = fmul double %1, %x0
    %3 = call double @llpc.determinant3.f64(double %z1, double %x1, double %w1, double %z2, double %x2, double %w2, double %z3, double %x3, double %w3)
    %4 = fmul double %3, %y0
    %5 = fadd double %2, %4
    %6 = call double @llpc.determinant3.f64(double %x1, double %y1, double %w1, double %x2, double %y2, double %w2, double %x3, double %y3, double %w3)
    %7 = fmul double %6, %z0
    %8 = fadd double %5, %7
    %9 = call double @llpc.determinant3.f64(double %y1, double %x1, double %z1, double %y2, double %x2, double %z2, double %y3, double %x3, double %z3)
    %10 = fmul double %9, %w0
    %11 = fadd double %8, %10
    ret double %11
}

; GLSL: double = determinant(dmat2)
define spir_func double @_Z11determinantDv2_Dv2_d(
    [2 x <2 x double>] %m) #0
{
    %m0v = extractvalue [2 x <2 x double>] %m, 0
    %m0v0 = extractelement <2 x double> %m0v, i32 0
    %m0v1 = extractelement <2 x double> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x double>] %m, 1
    %m1v0 = extractelement <2 x double> %m1v, i32 0
    %m1v1 = extractelement <2 x double> %m1v, i32 1

    %d = call double @llpc.determinant2.f64(double %m0v0, double %m0v1, double %m1v0, double %m1v1)
    ret double %d
}

; GLSL: double = determinant(dmat3)
define spir_func double @_Z11determinantDv3_Dv3_d(
    [3 x <3 x double>] %m) #0
{
    %m0v = extractvalue [3 x <3 x double>] %m, 0
    %m0v0 = extractelement <3 x double> %m0v, i32 0
    %m0v1 = extractelement <3 x double> %m0v, i32 1
    %m0v2 = extractelement <3 x double> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x double>] %m, 1
    %m1v0 = extractelement <3 x double> %m1v, i32 0
    %m1v1 = extractelement <3 x double> %m1v, i32 1
    %m1v2 = extractelement <3 x double> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x double>] %m, 2
    %m2v0 = extractelement <3 x double> %m2v, i32 0
    %m2v1 = extractelement <3 x double> %m2v, i32 1
    %m2v2 = extractelement <3 x double> %m2v, i32 2

    %d = call double @llpc.determinant3.f64(
        double %m0v0, double %m0v1, double %m0v2,
        double %m1v0, double %m1v1, double %m1v2,
        double %m2v0, double %m2v1, double %m2v2)
    ret double %d
}

; GLSL: double = determinant(dmat4)
define spir_func double @_Z11determinantDv4_Dv4_d(
    [4 x <4 x double>] %m) #0
{
    %m0v = extractvalue [4 x <4 x double>] %m, 0
    %m0v0 = extractelement <4 x double> %m0v, i32 0
    %m0v1 = extractelement <4 x double> %m0v, i32 1
    %m0v2 = extractelement <4 x double> %m0v, i32 2
    %m0v3 = extractelement <4 x double> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x double>] %m, 1
    %m1v0 = extractelement <4 x double> %m1v, i32 0
    %m1v1 = extractelement <4 x double> %m1v, i32 1
    %m1v2 = extractelement <4 x double> %m1v, i32 2
    %m1v3 = extractelement <4 x double> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x double>] %m, 2
    %m2v0 = extractelement <4 x double> %m2v, i32 0
    %m2v1 = extractelement <4 x double> %m2v, i32 1
    %m2v2 = extractelement <4 x double> %m2v, i32 2
    %m2v3 = extractelement <4 x double> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x double>] %m, 3
    %m3v0 = extractelement <4 x double> %m3v, i32 0
    %m3v1 = extractelement <4 x double> %m3v, i32 1
    %m3v2 = extractelement <4 x double> %m3v, i32 2
    %m3v3 = extractelement <4 x double> %m3v, i32 3

    %d = call double @llpc.determinant4.f64(
        double %m0v0, double %m0v1, double %m0v2, double %m0v3,
        double %m1v0, double %m1v1, double %m1v2, double %m1v3,
        double %m2v0, double %m2v1, double %m2v2, double %m2v3,
        double %m3v0, double %m3v1, double %m3v2, double %m3v3)
    ret double %d
}

; GLSL helper: double = dot3(dvec3(double, double, double), dvec3(double, double, double))
define spir_func double @llpc.dot3.f64(
    double %x0, double %y0, double %z0,
    double %x1, double %y1, double %z1)
{
    %1 = fmul double %x1, %x0
    %2 = fmul double %y1, %y0
    %3 = fadd double %1, %2
    %4 = fmul double %z1, %z0
    %5 = fadd double %3, %4
    ret double %5
}

; GLSL helper: double = dot4(dvec4(double, double, double, double), dvec4(double, double, double, double))
define spir_func double @llpc.dot4.f64(
    double %x0, double %y0, double %z0, double %w0,
    double %x1, double %y1, double %z1, double %w1)
{
    %1 = fmul double %x1, %x0
    %2 = fmul double %y1, %y0
    %3 = fadd double %1, %2
    %4 = fmul double %z1, %z0
    %5 = fadd double %3, %4
    %6 = fmul double %w1, %w0
    %7 = fadd double %5, %6
    ret double %7
}

; GLSL: dmat2 = inverse(dmat2)
define spir_func [2 x <2 x double>] @_Z13matrixInverseDv2_Dv2_d(
    [2 x <2 x double>] %m) #0
{
    ; [ x0   x1 ]                    [  y1 -x1 ]
    ; [         ]  = (1 / det(M))) * [         ]
    ; [ y0   y1 ]                    [ -y0  x0 ]
    %m0v = extractvalue [2 x <2 x double>] %m, 0
    %x0 = extractelement <2 x double> %m0v, i32 0
    %y0 = extractelement <2 x double> %m0v, i32 1

    %m1v = extractvalue [2 x <2 x double>] %m, 1
    %x1 = extractelement <2 x double> %m1v, i32 0
    %y1 = extractelement <2 x double> %m1v, i32 1

    %1 = call double @llpc.determinant2.f64(double %x0, double %y0, double %x1, double %y1)
    %2 = fdiv double 1.0, %1
    %3 = fsub double 0.0, %2
    %4 = fmul double %2, %y1
    %5 = fmul double %3, %y0
    %6 = fmul double %3, %x1
    %7 = fmul double %2, %x0
    %8 = insertelement <2 x double> undef, double %4, i32 0
    %9 = insertelement <2 x double> %8, double %5, i32 1
    %10 = insertvalue [2 x <2 x double>] undef, <2 x double> %9, 0
    %11 = insertelement <2 x double> undef, double %6, i32 0
    %12 = insertelement <2 x double> %11, double %7, i32 1
    %13 = insertvalue [2 x <2 x double>] %10 , <2 x double> %12, 1

    ret [2 x <2 x double>]  %13
}

; GLSL: dmat3 = inverse(dmat3)
define spir_func [3 x <3 x double>] @_Z13matrixInverseDv3_Dv3_d(
    [3 x <3 x double>] %m) #0
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

    %m0v = extractvalue [3 x <3 x double>] %m, 0
    %x0 = extractelement <3 x double> %m0v, i32 0
    %y0 = extractelement <3 x double> %m0v, i32 1
    %z0 = extractelement <3 x double> %m0v, i32 2

    %m1v = extractvalue [3 x <3 x double>] %m, 1
    %x1 = extractelement <3 x double> %m1v, i32 0
    %y1 = extractelement <3 x double> %m1v, i32 1
    %z1 = extractelement <3 x double> %m1v, i32 2

    %m2v = extractvalue [3 x <3 x double>] %m, 2
    %x2 = extractelement <3 x double> %m2v, i32 0
    %y2 = extractelement <3 x double> %m2v, i32 1
    %z2 = extractelement <3 x double> %m2v, i32 2

    %adjx0 = call double @llpc.determinant2.f64(double %y1, double %z1, double %y2, double %z2)
    %adjx1 = call double @llpc.determinant2.f64(double %y2, double %z2, double %y0, double %z0)
    %adjx2 = call double @llpc.determinant2.f64(double %y0, double %z0, double %y1, double %z1)

    %det = call double @llpc.dot3.f64(double %x0, double %x1, double %x2,
                    double %adjx0, double %adjx1, double %adjx2)
    %rdet = fdiv double 1.0, %det

    %nx0 = fmul double %rdet, %adjx0
    %nx1 = fmul double %rdet, %adjx1
    %nx2 = fmul double %rdet, %adjx2

    %m00 = insertelement <3 x double> undef, double %nx0, i32 0
    %m01 = insertelement <3 x double> %m00, double %nx1, i32 1
    %m02 = insertelement <3 x double> %m01, double %nx2, i32 2
    %m0 = insertvalue [3 x <3 x double>] undef, <3 x double> %m02, 0

    %adjy0 = call double @llpc.determinant2.f64(double %z1, double %x1, double %z2, double %x2)
    %adjy1 = call double @llpc.determinant2.f64(double %z2, double %x2, double %z0, double %x0)
    %adjy2 = call double @llpc.determinant2.f64(double %z0, double %x0, double %z1, double %x1)


    %ny0 = fmul double %rdet, %adjy0
    %ny1 = fmul double %rdet, %adjy1
    %ny2 = fmul double %rdet, %adjy2

    %m10 = insertelement <3 x double> undef, double %ny0, i32 0
    %m11 = insertelement <3 x double> %m10, double %ny1, i32 1
    %m12 = insertelement <3 x double> %m11, double %ny2, i32 2
    %m1 = insertvalue [3 x <3 x double>] %m0, <3 x double> %m12, 1

    %adjz0 = call double @llpc.determinant2.f64(double %x1, double %y1, double %x2, double %y2)
    %adjz1 = call double @llpc.determinant2.f64(double %x2, double %y2, double %x0, double %y0)
    %adjz2 = call double @llpc.determinant2.f64(double %x0, double %y0, double %x1, double %y1)

    %nz0 = fmul double %rdet, %adjz0
    %nz1 = fmul double %rdet, %adjz1
    %nz2 = fmul double %rdet, %adjz2

    %m20 = insertelement <3 x double> undef, double %nz0, i32 0
    %m21 = insertelement <3 x double> %m20, double %nz1, i32 1
    %m22 = insertelement <3 x double> %m21, double %nz2, i32 2
    %m2 = insertvalue [3 x <3 x double>] %m1, <3 x double> %m22, 2

    ret [3 x <3 x double>] %m2
}

; GLSL: dmat4 = inverse(dmat4)
define spir_func [4 x <4 x double>] @_Z13matrixInverseDv4_Dv4_d(
    [4 x <4 x double>] %m) #0
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

    %m0v = extractvalue [4 x <4 x double>] %m, 0
    %x0 = extractelement <4 x double> %m0v, i32 0
    %y0 = extractelement <4 x double> %m0v, i32 1
    %z0 = extractelement <4 x double> %m0v, i32 2
    %w0 = extractelement <4 x double> %m0v, i32 3

    %m1v = extractvalue [4 x <4 x double>] %m, 1
    %x1 = extractelement <4 x double> %m1v, i32 0
    %y1 = extractelement <4 x double> %m1v, i32 1
    %z1 = extractelement <4 x double> %m1v, i32 2
    %w1 = extractelement <4 x double> %m1v, i32 3

    %m2v = extractvalue [4 x <4 x double>] %m, 2
    %x2 = extractelement <4 x double> %m2v, i32 0
    %y2 = extractelement <4 x double> %m2v, i32 1
    %z2 = extractelement <4 x double> %m2v, i32 2
    %w2 = extractelement <4 x double> %m2v, i32 3

    %m3v = extractvalue [4 x <4 x double>] %m, 3
    %x3 = extractelement <4 x double> %m3v, i32 0
    %y3 = extractelement <4 x double> %m3v, i32 1
    %z3 = extractelement <4 x double> %m3v, i32 2
    %w3 = extractelement <4 x double> %m3v, i32 3

    %adjx0 = call double @llpc.determinant3.f64(
            double %y1, double %z1, double %w1,
            double %y2, double %z2, double %w2,
            double %y3, double %z3, double %w3)
    %adjx1 = call double @llpc.determinant3.f64(
            double %y2, double %z2, double %w2,
            double %y0, double %z0, double %w0,
            double %y3, double %z3, double %w3)
    %adjx2 = call double @llpc.determinant3.f64(
            double %y3, double %z3, double %w3,
            double %y0, double %z0, double %w0,
            double %y1, double %z1, double %w1)
    %adjx3 = call double @llpc.determinant3.f64(
            double %y0, double %z0, double %w0,
            double %y2, double %z2, double %w2,
            double %y1, double %z1, double %w1)

    %det = call double @llpc.dot4.f64(double %x0, double %x1, double %x2, double %x3,
            double %adjx0, double %adjx1, double %adjx2, double %adjx3)
    %rdet = fdiv double 1.0, %det

    %nx0 = fmul double %rdet, %adjx0
    %nx1 = fmul double %rdet, %adjx1
    %nx2 = fmul double %rdet, %adjx2
    %nx3 = fmul double %rdet, %adjx3

    %m00 = insertelement <4 x double> undef, double %nx0, i32 0
    %m01 = insertelement <4 x double> %m00, double %nx1, i32 1
    %m02 = insertelement <4 x double> %m01, double %nx2, i32 2
    %m03 = insertelement <4 x double> %m02, double %nx3, i32 3
    %m0 = insertvalue [4 x <4 x double>] undef, <4 x double> %m03, 0

    %adjy0 = call double @llpc.determinant3.f64(
            double %z2, double %w2, double %x2,
            double %z1, double %w1, double %x1,
            double %z3, double %w3, double %x3)
    %adjy1 = call double @llpc.determinant3.f64(
             double %z2, double %w2, double %x2,
             double %z3, double %w3, double %x3,
             double %z0, double %w0, double %x0)
    %adjy2 = call double @llpc.determinant3.f64(
            double %z0, double %w0, double %x0,
            double %z3, double %w3, double %x3,
            double %z1, double %w1, double %x1)
    %adjy3 = call double @llpc.determinant3.f64(
            double %z0, double %w0, double %x0,
            double %z1, double %w1, double %x1,
            double %z2, double %w2, double %x2)

    %ny0 = fmul double %rdet, %adjy0
    %ny1 = fmul double %rdet, %adjy1
    %ny2 = fmul double %rdet, %adjy2
    %ny3 = fmul double %rdet, %adjy3

    %m10 = insertelement <4 x double> undef, double %ny0, i32 0
    %m11 = insertelement <4 x double> %m10, double %ny1, i32 1
    %m12 = insertelement <4 x double> %m11, double %ny2, i32 2
    %m13 = insertelement <4 x double> %m12, double %ny3, i32 3
    %m1 = insertvalue [4 x <4 x double>] %m0, <4 x double> %m13, 1

    %adjz0 = call double @llpc.determinant3.f64(
            double %w1, double %x1, double %y1,
            double %w2, double %x2, double %y2,
            double %w3, double %x3, double %y3)
    %adjz1 = call double @llpc.determinant3.f64(
            double %w3, double %x3, double %y3,
            double %w2, double %x2, double %y2,
            double %w0, double %x0, double %y0)
    %adjz2 = call double @llpc.determinant3.f64(
            double %w3, double %x3, double %y3,
            double %w0, double %x0, double %y0,
            double %w1, double %x1, double %y1)
    %adjz3 = call double @llpc.determinant3.f64(
            double %w1, double %x1, double %y1,
            double %w0, double %x0, double %y0,
            double %w2, double %x2, double %y2)

    %nz0 = fmul double %rdet, %adjz0
    %nz1 = fmul double %rdet, %adjz1
    %nz2 = fmul double %rdet, %adjz2
    %nz3 = fmul double %rdet, %adjz3

    %m20 = insertelement <4 x double> undef, double %nz0, i32 0
    %m21 = insertelement <4 x double> %m20, double %nz1, i32 1
    %m22 = insertelement <4 x double> %m21, double %nz2, i32 2
    %m23 = insertelement <4 x double> %m22, double %nz3, i32 3
    %m2 = insertvalue [4 x <4 x double>] %m1, <4 x double> %m23, 2

    %adjw0 = call double @llpc.determinant3.f64(
            double %x2, double %y2, double %z2,
            double %x1, double %y1, double %z1,
            double %x3, double %y3, double %z3)
    %adjw1 = call double @llpc.determinant3.f64(
            double %x2, double %y2, double %z2,
            double %x3, double %y3, double %z3,
            double %x0, double %y0, double %z0)
    %adjw2 = call double @llpc.determinant3.f64(
            double %x0, double %y0, double %z0,
            double %x3, double %y3, double %z3,
            double %x1, double %y1, double %z1)
    %adjw3 = call double @llpc.determinant3.f64(
            double %x0, double %y0, double %z0,
            double %x1, double %y1, double %z1,
            double %x2, double %y2, double %z2)

    %nw0 = fmul double %rdet, %adjw0
    %nw1 = fmul double %rdet, %adjw1
    %nw2 = fmul double %rdet, %adjw2
    %nw3 = fmul double %rdet, %adjw3

    %m30 = insertelement <4 x double> undef, double %nw0, i32 0
    %m31 = insertelement <4 x double> %m30, double %nw1, i32 1
    %m32 = insertelement <4 x double> %m31, double %nw2, i32 2
    %m33 = insertelement <4 x double> %m32, double %nw3, i32 3
    %m3 = insertvalue [4 x <4 x double>] %m2, <4 x double> %m33, 3

    ret [4 x <4 x double>] %m3
}

attributes #0 = { nounwind }
