;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024"
target triple = "spir64-unknown-unknown"

; NGG culling: fetch culling-control registers from primitive shader table
define i32 @llpc.ngg.culling.fetchreg(
    i32 %primShaderTablePtrLow,
    i32 %primShaderTablePtrHigh,
    i32 %regOffset
    ) #2
{
    %1 = insertelement <2 x i32> undef, i32 %primShaderTablePtrLow, i32 0
    %2 = insertelement <2 x i32> %1, i32 %primShaderTablePtrHigh, i32 1
    %3 = bitcast <2 x i32> %2 to i64
    %4 = inttoptr i64 %3 to [256 x i32] addrspace(4)*

    %5 = lshr i32 %regOffset, 2 ; DWORD offset
    ; addrspace(4) = AMDGPUAS::Constant
    %6 = getelementptr [256 x i32], [256 x i32] addrspace(4)* %4, i32 0, i32 %5, !amdgpu.uniform !0
    ; NOTE: Use "volatile" to prevent backend compiler changing the load opertions. This is documented in LLVM language
    ; reference manual.
    %7 = load volatile i32, i32 addrspace(4)* %6, align 4, !invariant.load !0

    ret i32 %7
}

; NGG culling: backface culler
define i1 @llpc.ngg.culling.backface(
    i1          %cullFlag,
    <4 x float> %vtx0,
    <4 x float> %vtx1,
    <4 x float> %vtx2,
    i32         %backfaceExponent,
    i32         %paSuScModeCntl,
    i32         %paClVportXscale,
    i32         %paClVportYscale
    ) #1
{
.backfaceEntry:
    ; If cull flag has already been TRUE, early return
    br i1 %cullFlag, label %.backfaceExit, label %.backfaceCull

.backfaceCull:
    ;
    ; Backface culling algorithm is described as follow:
    ;
    ;   if (((area > 0) && (face == CCW)) || ((area < 0) && (face == CW)))
    ;       frontFace = true
    ;
    ;   if (((area < 0) && (face == CCW)) || ((area > 0) && (face == CW)))
    ;       backFace = true
    ;
    ;   if ((area == 0) || (frontFace && cullFront) || (backFace && cullBack))
    ;       cullFlag = true
    ;

    ;        | x0 y0 w0 |
    ;        |          |
    ; area = | x1 y1 w1 | =  x0 * (y1 * w2 - y2 * w1) - x1 * (y0 * w2 - y2 * w0) + x2 * (y0 * w1 - y1 * w0)
    ;        |          |
    ;        | x2 y2 w2 |
    %0 = extractelement <4 x float> %vtx0, i32 0    ; x0
    %1 = extractelement <4 x float> %vtx0, i32 1    ; y0
    %2 = extractelement <4 x float> %vtx0, i32 3    ; w0

    %3 = extractelement <4 x float> %vtx1, i32 0    ; x1
    %4 = extractelement <4 x float> %vtx1, i32 1    ; y1
    %5 = extractelement <4 x float> %vtx1, i32 3    ; w1

    %6 = extractelement <4 x float> %vtx2, i32 0    ; x2
    %7 = extractelement <4 x float> %vtx2, i32 1    ; y2
    %8 = extractelement <4 x float> %vtx2, i32 3    ; w2

    %9  = fmul float %4, %8    ; y1 * w2
    %10 = fmul float %7, %5    ; y2 * w1
    %11 = fsub float %9, %10   ; y1 * w2 - y2 * w1
    %12 = fmul float %0, %11   ; x0 * (y1 * w2 - y2 * w1)

    %13 = fmul float %1, %8    ; y0 * w2
    %14 = fmul float %7, %2    ; y2 * w0
    %15 = fsub float %13, %14  ; y0 * w2 - y2 * w0
    %16 = fmul float %3, %15   ; x1 * (y0 * w2 - y2 * w0)

    %17 = fmul float %1, %5    ; y0 * w1
    %18 = fmul float %4, %2    ; y1 * w0
    %19 = fsub float %17, %18  ; y0 * w1 - y1 * w0
    %20 = fmul float %6, %19   ; x2 * (y0 * w1 - y1 * w0)

    ; area = x0 * (y1 * w2 - y2 * w1) - x1 * (y0 * w2 - y2 * w0) + x2 * (y0 * w1 - y1 * w0)
    %21 = fsub float %12, %16
    %22 = fadd float %21, %20

    ; area < 0
    %23 = fcmp olt float %22, 0.0

    ; area > 0
    %24 = fcmp ogt float %22, 0.0

    ; xScale ^ yScale
    %25 = xor i32 %paClVportXscale, %paClVportYscale

    ; signbit(xScale ^ yScale)
    %26 = call i32 @llvm.amdgcn.ubfe.i32(i32 %25, i32 31, i32 1)

    ; face = (FACE, PA_SU_SC_MODE_CNTRL[2], 0 = CCW, 1 = CW)
    %27 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paSuScModeCntl, i32 2, i32 1)

    ; face ^ signbit(xScale ^ yScale)
    %28 = xor i32 %26, %27

    ; (face ^ signbit(xScale ^ yScale)) == 0
    %29 = icmp eq i32 %28, 0

    ; frontFace = ((face ^ signbit(xScale ^ yScale)) == 0) ? (area < 0) ? (area > 0)
    %30 = select i1 %29, i1 %23, i1 %24

    ; backFace = !frontFace
    %31 = xor i1 %30, true

    ; cullFront = (CULL_FRONT, PA_SU_SC_MODE_CNTRL[0], 0 = DONT CULL, 1 = CULL)
    %32 = and i32 %paSuScModeCntl, 1
    %33 = trunc i32 %32 to i1

    ; cullBack = (CULL_BACK, PA_SU_SC_MODE_CNTRL[1], 0 = DONT CULL, 1 = CULL)
    %34 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paSuScModeCntl, i32 1, i32 1)
    %35 = trunc i32 %34 to i1

    ; cullFront ? frontFace : false
    %36 = select i1 %33, i1 %30, i1 false

    ; cullBack ? backFace : false
    %37 = select i1 %35, i1 %31, i1 false

    ; cullFlag = (cullFront ? frontFace : false) || (cullBack ? backFace : false)
    %38 = or i1 %36, %37

    ; backfaceExponent != 0
    %39 = icmp ne i32 %backfaceExponent, 0
    br i1 %39, label %.backfaceExponent, label %.endBackfaceCull

.backfaceExponent:
    ;
    ; Ignore area calculations that are less enough
    ;   if (|area| < (10 ^ (-backfaceExponent)) / |w0 * w1 * w2|)
    ;       cullFlag = false
    ;

    %40 = fmul float %2, %5                         ; w0 * w1
    %41 = fmul float %40, %8                        ; w0 * w1 * w2
    %42 = call float @llvm.fabs.f32(float %41)      ; abs(w0 * w1 * w2)
    %43 = fdiv float 1.0, %42                       ; 1/abs(w0 * w1 * w2)

    ; threshold = (10 ^ (-backfaceExpoent)) / abs(w0 * w1 * w2)
    %44 = sub i32 0, %backfaceExponent
    %45 = call float @llvm.powi.f32(float 10.0, i32 %44)
    %46 = fmul float %45, %43

    ; abs(area) >= threshold
    %47 = call float @llvm.fabs.f32(float %22)
    %48 = fcmp oge float %47, %46

    ; cullFlag = cullFlag && (abs(area) >= threshold)
    %49 = and i1 %38, %48

    br label %.endBackfaceCull

.endBackfaceCull:
    ; cullFlag = cullFlag || (area == 0)
    %50 = phi i1 [ %38, %.backfaceCull ], [ %49, %.backfaceExponent ]
    %51 = fcmp oeq float %22, 0.0
    %52 = or i1 %50, %51

    br label %.backfaceExit

.backfaceExit:
    %53 = phi i1 [ %cullFlag, %.backfaceEntry ], [ %52, %.endBackfaceCull ]

    ; polyMode = (POLY_MODE, PA_SU_SC_MODE_CNTRL[4:3], 0 = DISABLE, 1 = DUAL)
    %54 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paSuScModeCntl, i32 3, i32 2)

    ; polyMode == 1
    %55 = icmp eq i32 %54, 1

    ; Disable backface culler if POLY_MODE is set to 1 (wireframe)
    ; cullFlag = (polyMode == 1) ? false : cullFlag
    %56 = select i1 %55, i1 false, i1 %53

    ret i1 %56
}

; NGG culling: frustum culler
define i1 @llpc.ngg.culling.frustum(
    i1          %cullFlag,
    <4 x float> %vtx0,
    <4 x float> %vtx1,
    <4 x float> %vtx2,
    i32         %paClClipCntl,
    i32         %paClGbHorzDiscAdj,
    i32         %paClGbVertDiscAdj
    ) #1
{
.frustumEntry:
    ; If cull flag has already been TRUE, early return
    br i1 %cullFlag, label %.frustumExit, label %.frustumCull

.frustumCull:
    ;
    ; Frustum culling algorithm is described as follow:
    ;
    ;   if ((x[i] > xDiscAdj * w[i]) && (y[i] > yDiscAdj * w[i]) && (z[i] > zFar * w[i]))
    ;       cullFlag = true
    ;
    ;   if ((x[i] < -xDiscAdj * w[i]) && (y[i] < -yDiscAdj * w[i]) && (z[i] < zNear * w[i]))
    ;       cullFlag &= true
    ;
    ;   i = [0..2]
    ;

    ; clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    %0 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClClipCntl, i32 19, i32 1)
    %1 = trunc i32 %0 to i1

    ; zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    %2 = select i1 %1, float -1.0, float 0.0

    ; xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    %3 = bitcast i32 %paClGbHorzDiscAdj to float

    ; yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    %4 = bitcast i32 %paClGbVertDiscAdj to float

    %5 = extractelement <4 x float> %vtx0, i32 0    ; x0
    %6 = extractelement <4 x float> %vtx0, i32 1    ; y0
    %7 = extractelement <4 x float> %vtx0, i32 2    ; z0
    %8 = extractelement <4 x float> %vtx0, i32 3    ; w0

    %9  = extractelement <4 x float> %vtx1, i32 0   ; x1
    %10 = extractelement <4 x float> %vtx1, i32 1   ; y1
    %11 = extractelement <4 x float> %vtx1, i32 2   ; z1
    %12 = extractelement <4 x float> %vtx1, i32 3   ; w1

    %13 = extractelement <4 x float> %vtx2, i32 0   ; x2
    %14 = extractelement <4 x float> %vtx2, i32 1   ; y2
    %15 = extractelement <4 x float> %vtx2, i32 2   ; z2
    %16 = extractelement <4 x float> %vtx2, i32 3   ; w2

    %17 = fsub float 0.0, %3    ; -xDiscAdj
    %18 = fsub float 0.0, %4    ; -yDiscAdj

    ; Get clip mask for vertex0
    %19 = fmul float %17, %8            ; -xDiscAdj * w0
    %20 = fcmp olt float %5, %19        ; x0 < -xDiscAdj * w0
    %21 = select i1 %20, i32 1, i32 0   ; (x0 < -xDiscAdj * w0) ? 0x1 : 0

    %22 = fmul float %3, %8             ; xDiscAdj * w0
    %23 = fcmp ogt float %5, %22        ; x0 > xDiscAdj * w0
    %24 = select i1 %23, i32 2, i32 0   ; (x0 > xDiscAdj * w0) ? 0x2 : 0

    %25 = fmul float %18, %8            ; -yDiscAdj * w0
    %26 = fcmp olt float %6, %25        ; y0 < -yDiscAdj * w0
    %27 = select i1 %26, i32 4, i32 0   ; (y0 < -yDiscAdj * w0) ? 0x4 : 0

    %28 = fmul float %4, %8             ; yDiscAdj * w0
    %29 = fcmp ogt float %6, %28        ; y0 > yDiscAdj * w0
    %30 = select i1 %29, i32 8, i32 0   ; (y0 > yDiscAdj * w0) ? 0x8 : 0

    %31 = fmul float %2, %8             ; zNear * w0
    %32 = fcmp olt float %7, %31        ; z0 < zNear * w0
    %33 = select i1 %32, i32 16, i32 0  ; (z0 < zNear * w0) ? 0x10 : 0

    %34 = fcmp ogt float %7, %8         ; z0 > w0
    %35 = select i1 %34, i32 32, i32 0  ; (z0 > w0) ? 0x20 : 0

    ; clipMask0
    %36 = or i32 %21, %24
    %37 = or i32 %27, %30
    %38 = or i32 %33, %35
    %39 = or i32 %36, %37
    %40 = or i32 %38, %39

    ; Get clip mask for vertex1
    %41 = fmul float %17, %12           ; -xDiscAdj * w1
    %42 = fcmp olt float %9, %41        ; x1 < -xDiscAdj * w1
    %43 = select i1 %42, i32 1, i32 0   ; (x1 < -xDiscAdj * w1) ? 0x1 : 0

    %44 = fmul float %3, %12            ; xDiscAdj * w1
    %45 = fcmp ogt float %9, %44        ; x1 > xDiscAdj * w1
    %46 = select i1 %45, i32 2, i32 0   ; (x1 > xDiscAdj * w1) ? 0x2 : 0

    %47 = fmul float %18, %12           ; -yDiscAdj * w1
    %48 = fcmp olt float %10, %47       ; y1 < -yDiscAdj * w1
    %49 = select i1 %48, i32 4, i32 0   ; (y1 < -yDiscAdj * w1) ? 0x4 : 0

    %50 = fmul float %4, %12            ; yDiscAdj * w1
    %51 = fcmp ogt float %10, %50       ; y1 > yDiscAdj * w1
    %52 = select i1 %51, i32 8, i32 0   ; (y1 > yDiscAdj * w1) ? 0x8 : 0

    %53 = fmul float %2, %12            ; zNear * w1
    %54 = fcmp olt float %11, %53       ; z1 < zNear * w1
    %55 = select i1 %54, i32 16, i32 0  ; (z1 < zNear * w1) ? 0x10 : 0

    %56 = fcmp ogt float %11, %12       ; z1 > w1
    %57 = select i1 %56, i32 32, i32 0  ; (z1 > w1) ? 0x20 : 0

    ; clipMask1
    %58 = or i32 %43, %46
    %59 = or i32 %49, %52
    %60 = or i32 %55, %57
    %61 = or i32 %58, %59
    %62 = or i32 %60, %61

    ; Get clip mask for vertex2
    %63 = fmul float %17, %16           ; -xDiscAdj * w2
    %64 = fcmp olt float %13, %63       ; x2 < -xDiscAdj * w2
    %65 = select i1 %64, i32 1, i32 0   ; (x2 < -xDiscAdj * w2) ? 0x1 : 0

    %66 = fmul float %3, %16            ; xDiscAdj * w2
    %67 = fcmp ogt float %13, %66       ; x2 > xDiscAdj * w2
    %68 = select i1 %67, i32 2, i32 0   ; (x2 > xDiscAdj * w2) ? 0x2 : 0

    %69 = fmul float %18, %16           ; -yDiscAdj * w2
    %70 = fcmp olt float %14, %69       ; y2 < -yDiscAdj * w2
    %71 = select i1 %70, i32 4, i32 0   ; (y2 < -yDiscAdj * w2) ? 0x4 : 0

    %72 = fmul float %4, %16            ; yDiscAdj * w2
    %73 = fcmp ogt float %14, %72       ; y2 > yDiscAdj * w2
    %74 = select i1 %73, i32 8, i32 0   ; (y2 > yDiscAdj * w2) ? 0x8 : 0

    %75 = fmul float %2, %16            ; zNear * w2
    %76 = fcmp olt float %15, %75       ; z2 < zNear * w2
    %77 = select i1 %76, i32 16, i32 0  ; (z2 < zNear * w2) ? 0x10 : 0

    %78 = fcmp ogt float %15, %16       ; z2 > w1
    %79 = select i1 %78, i32 32, i32 0  ; (z2 > w1) ? 0x20 : 0

    ; clipMask2
    %80 = or i32 %65, %68
    %81 = or i32 %71, %74
    %82 = or i32 %77, %79
    %83 = or i32 %80, %81
    %84 = or i32 %82, %83

    ; clipMask = clipMask0 & clipMask1 & clipMask2
    %85 = and i32 %40, %62
    %86 = and i32 %84, %85

    ; cullFlag = (clipMask != 0)
    %87 = icmp ne i32 %86, 0

    br label %.frustumExit

.frustumExit:
    %88 = phi i1 [ %cullFlag, %.frustumEntry ], [ %87, %.frustumCull ]
    ret i1 %88
}

; NGG culling: box filter culler
define i1 @llpc.ngg.culling.boxfilter(
    i1          %cullFlag,
    <4 x float> %vtx0,
    <4 x float> %vtx1,
    <4 x float> %vtx2,
    i32         %paClVteCntl,
    i32         %paClClipCntl,
    i32         %paClGbHorzDiscAdj,
    i32         %paClGbVertDiscAdj
    ) #1
{
.boxfilterEntry:
    ; If cull flag has already been TRUE, early return
    br i1 %cullFlag, label %.boxfilterExit, label %.boxfilterCull

.boxfilterCull:
    ;
    ; Box filter culling algorithm is described as follow:
    ;
    ;   if ((min(x0/w0, x1/w1, x2/w2) > xDiscAdj)  ||
    ;       (max(x0/w0, x1/w1, x2/w2) < -xDiscAdj) ||
    ;       (min(y0/w0, y1/w1, y2/w2) > yDiscAdj)  ||
    ;       (max(y0/w0, y1/w1, y2/w2) < -yDiscAdj) ||
    ;       (min(z0/w0, z1/w1, z2/w2) > zFar)      ||
    ;       (min(z0/w0, z1/w1, z2/w2) < zNear))
    ;       cullFlag = true
    ;

    ;vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    %0 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClVteCntl, i32 8, i32 1)
    %1 = trunc i32 %0 to i1

    ;vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClVteCntl, i32 9, i32 1)
    %3 = trunc i32 %2 to i1

    ; clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    %4 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClClipCntl, i32 19, i32 1)
    %5 = trunc i32 %4 to i1

    ; zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    %6 = select i1 %5, float -1.0, float 0.0

    ; xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    %7 = bitcast i32 %paClGbHorzDiscAdj to float

    ; yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    %8 = bitcast i32 %paClGbVertDiscAdj to float

    %9  = extractelement <4 x float> %vtx0, i32 0   ; x0
    %10 = extractelement <4 x float> %vtx0, i32 1   ; y0
    %11 = extractelement <4 x float> %vtx0, i32 2   ; z0
    %12 = extractelement <4 x float> %vtx0, i32 3   ; w0

    %13 = extractelement <4 x float> %vtx1, i32 0   ; x1
    %14 = extractelement <4 x float> %vtx1, i32 1   ; y1
    %15 = extractelement <4 x float> %vtx1, i32 2   ; z1
    %16 = extractelement <4 x float> %vtx1, i32 3   ; w1

    %17 = extractelement <4 x float> %vtx2, i32 0   ; x2
    %18 = extractelement <4 x float> %vtx2, i32 1   ; y2
    %19 = extractelement <4 x float> %vtx2, i32 2   ; z2
    %20 = extractelement <4 x float> %vtx2, i32 3   ; w2

    ; Convert xyz coordinate to normalized device coordinate (NDC)
    %21 = fdiv float 1.0, %12   ; 1/w0
    %22 = fdiv float 1.0, %16   ; 1/w1
    %23 = fdiv float 1.0, %20   ; 1/w2

    %24 = select i1 %1, float 1.0, float %21    ; VTX_XY_FMT ? 1.0 : 1/w0
    %25 = select i1 %1, float 1.0, float %22    ; VTX_XY_FMT ? 1.0 : 1/w1
    %26 = select i1 %1, float 1.0, float %23    ; VTX_XY_FMT ? 1.0 : 1/w2

    %27 = select i1 %3, float 1.0, float %21    ; VTX_Z_FMT ? 1.0 : 1/w0
    %28 = select i1 %3, float 1.0, float %22    ; VTX_Z_FMT ? 1.0 : 1/w1
    %29 = select i1 %3, float 1.0, float %23    ; VTX_Z_FMT ? 1.0 : 1/w2

    %30 = fmul float %9,  %24       ; x0' = x0/w0
    %31 = fmul float %10, %24       ; y0' = y0/w0
    %32 = fmul float %11, %27       ; z0' = z0/w0

    %33 = fmul float %13, %25       ; x1' = x1/w1
    %34 = fmul float %14, %25       ; y1' = y1/w1
    %35 = fmul float %15, %28       ; z1' = z1/w1

    %36 = fmul float %17, %26       ; x2' = x2/w2
    %37 = fmul float %18, %26       ; y2' = y2/w2
    %38 = fmul float %19, %29       ; z2' = z2/w2

    %39 = fsub float 0.0, %7    ; -xDiscAdj
    %40 = fsub float 0.0, %8    ; -yDiscAdj

    ; minX = min(x0', x1', x2')
    %41 = call float @llpc.fmin3.f32(float %30, float %33, float %36)

    ; minX > xDiscAdj
    %42 = fcmp ogt float %41, %7

    ; maxX = max(x0', x1', x2')
    %43 = call float @llpc.fmax3.f32(float %30, float %33, float %36)

    ; maxX < -xDiscAdj
    %44 = fcmp olt float %43, %39

    ; minY = min(y0', y1', y2')
    %45 = call float @llpc.fmin3.f32(float %31, float %34, float %37)

    ; minY > yDiscAdj
    %46 = fcmp ogt float %45, %8

    ; maxY = max(y0', y1', y2')
    %47 = call float @llpc.fmax3.f32(float %31, float %34, float %37)

    ; maxY < -yDiscAdj
    %48 = fcmp olt float %47, %40

    ; minZ = min(z0', z1', z2')
    %49 = call float @llpc.fmin3.f32(float %32, float %35, float %38)

    ; minZ > zFar (1.0)
    %50 = fcmp ogt float %49, 1.0

    ; maxZ = min(z0', z1', z2')
    %51 = call float @llpc.fmax3.f32(float %32, float %35, float %38)

    ; maxZ < zNear
    %52 = fcmp olt float %51, %6

    ; Get cull flag
    %53 = or i1 %42, %44
    %54 = or i1 %46, %48
    %55 = or i1 %50, %52
    %56 = or i1 %53, %54
    %57 = or i1 %55, %56

    br label %.boxfilterExit

.boxfilterExit:
    %58 = phi i1 [ %cullFlag, %.boxfilterEntry ], [ %57, %.boxfilterCull ]
    ret i1 %58
}

; NGG culling: sphere culler
define i1 @llpc.ngg.culling.sphere(
    i1          %cullFlag,
    <4 x float> %vtx0,
    <4 x float> %vtx1,
    <4 x float> %vtx2,
    i32         %paClVteCntl,
    i32         %paClClipCntl,
    i32         %paClGbHorzDiscAdj,
    i32         %paClGbVertDiscAdj
    ) #1
{
.sphereEntry:
    ; If cull flag has already been TRUE, early return
    br i1 %cullFlag, label %.sphereExit, label %.sphereCull

.sphereCull:
    ;
    ; Sphere culling algorithm is somewhat complex and is described as following steps:
    ;   (1) Transform discard space to -1..1 space;
    ;   (2) Project from 3D coordinates to barycentric coordinates;
    ;   (3) Solve linear system and find barycentric coordinates of the point closest to the origin;
    ;   (4) Do clamping for the closest point if necessary;
    ;   (5) Backproject from barycentric coordinates to 3D coordinates;
    ;   (6) Compute the distance squared from 3D coordinates of the closest point;
    ;   (7) Compare the distance with 3.0 and determine the cull flag.
    ;

    ;vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    %0 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClVteCntl, i32 8, i32 1)
    %1 = trunc i32 %0 to i1

    ;vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    %2 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClVteCntl, i32 9, i32 1)
    %3 = trunc i32 %2 to i1

    ; clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    %4 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClClipCntl, i32 19, i32 1)
    %5 = trunc i32 %4 to i1

    ; zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    %6 = select i1 %5, float -1.0, float 0.0

    ; xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    %7 = bitcast i32 %paClGbHorzDiscAdj to float

    ; yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    %8 = bitcast i32 %paClGbVertDiscAdj to float

    %9  = extractelement <4 x float> %vtx0, i32 0   ; x0
    %10 = extractelement <4 x float> %vtx0, i32 1   ; y0
    %11 = extractelement <4 x float> %vtx0, i32 2   ; z0
    %12 = extractelement <4 x float> %vtx0, i32 3   ; w0

    %13 = extractelement <4 x float> %vtx1, i32 0   ; x1
    %14 = extractelement <4 x float> %vtx1, i32 1   ; y1
    %15 = extractelement <4 x float> %vtx1, i32 2   ; z1
    %16 = extractelement <4 x float> %vtx1, i32 3   ; w1

    %17 = extractelement <4 x float> %vtx2, i32 0   ; x2
    %18 = extractelement <4 x float> %vtx2, i32 1   ; y2
    %19 = extractelement <4 x float> %vtx2, i32 2   ; z2
    %20 = extractelement <4 x float> %vtx2, i32 3   ; w2

    ; Convert xyz coordinate to normalized device coordinate (NDC)
    %21 = fdiv float 1.0, %12   ; 1/w0
    %22 = fdiv float 1.0, %16   ; 1/w1
    %23 = fdiv float 1.0, %20   ; 1/w2

    %24 = select i1 %1, float 1.0, float %21    ; VTX_XY_FMT ? 1.0 : 1/w0
    %25 = select i1 %1, float 1.0, float %22    ; VTX_XY_FMT ? 1.0 : 1/w1
    %26 = select i1 %1, float 1.0, float %23    ; VTX_XY_FMT ? 1.0 : 1/w2

    %27 = select i1 %3, float 1.0, float %21    ; VTX_Z_FMT ? 1.0 : 1/w0
    %28 = select i1 %3, float 1.0, float %22    ; VTX_Z_FMT ? 1.0 : 1/w1
    %29 = select i1 %3, float 1.0, float %23    ; VTX_Z_FMT ? 1.0 : 1/w2

    %30 = fmul float %9,  %24       ; x0' = x0/w0
    %31 = fmul float %10, %24       ; y0' = y0/w0
    %32 = fmul float %11, %27       ; z0' = z0/w0

    %33 = fmul float %13, %25       ; x1' = x1/w1
    %34 = fmul float %14, %25       ; y1' = y1/w1
    %35 = fmul float %15, %28       ; z1' = z1/w1

    %36 = fmul float %17, %26       ; x2' = x2/w2
    %37 = fmul float %18, %26       ; y2' = y2/w2
    %38 = fmul float %19, %29       ; z2' = z2/w2

    ;
    ; === Step 1 ===: Discard space to -1..1 space.
    ;

    ; x" = x'/xDiscAdj
    ; y" = y'/yDiscAdj
    ; z" = (zNear + 2.0)z' + (-1.0 - zNear)
    %39 = fdiv float 1.0, %7                                            ; 1/xDiscAdj
    %40 = fdiv float 1.0, %8                                            ; 1/yDiscAdj
    %41 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %39, float %40)  ; <1/xDiscAdj, 1/yDiscAdj>

    %42 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %30, float %31)  ; <x0', y0'>
    %43 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %33, float %34)  ; <x1', y1'>
    %44 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %36, float %37)  ; <x2', y2'>

    %45 = fmul <2 x half> %42, %41  ; <x0", y0">
    %46 = fmul <2 x half> %43, %41  ; <x1", y1">
    %47 = fmul <2 x half> %44, %41  ; <x2", y2">

    ; zNear + 2.0
    %48 = fadd float %6, 2.0
    %49 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %48, float %48)

    ; -1.0 - zNear
    %50 = fsub float -1.0, %6
    %51 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %50, float %50)

    %52 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %32, float %32)  ; <z0', z0'>
    %53 = call <2 x half> @llvm.amdgcn.cvt.pkrtz(float %38, float %35)  ; <z2', z1'>

    %54 = call <2 x half> @llvm.fma.v2f16(<2 x half> %49, <2 x half> %52, <2 x half> %51)   ; <z0", z0">
    %55 = call <2 x half> @llvm.fma.v2f16(<2 x half> %49, <2 x half> %53, <2 x half> %51)   ; <z2", z1">

    ;
    ; === Step 2 ===: 3D coordinates to barycentric coordinates.
    ;

    ; <x20, y20> = <x2", y2"> - <x0", y0">
    %56 = fsub <2 x half> %47, %45

    ; <x10, y10> = <x1", y1"> - <x0", y0">
    %57 = fsub <2 x half> %46, %45

    ; <z20, z10> = <z2", z1"> - <z0", z0">
    %58 = fsub <2 x half> %55, %54

    ;
    ; === Step 3 ===: Solve linear system and find the point closest to the origin.
    ;

    ; a00 = x10 + z10
    %59 = extractelement <2 x half> %57, i32 0  ; x10
    %60 = extractelement <2 x half> %58, i32 1  ; z10
    %61 = fadd half %59, %60

    ; a01 = x20 + z20
    %62 = extractelement <2 x half> %56, i32 0  ; x20
    %63 = extractelement <2 x half> %58, i32 0  ; z20
    %64 = fadd half %62, %63

    ; a10 = y10 + y10
    %65 = extractelement <2 x half> %57, i32 1  ; y10
    %66 = fadd half %65, %65

    ; a11 = y20 + z20
    %67 = extractelement <2 x half> %56, i32 1  ; y20
    %68 = fadd half %67, %63

    ; b0 = -x0" - x2"
    %69 = extractelement <2 x half> %45, i32 0  ; x0"
    %70 = fsub half 0.0, %69                    ; -x0"
    %71 = extractelement <2 x half> %47, i32 0  ; x2"
    %72 = fsub half %70, %71

    ; b1 = -x1" - x2"
    %73 = extractelement <2 x half> %46, i32 0  ; x1"
    %74 = fsub half 0.0, %73                    ; -x1"
    %75 = fsub half %74, %71

    ;     [ a00 a01 ]      [ b0 ]       [ s ]
    ; A = [         ], B = [    ], ST = [   ], A * ST = B (crame rules)
    ;     [ a10 a11 ]      [ b1 ]       [ t ]

    ;           | a00 a01 |
    ; det(A) =  |         | = a00 * a11 - a01 * a10
    ;           | a10 a11 |
    %76 = fmul half %61, %68 ; a00 * a11
    %77 = fsub half 0.0, %64 ; -a01
    %78 = call half @llvm.fma.f16(half %77, half %66, half %76)

    ;            | b0 a01 |
    ; det(Ab0) = |        | = b0 * a11 - a01 * b1
    ;            | b1 a11 |
    %79 = fmul half %72, %68 ; b0 * a11
    %80 = call half @llvm.fma.f16(half %77, half %75, half %79)

    ;            | a00 b0 |
    ; det(Ab1) = |        | = a00 * b1 - b0 * a10
    ;            | a10 b1 |
    %81 = fmul half %61, %75 ; a00 * b1
    %82 = fsub half 0.0, %72 ; -b0
    %83 = call half @llvm.fma.f16(half %82, half %66, half %81)

    ; s = det(Ab0) / det(A)
    %84 = fdiv half 1.0, %78
    %85 = fmul half %80, %84

    ; t = det(Ab1) / det(A)
    %86 = fmul half %83, %84

    ;
    ; === Step 4 ===: Do clamping for the closest point.
    ;

    ; <s, t>
    %87 = insertelement <2 x half> undef, half %85, i32 0
    %88 = insertelement <2 x half> %87, half %86, i32 1

    ; <s', t'> = <0.5 - 0.5(t - s), 0.5 + 0.5(t - s)>
    %89 = fsub half %86, %85 ; t - s
    %90 = insertelement <2 x half> undef, half %89, i32 0
    %91 = insertelement <2 x half> %90, half %89, i32 1
    %92 = call <2 x half> @llvm.fma.v2f16(<2 x half> <half -0.5, half 0.5>,
                                          <2 x half> %91,
                                          <2 x half> <half 0.5, half 0.5>)

    ; <s", t"> = clamp(<s, t>)
    %93 = call <2 x half> @llvm.maxnum.v2f16(<2 x half> %88, <2 x half> <half 0.0, half 0.0>)
    %94 = call <2 x half> @llvm.minnum.v2f16(<2 x half> %93, <2 x half> <half 1.0, half 1.0>)

    ; <s, t> = (s + t) > 1.0 ? <s', t'> : <s", t">
    %95 = fadd half %85, %86     ; s + t
    %96 = fcmp ogt half %95, 1.0 ; s + t > 1.0
    %97 = select i1 %96, <2 x half> %92, <2 x half> %94

    ;
    ; === Step 5 ===: Barycentric coordinates to 3D coordinates.
    ;

    ; x = x0" + s * x10 + t * x20
    ; y = y0" + s * y10 + t * y20
    ; z = z0" + s * z10 + t * z20
    %98 = extractelement <2 x half> %97, i32 0 ; s
    %99 = extractelement <2 x half> %97, i32 1 ; t
    %100 = insertelement <2 x half> %97, half %98, i32 1; <s, s>
    %101 = insertelement <2 x half> %97, half %99, i32 0; <t, t>

    ;
    ; === Step 6 ===: Compute the distance squared of the closest point.
    ;

    ; s * <x10, y10> + <x0", y0">
    %102 = call <2 x half> @llvm.fma.v2f16(<2 x half> %100, <2 x half> %57, <2 x half> %45)

    ; <x, y> = t * <x20, y20> + (s * <x10, y10> + <x0", y0">)
    %103 = call <2 x half> @llvm.fma.v2f16(<2 x half> %101, <2 x half> %56, <2 x half> %102)

    ; s * z10 + z0"
    %104 = extractelement <2 x half> %54, i32 0 ; z0"
    %105 = call half @llvm.fma.f16(half %98, half %60, half %104)

    ; z = t * z20 + (s * z10 + z0")
    %106 = call half @llvm.fma.f16(half %99, half %63, half %105)

    %107 = extractelement <2 x half> %103, i32 0 ; x
    %108 = extractelement <2 x half> %103, i32 1 ; y

    ; r^2 = x^2 + y^2 + z^2
    %109 = fmul half %107, %107
    %110 = call half @llvm.fma.f16(half %108, half %108, half %109)
    %111 = call half @llvm.fma.f16(half %106, half %106, half %110)

    ;
    ; === Step 7 ===: Determine the cull flag
    ;

    ; cullFlag = (r^2 > 3.0)
    %112 = fcmp ogt half %111, 3.0

    br label %.sphereExit

.sphereExit:
    %113 = phi i1 [ %cullFlag, %.sphereEntry ], [ %112, %.sphereCull ]
    ret i1 %113
}

; NGG culling: small primitive filter culler
define i1 @llpc.ngg.culling.smallprimfilter(
    i1          %cullFlag,
    <4 x float> %vtx0,
    <4 x float> %vtx1,
    <4 x float> %vtx2,
    i32         %paClVteCntl,
    i32         %paClVportXscale,
    i32         %paClVportYscale
    ) #1
{
.smallprimfilterEntry:
    ; If cull flag has already been TRUE, early return
    br i1 %cullFlag, label %.smallprimfilterExit, label %.smallprimfilterCull

.smallprimfilterCull:
    ;
    ; Small primitive filter culling algorithm is described as follow:
    ;
    ;   if ((floor(min(scaled(x0/w0), scaled(x1/w1), scaled(x2/w2))) ==
    ;        floor(max(scaled(x0/w0), scaled(x1/w1), scaled(x2/w2)))) ||
    ;       (floor(min(scaled(y0/w0), scaled(y1/w1), scaled(y2/w2))) ==
    ;        floor(max(scaled(y0/w0), scaled(y1/w1), scaled(y2/w2)))))
    ;       cullFlag = true
    ;

    ;vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    %0 = call i32 @llvm.amdgcn.ubfe.i32(i32 %paClVteCntl, i32 8, i32 1)
    %1 = trunc i32 %0 to i1

    ; xScale = (VPORT_XSCALE, PA_CL_VPORT_XSCALE[31:0])
    %2 = bitcast i32 %paClVportXscale to float

    ; yScale = (VPORT_YSCALE, PA_CL_VPORT_YSCALE[31:0])
    %3 = bitcast i32 %paClVportYscale to float

    %4  = extractelement <4 x float> %vtx0, i32 0   ; x0
    %5  = extractelement <4 x float> %vtx0, i32 1   ; y0
    %6  = extractelement <4 x float> %vtx0, i32 3   ; w0

    %7  = extractelement <4 x float> %vtx1, i32 0   ; x1
    %8  = extractelement <4 x float> %vtx1, i32 1   ; y1
    %9  = extractelement <4 x float> %vtx1, i32 3   ; w1

    %10 = extractelement <4 x float> %vtx2, i32 0   ; x2
    %11 = extractelement <4 x float> %vtx2, i32 1   ; y2
    %12 = extractelement <4 x float> %vtx2, i32 3   ; w2

    ; Convert xyz coordinate to normalized device coordinate (NDC)
    %13 = fdiv float 1.0, %6    ; 1/w0
    %14 = fdiv float 1.0, %9    ; 1/w1
    %15 = fdiv float 1.0, %12   ; 1/w2

    %16 = select i1 %1, float 1.0, float %13    ; VTX_XY_FMT ? 1.0 : 1/w0
    %17 = select i1 %1, float 1.0, float %14    ; VTX_XY_FMT ? 1.0 : 1/w1
    %18 = select i1 %1, float 1.0, float %15    ; VTX_XY_FMT ? 1.0 : 1/w2

    %19 = fmul float %4, %16       ; x0' = x0/w0
    %20 = fmul float %5, %16       ; y0' = y0/w0

    %21 = fmul float %7, %17       ; x1' = x1/w1
    %22 = fmul float %8, %17       ; y1' = y1/w1

    %23 = fmul float %10, %18      ; x2' = x2/w2
    %24 = fmul float %11, %18      ; y2' = y2/w2

    ; clampX0' = clamp((x0' + 1.0) / 2)
    %25 = fadd float %19, 1.0
    %26 = fmul float %25, 0.5
    %27 = call float @llvm.maxnum.f32(float %26, float 0.0)
    %28 = call float @llvm.minnum.f32(float %27, float 1.0)

    ; scaledX0' = (clampX0' * xScale) * 2
    %29 = fmul float %28, %2
    %30 = fmul float %29, 2.0

    ; clampX1' = clamp((x1' + 1.0) / 2)
    %31 = fadd float %21, 1.0
    %32 = fmul float %31, 0.5
    %33 = call float @llvm.maxnum.f32(float %32, float 0.0)
    %34 = call float @llvm.minnum.f32(float %33, float 1.0)

    ; scaledX1' = (clampX1' * xScale) * 2
    %35 = fmul float %34, %2
    %36 = fmul float %35, 2.0

    ; clampX2' = clamp((x2' + 1.0) / 2)
    %37 = fadd float %23, 1.0
    %38 = fmul float %37, 0.5
    %39 = call float @llvm.maxnum.f32(float %38, float 0.0)
    %40 = call float @llvm.minnum.f32(float %39, float 1.0)

    ; scaledX2' = (clampX2' * xScale) * 2
    %41 = fmul float %40, %2
    %42 = fmul float %41, 2.0

    ; clampY0' = clamp((y0' + 1.0) / 2)
    %43 = fadd float %20, 1.0
    %44 = fmul float %43, 0.5
    %45 = call float @llvm.maxnum.f32(float %44, float 0.0)
    %46 = call float @llvm.minnum.f32(float %45, float 1.0)

    ; scaledY0' = (clampY0' * yScale) * 2
    %47 = fmul float %46, %3
    %48 = fmul float %47, 2.0

    ; clampy1' = clamp((y1' + 1.0) / 2)
    %49 = fadd float %22, 1.0
    %50 = fmul float %49, 0.5
    %51 = call float @llvm.maxnum.f32(float %50, float 0.0)
    %52 = call float @llvm.minnum.f32(float %51, float 1.0)

    ; scaledY1' = (clampY1' * yScale) * 2
    %53 = fmul float %52, %3
    %54 = fmul float %53, 2.0

    ; clampY2' = clamp((y2' + 1.0) / 2)
    %55 = fadd float %24, 1.0
    %56 = fmul float %55, 0.5
    %57 = call float @llvm.maxnum.f32(float %56, float 0.0)
    %58 = call float @llvm.minnum.f32(float %57, float 1.0)

    ; scaledY2' = (clampY2' * yScale) * 2
    %59 = fmul float %58, %3
    %60 = fmul float %59, 2.0

    ; minX = floor(min(scaledX0', scaledX1', scaledX2') - 0.000001)
    %61 = call float @llpc.fmin3.f32(float %30, float %36, float %42)
    %62 = fadd float %61, 0xBEB0C6F7A0000000
    %63 = call float @llvm.floor.f32(float %62)

    ; maxX = floor(max(scaledX0', scaledX1', scaledX2') + 0.000001)
    %64 = call float @llpc.fmax3.f32(float %30, float %36, float %42)
    %65 = fadd float %64, 0x3EB0C6F7A0000000
    %66 = call float @llvm.floor.f32(float %65)

    ; minY = floor(min(scaledY0', scaledY1', scaledY2') - 0.000001)
    %67 = call float @llpc.fmin3.f32(float %48, float %54, float %60)
    %68 = fadd float %67, 0xBEB0C6F7A0000000
    %69 = call float @llvm.floor.f32(float %68)

    ; maxX = floor(max(scaledY0', scaledY1', scaledY2') + 0.000001)
    %70 = call float @llpc.fmax3.f32(float %48, float %54, float %60)
    %71 = fadd float %70, 0x3EB0C6F7A0000000
    %72 = call float @llvm.floor.f32(float %71)

    ; minX == maxX
    %73 = fcmp oeq float %63, %66

    ; minY == maxY
    %74 = fcmp oeq float %69, %72

    ; Get cull flag
    %75 = or i1 %73, %74

    br label %.smallprimfilterExit

.smallprimfilterExit:
    %76 = phi i1 [ %cullFlag, %.smallprimfilterEntry ], [ %75, %.smallprimfilterCull ]
    ret i1 %76
}

; NGG culling: cull distance culler
define i1 @llpc.ngg.culling.culldistance(
    i1  %cullFlag,
    i32 %signMask0,
    i32 %signMask1,
    i32 %signMask2
    ) #1
{
.culldistanceEntry:
    ; If cull flag has already been TRUE, early return
    br i1 %cullFlag, label %.culldistanceExit, label %.culldistanceCull

.culldistanceCull:
    ;
    ; Cull distance culling algorithm is described as follow:
    ;
    ;   vertexSignMask[7:0] = [sign(ClipDistance[0])..sign(ClipDistance[7])]
    ;   primSignMask = vertexSignMask0 & vertexSignMask1 & vertexSignMask2
    ;   cullFlag = (primSignMask != 0)
    ;

    %0 = and i32 %signMask0, %signMask1
    %1 = and i32 %0, %signMask2
    %2 = icmp ne i32 %1, 0

    br label %.culldistanceExit

.culldistanceExit:
    %3 = phi i1 [ %cullFlag, %.culldistanceEntry ], [ %2, %.culldistanceCull ]
    ret i1 %3
}

declare float @llvm.fabs.f32(float) #0
declare float @llvm.powi.f32(float, i32) #0
declare i32 @llvm.amdgcn.ubfe.i32(i32, i32, i32) #1
declare float @llpc.fmin3.f32(float, float, float) #0
declare float @llpc.fmax3.f32(float, float, float) #0
declare float @llvm.maxnum.f32(float, float) #0
declare float @llvm.minnum.f32(float, float) #0
declare <2 x half> @llvm.maxnum.v2f16(<2 x half>, <2 x half>) #0
declare <2 x half> @llvm.minnum.v2f16(<2 x half>, <2 x half>) #0
declare float @llvm.floor.f32(float) #0
declare <2 x half> @llvm.amdgcn.cvt.pkrtz(float, float) #1
declare half @llvm.fma.f16(half, half, half) #0
declare <2 x half> @llvm.fma.v2f16(<2 x half>, <2 x half>, <2 x half>) #0

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }

!0 = !{}
