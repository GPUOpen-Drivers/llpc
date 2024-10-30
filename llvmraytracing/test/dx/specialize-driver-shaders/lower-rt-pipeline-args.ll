; RUN: opt --verify-each -passes="dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,sroa,specialize-driver-shaders,lint,remove-types-metadata" -S --lint-abort-on-error -debug-only='specialize-driver-shaders' %s 2>&1 | FileCheck %s
;
; Test that argument layouts (number of ignored arguments) expected in specialize-driver-shaders matches what lower-raytracing-pipeline does.
; Intentionally only test non-lgc.cps-mode, as lgc.cps mode requires different arguments in test IR,
; and as it is already tested as part of an LLPC offline pipeline compilation test.
;
; REQUIRES: assertions

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { <3 x i32> }
%struct.TraversalData = type { %struct.SystemData, %struct.HitData, <3 x float>, <3 x float>, float, i64 }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.HitData = type { <3 x float>, <3 x float>, float, i32 }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.RayPayload = type { <4 x float> }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.BuiltInTriangleIntersectionAttributes2 = type { <2 x float> }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

define i32 @_cont_GetContinuationStackAddr() #0 {
  ret i32 0
}

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !{%struct.DispatchSystemData poison} {
  ret void
}

declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #0

declare %struct.DispatchSystemData @_AmdAwaitShader(i64, i64, %struct.DispatchSystemData) #0

declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, i64, %struct.AnyHitTraversalData, float, i32) #0

define %struct.HitData @_cont_GetCandidateState(%struct.AnyHitTraversalData* %data) #0 !pointeetys !32 {
  %resPtr = getelementptr %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, i32 0, i32 0
  %res = load %struct.HitData, %struct.HitData* %resPtr, align 4
  ret %struct.HitData %res
}

declare !pointeetys !34 %struct.HitData @_cont_GetCommittedState(%struct.SystemData*) #0

declare !pointeetys !36 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

define void @_cont_SetTriangleHitAttributes(%struct.SystemData* %data, %struct.BuiltInTriangleIntersectionAttributes %val) !pointeetys !37 {
  %addr = getelementptr %struct.SystemData, %struct.SystemData* %data, i32 0, i32 0
  store %struct.BuiltInTriangleIntersectionAttributes %val, %struct.BuiltInTriangleIntersectionAttributes* %addr, align 4
  ret void
}

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) !pointeetys !38 {
  ret i32 5
}

declare i1 @opaqueIsEnd()

define i1 @_cont_IsEndSearch(%struct.TraversalData*) #0 !pointeetys !40 {
  %isEnd = call i1 @opaqueIsEnd()
  ret i1 %isEnd
}

declare !pointeetys !42 i32 @_cont_HitKind(%struct.SystemData*) #0

; Function Attrs: nounwind
declare i64 @_AmdGetResumePointAddr() #1

; Function Attrs: nounwind
declare !pointeetys !43 void @_AmdRestoreSystemData(%struct.DispatchSystemData*) #1

; Function Attrs: nounwind
declare !pointeetys !44 void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData*) #1

; Function Attrs: nounwind
declare !pointeetys !43 void @_cont_AcceptHitAndEndSearch(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind
declare !pointeetys !44 void @_cont_AcceptHit(%struct.AnyHitTraversalData* nocapture readnone) #1

; Function Attrs: nounwind
declare !pointeetys !43 void @_cont_IgnoreHit(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind
declare !pointeetys !44 void @_AmdAcceptHitAttributes(%struct.AnyHitTraversalData* nocapture readnone) #1

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #0 !pointeetys !45 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData zeroinitializer, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData zeroinitializer, %struct.SystemData %sys_data, 0
  %addr = call i64 @_AmdGetResumePointAddr() #3
  %trav_data2 = insertvalue %struct.TraversalData %trav_data, i64 %addr, 5
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data2)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) #0 !pointeetys !46 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, i64 poison, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) #0 !pointeetys !47 {
  %origTPtr = getelementptr inbounds %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, i32 0, i32 0, i32 4
  %origT = load float, float* %origTPtr, align 4
  %isNoHit = fcmp fast uge float %t, %origT
  br i1 %isNoHit, label %isEnd, label %callAHit

callAHit:                                         ; preds = %0
  %trav_data = load %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, align 4
  %newdata = call %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64 3, i64 poison, %struct.AnyHitTraversalData %trav_data, float %t, i32 %hitKind)
  store %struct.AnyHitTraversalData %newdata, %struct.AnyHitTraversalData* %data, align 4
  call void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData* %data)
  ret i1 true

isEnd:                                            ; preds = %0
  ; Call AcceptHitAttributes, just to simulate it
  call void @_AmdAcceptHitAttributes(%struct.AnyHitTraversalData* %data)
  ret i1 false
}

define <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData* %data) !pointeetys !48 {
  %resPtr.1 = getelementptr %struct.DispatchSystemData, %struct.DispatchSystemData* %data, i32 0, i32 0, i32 0
  %res.1 = load i32, i32* %resPtr.1, align 4
  %resPtr.2 = getelementptr %struct.DispatchSystemData, %struct.DispatchSystemData* %data, i32 0, i32 0, i32 1
  %res.2 = load i32, i32* %resPtr.2, align 4
  %resPtr.3 = getelementptr %struct.DispatchSystemData, %struct.DispatchSystemData* %data, i32 0, i32 0, i32 2
  %res.3 = load i32, i32* %resPtr.3, align 4
  %val.0 = insertelement <3 x i32> undef, i32 %res.1, i32 0
  %val.1 = insertelement <3 x i32> %val.0, i32 %res.2, i32 1
  %val.2 = insertelement <3 x i32> %val.1, i32 %res.3, i32 2
  ret <3 x i32> %val.2
}

define <3 x float> @_cont_ObjectRayOrigin3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData) !pointeetys !49 {
  %resPtr.1 = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 0, i32 0
  %res.1 = load float, float* %resPtr.1, align 4
  %resPtr.2 = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 0, i32 1
  %res.2 = load float, float* %resPtr.2, align 4
  %resPtr.3 = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 0, i32 2
  %res.3 = load float, float* %resPtr.3, align 4
  %val.0 = insertelement <3 x float> undef, float %res.1, i32 0
  %val.1 = insertelement <3 x float> %val.0, float %res.2, i32 1
  %val.2 = insertelement <3 x float> %val.1, float %res.3, i32 2
  ret <3 x float> %val.2
}

define <3 x float> @_cont_ObjectRayDirection3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData) !pointeetys !49 {
  %resPtr.1 = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 1, i32 0
  %res.1 = load float, float* %resPtr.1, align 4
  %resPtr.2 = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 1, i32 1
  %res.2 = load float, float* %resPtr.2, align 4
  %resPtr.3 = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 1, i32 2
  %res.3 = load float, float* %resPtr.3, align 4
  %val.0 = insertelement <3 x float> undef, float %res.1, i32 0
  %val.1 = insertelement <3 x float> %val.0, float %res.2, i32 1
  %val.2 = insertelement <3 x float> %val.1, float %res.3, i32 2
  ret <3 x float> %val.2
}

define float @_cont_RayTCurrent(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData) !pointeetys !51 {
  %resPtr = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 2
  %res = load float, float* %resPtr, align 4
  ret float %res
}

; RayGen: In this test case, we have mostly constant system data (_cont_Traceray uses zero-initialized traversal system data),
;         undef padding for the candidate, and constant payload. The storage for committed hit attributes
;         within the payload storage is undef as well.
;         Note that the dispatch system data (passed in the first args) is dynamic although it preserves an
;         argument incoming to RayGen. This is because we only allow arg preservation *within* Traversal.
; CHECK-LABEL: [SDS] Finished analysis of function MyRayGen
; CHECK-NEXT:  [SDS] 0         1         2         3         4     {{$}}
; CHECK-NEXT:  [SDS] 0123456789012345678901234567890123456789012345{{$}}
; CHECK-NEXT:  [SDS] DDDCCCCCCCCCCCCCCCDDUUUUUUUUUUUUUUUUCUUUUUUCCC{{$}}
;                    ^^^ dynamic dispatch system data
;                       ^^^^^^^^^^^^^^^ constant ray
;                                      ^^ dynamic raygen.resume return addr
;                                        ^^^^^^^^^^^^^^^^ undef candidate
;                                                        ^      ^^^ constant payload
;                                                         ^^^^^^ undef committed attrs
define void @MyRayGen() #2 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = bitcast %struct.RayPayload* %3 to i8*
  call void @llvm.lifetime.start.p0i8(i64 16, i8* %4) #1
  %5 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  store <4 x float> zeroinitializer, <4 x float>* %5, align 4, !tbaa !52
  %6 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)
  %7 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %6, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %7, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)
  %8 = load <4 x float>, <4 x float>* %5, align 4, !tbaa !52
  %9 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)
  %10 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 1)
  %11 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %2)
  %12 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %11, %dx.types.ResourceProperties { i32 4098, i32 1033 })
  %13 = extractelement <4 x float> %8, i64 0
  %14 = extractelement <4 x float> %8, i64 1
  %15 = extractelement <4 x float> %8, i64 2
  %16 = extractelement <4 x float> %8, i64 3
  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %12, i32 %9, i32 %10, i32 undef, float %13, float %14, float %15, float %16, i8 15)
  call void @llvm.lifetime.end.p0i8(i64 16, i8* %4) #1
  ret void
}

; Non-recursive CHS: No calls to Traversal, so no state to report.
; CHECK-LABEL: [SDS] Finished analysis of function MyClosestHitShader
; CHECK-NEXT:  [SDS] <empty>
define void @MyClosestHitShader(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #2 !pointeetys !55 {
  %1 = getelementptr inbounds %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %attr, i32 0, i32 0
  %2 = load <2 x float>, <2 x float>* %1, align 4
  %3 = extractelement <2 x float> %2, i32 0
  %4 = fsub fast float 1.000000e+00, %3
  %5 = extractelement <2 x float> %2, i32 1
  %6 = fsub fast float %4, %5
  %7 = insertelement <4 x float> undef, float %6, i64 0
  %8 = insertelement <4 x float> %7, float %3, i64 1
  %9 = insertelement <4 x float> %8, float %5, i64 2
  %10 = insertelement <4 x float> %9, float 1.000000e+00, i64 3
  %11 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  store <4 x float> %10, <4 x float>* %11, align 4
  ret void
}

; AnyHit: Payload and committed hit attrs are preserved.
; CHECK-LABEL: [SDS] Finished analysis of function MyAnyHitShader
; CHECK-NEXT:  [SDS] 0         1         2         3         4     {{$}}
; CHECK-NEXT:  [SDS] 0123456789012345678901234567890123456789012345{{$}}
; CHECK-NEXT:  [SDS] DDDDDDDDDDDDDDDDDDDDDDDDDDDDUUUUUUUUPPPPPPPPPP{{$}}
define void @MyAnyHitShader(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readnone %attr) #2 !pointeetys !55 {
  %1 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  %2 = load <4 x float>, <4 x float>* %1, align 4
  %3 = call float @dx.op.objectRayOrigin.f32(i32 149, i8 0)
  %4 = call float @dx.op.objectRayDirection.f32(i32 150, i8 0)
  %5 = call float @dx.op.rayTCurrent.f32(i32 154)
  %6 = fmul fast float %5, %4
  %7 = fadd fast float %6, %3
  %8 = fcmp fast ogt float %7, 0.000000e+00
  %9 = fcmp fast ogt float %7, 1.000000e+00
  %10 = fcmp fast ogt float %7, -1.000000e+00
  br i1 %8, label %11, label %14

11:                                               ; preds = %0
; acceptHitAndEndSearch
  store <4 x float> %2, <4 x float>* %1, align 4
  br i1 %9, label %12, label %13

12:                                               ; preds = %11
; acceptHitAndEndSearch with unreachable
  call void @dx.op.acceptHitAndEndSearch(i32 156)
  unreachable

13:                                               ; preds = %11
; acceptHitAndEndSearch with ret void
  call void @dx.op.acceptHitAndEndSearch(i32 156)
  ret void

14:                                               ; preds = %0
; IgnoreHit or AcceptHit
  br i1 %10, label %15, label %18

15:                                               ; preds = %14
; IgnoreHit
  br i1 %9, label %16, label %17

16:                                               ; preds = %15
; IgnoreHit with unreachable
  call void @dx.op.ignoreHit(i32 155)
  unreachable

17:                                               ; preds = %15
; IgnoreHit with ret void (as emitted by debug mode dxc)
  call void @dx.op.ignoreHit(i32 155)
  ret void

18:                                               ; preds = %14
; AcceptHit
  store <4 x float> %2, <4 x float>* %1, align 4
  ret void
}

; Intersection: The payload is preserved, even across ReportHit calls.
; Six Argument slots unused by the small hit attributes are undef.
; CHECK-LABEL: [SDS] Finished analysis of function MyIntersectionShader
; CHECK-NEXT:  [SDS] 0         1         2         3         4         5         6     {{$}}
; CHECK-NEXT:  [SDS] 012345678901234567890123456789012345678901234567890123456789012345{{$}}
; CHECK-NEXT:  [SDS] DDDPPPPPPPPPPPPPPPPPPPPPPPPPDCUUUUUUPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
define void @MyIntersectionShader() #2 {
  %1 = alloca %struct.BuiltInTriangleIntersectionAttributes, align 4
  %2 = call float @dx.op.rayTCurrent.f32(i32 154)
  %3 = bitcast %struct.BuiltInTriangleIntersectionAttributes* %1 to i8*
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %3) #1
  %4 = call i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32 158, float %2, i32 0, %struct.BuiltInTriangleIntersectionAttributes* nonnull %1)
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %3) #1
  ret void
}

; Intersection with ReportHit in a loop: The analysis doesn't understand that the payload is preserved,
; because we don't repeatedly propagate through loops. This could be improved in ValueOriginTracking.
; CHECK-LABEL: [SDS] Finished analysis of function MyIntersectionShaderLoop
; CHECK-NEXT:  [SDS] 0         1         2         3         4         5         6     {{$}}
; CHECK-NEXT:  [SDS] 012345678901234567890123456789012345678901234567890123456789012345{{$}}
; CHECK-NEXT:  [SDS] DDDDDDDDDDDDDDDDDDDDDDDDDDDDDCUUUUUUDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD{{$}}
define void @MyIntersectionShaderLoop() #2 {
  %1 = alloca %struct.BuiltInTriangleIntersectionAttributes, align 4
  %2 = call float @dx.op.rayTCurrent.f32(i32 154)
  %3 = bitcast %struct.BuiltInTriangleIntersectionAttributes* %1 to i8*
  br label %loop
loop:
  %4 = call i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32 158, float %2, i32 0, %struct.BuiltInTriangleIntersectionAttributes* nonnull %1)
  br i1 %4, label %loop, label %exit
exit:
  ret void
}

; Non-recursive Miss: No calls to Traversal, so no state to report.
; CHECK-LABEL: [SDS] Finished analysis of function MyMissShader
; CHECK-NEXT:  [SDS] <empty>
define void @MyMissShader(%struct.RayPayload* noalias nocapture %payload) #2 !pointeetys !58 {
  %1 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  store <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float>* %1, align 4
  ret void
}

; Recursive Miss: The passes through the incoming payload to traceRay, but it's treated as dynamic because miss is outside of Traversal.
; CHECK-LABEL: [SDS] Finished analysis of function MyMissShaderRecursive
; CHECK-NEXT:  [SDS] 0         1         2         3         4     {{$}}
; CHECK-NEXT:  [SDS] 0123456789012345678901234567890123456789012345{{$}}
; CHECK-NEXT:  [SDS] DDDCCCCCCCCCCCCCCCDDUUUUUUUUUUUUUUUUDDDDDDDDDD{{$}}
define void @MyMissShaderRecursive(%struct.RayPayload* noalias nocapture %payload) #2 !pointeetys !58 {
  %tmp1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %tmp6 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %tmp1)
  %tmp7 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tmp6, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %tmp7, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %payload)
  ret void
}

; Function Attrs: nounwind
declare !pointeetys !59 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #1

; Function Attrs: nounwind
declare void @dx.op.textureStore.f32(i32, %dx.types.Handle, i32, i32, i32, float, float, float, float, i8) #1

; Function Attrs: nounwind memory(none)
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #3

; Function Attrs: nounwind memory(none)
declare float @dx.op.objectRayDirection.f32(i32, i8) #3

; Function Attrs: nounwind memory(none)
declare float @dx.op.objectRayOrigin.f32(i32, i8) #3

; Function Attrs: nounwind memory(read)
declare float @dx.op.rayTCurrent.f32(i32) #4

declare void @dx.op.acceptHitAndEndSearch(i32) #0

declare void @dx.op.ignoreHit(i32) #0

; Function Attrs: nounwind
declare !pointeetys !60 i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes*) #1

; Function Attrs: nounwind
declare !pointeetys !61 i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes2(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes2*) #1

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #3

; Function Attrs: nounwind memory(read)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #4

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !pointeetys !63 void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #5

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !pointeetys !63 void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #5

attributes #0 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind }
attributes #2 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #3 = { nounwind memory(none) }
attributes #4 = { nounwind memory(read) }
attributes #5 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.typeAnnotations = !{!10}
!dx.entryPoints = !{!18, !20, !23, !25, !27, !29, !31, !65}

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
!1 = !{i32 1, i32 6}
!2 = !{!"lib", i32 6, i32 6}
!3 = !{!4, !7, null, null}
!4 = !{!5}
!5 = !{i32 0, %struct.RaytracingAccelerationStructure* bitcast (%dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A" to %struct.RaytracingAccelerationStructure*), !"Scene", i32 0, i32 0, i32 1, i32 16, i32 0, !6}
!6 = !{i32 0, i32 4}
!7 = !{!8}
!8 = !{i32 0, %"class.RWTexture2D<vector<float, 4> >"* bitcast (%dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" to %"class.RWTexture2D<vector<float, 4> >"*), !"RenderTarget", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !9}
!9 = !{i32 0, i32 9}
!10 = !{i32 1, void ()* @MyRayGen, !11, void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @MyClosestHitShader, !14, void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @MyAnyHitShader, !14, void ()* @MyIntersectionShader, !11, void ()* @MyIntersectionShaderLoop, !11, void (%struct.RayPayload*)* @MyMissShader, !17}
!11 = !{!12}
!12 = !{i32 1, !13, !13}
!13 = !{}
!14 = !{!12, !15, !16}
!15 = !{i32 2, !13, !13}
!16 = !{i32 0, !13, !13}
!17 = !{!12, !15}
!18 = !{null, !"", null, !3, !19}
!19 = !{i32 0, i64 65536}
!20 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @MyAnyHitShader, !"MyAnyHitShader", null, null, !21}
!21 = !{i32 8, i32 9, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!22 = !{i32 0}
!23 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @MyClosestHitShader, !"MyClosestHitShader", null, null, !24}
!24 = !{i32 8, i32 10, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!25 = !{void ()* @MyIntersectionShader, !"MyIntersectionShader", null, null, !26}
!26 = !{i32 8, i32 8, i32 5, !22}
!27 = !{void (%struct.RayPayload*)* @MyMissShader, !"MyMissShader", null, null, !28}
!28 = !{i32 8, i32 11, i32 6, i32 16, i32 5, !22}
!29 = !{void ()* @MyRayGen, !"MyRayGen", null, null, !30}
!30 = !{i32 8, i32 7, i32 5, !22}
!31 = !{void ()* @MyIntersectionShaderLoop, !"MyIntersectionShaderLoop", null, null, !26}
!32 = !{%struct.AnyHitTraversalData poison}
!33 = !{i32 0, %struct.AnyHitTraversalData poison}
!34 = !{%struct.SystemData poison}
!35 = !{i32 0, %struct.SystemData poison}
!36 = !{%struct.SystemData poison}
!37 = !{%struct.SystemData poison}
!38 = !{%struct.DispatchSystemData poison}
!39 = !{i32 0, %struct.DispatchSystemData poison}
!40 = !{%struct.TraversalData poison}
!41 = !{i32 0, %struct.TraversalData poison}
!42 = !{%struct.SystemData poison}
!43 = !{%struct.DispatchSystemData poison}
!44 = !{%struct.AnyHitTraversalData poison}
!45 = !{%struct.DispatchSystemData poison}
!46 = !{%struct.DispatchSystemData poison}
!47 = !{%struct.AnyHitTraversalData poison}
!48 = !{%struct.DispatchSystemData poison}
!49 = !{null, %struct.DispatchSystemData poison, %struct.HitData poison}
!50 = !{i32 0, %struct.HitData poison}
!51 = !{null, %struct.DispatchSystemData poison, %struct.HitData poison}
!52 = !{!53, !53, i64 0}
!53 = !{!"omnipotent char", !54, i64 0}
!54 = !{!"Simple C/C++ TBAA"}
!55 = !{null, %struct.RayPayload poison, %struct.BuiltInTriangleIntersectionAttributes poison}
!56 = !{i32 0, %struct.RayPayload poison}
!57 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
!58 = !{%struct.RayPayload poison}
!59 = !{%struct.RayPayload poison}
!60 = !{%struct.BuiltInTriangleIntersectionAttributes poison}
!61 = !{%struct.BuiltInTriangleIntersectionAttributes2 poison}
!62 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes2 poison}
!63 = !{i8 poison}
!64 = !{i32 0, i8 poison}
!65 = !{void (%struct.RayPayload*)* @MyMissShaderRecursive, !"MyMissShaderRecursive", null, null, !28}
