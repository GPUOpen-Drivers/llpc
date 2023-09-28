; Test that multiple copies of TraceRay are generated for the same payload type
; if required by e.g. different shader configs (max hit attr size).
; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S %s 2>%t.stderr | FileCheck %s
; RUN: count 0 < %t.stderr

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { <3 x i32> }
%struct.TraversalData = type { %struct.SystemData, %struct.HitData, <3 x float>, <3 x float>, float, i64 }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.HitData = type { <3 x float>, <3 x float>, float, i32 }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.RayPayload = type { <4 x float> }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

declare i32 @_cont_GetContinuationStackAddr() #0

declare %struct.DispatchSystemData @_cont_SetupRayGen() #0

declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #0

declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #0

declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #0

declare !types !16 %struct.HitData @_cont_GetCandidateState(%struct.AnyHitTraversalData*) #0

declare !types !18 %struct.HitData @_cont_GetCommittedState(%struct.SystemData*) #0

declare !types !20 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

declare !types !21 void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #0

declare !types !22 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)

declare !types !24 i1 @_cont_IsEndSearch(%struct.TraversalData*) #0

declare !types !26 i32 @_cont_HitKind(%struct.SystemData*) #0

; Function Attrs: nounwind
declare i64 @_AmdGetResumePointAddr() #1

; Function Attrs: nounwind
declare !types !27 void @_AmdRestoreSystemData(%struct.DispatchSystemData*) #1

; Function Attrs: nounwind
declare !types !28 void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData*) #1

; Function Attrs: nounwind
declare !types !27 void @_cont_AcceptHitAndEndSearch(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind
declare !types !28 void @_cont_AcceptHit(%struct.AnyHitTraversalData* nocapture readnone) #1

; Function Attrs: nounwind
declare !types !27 void @_cont_IgnoreHit(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind
declare !types !28 void @_AmdAcceptHitAttributes(%struct.AnyHitTraversalData* nocapture readnone) #1

; CHECK: define{{.*}}_cont_TraceRay.struct.RayPayload.attr_max_24_bytes(
; CHECK: define{{.*}}_cont_TraceRay.struct.RayPayload.attr_max_8_bytes(
define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) !types !29 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %addr = call i64 @_AmdGetResumePointAddr() #3
  %trav_data2 = insertvalue %struct.TraversalData %trav_data, i64 %addr, 5
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data2)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) #0 !types !30 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) !types !31 {
  %origTPtr = getelementptr inbounds %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, i32 0, i32 0, i32 4
  %origT = load float, float* %origTPtr, align 4
  %isNoHit = fcmp fast uge float %t, %origT
  br i1 %isNoHit, label %isEnd, label %callAHit

callAHit:                                         ; preds = %0
  %trav_data = load %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, align 4
  %newdata = call %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64 3, %struct.AnyHitTraversalData %trav_data, float %t, i32 %hitKind)
  store %struct.AnyHitTraversalData %newdata, %struct.AnyHitTraversalData* %data, align 4
  call void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData* %data)
  ret i1 true

isEnd:                                            ; preds = %0
  call void @_AmdAcceptHitAttributes(%struct.AnyHitTraversalData* %data)
  ret i1 false
}

define i32 @_cont_DispatchRaysIndex(%struct.DispatchSystemData* %data, i32 %i) !types !32 {
  %resPtr = getelementptr %struct.DispatchSystemData, %struct.DispatchSystemData* %data, i32 0, i32 0, i32 %i
  %res = load i32, i32* %resPtr, align 4
  ret i32 %res
}

define float @_cont_ObjectRayOrigin(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData, i32 %i) !types !33 {
  %resPtr = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 0, i32 %i
  %res = load float, float* %resPtr, align 4
  ret float %res
}

define float @_cont_ObjectRayDirection(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData, i32 %i) !types !33 {
  %resPtr = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 1, i32 %i
  %res = load float, float* %resPtr, align 4
  ret float %res
}

define float @_cont_RayTCurrent(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData) !types !35 {
  %resPtr = getelementptr %struct.HitData, %struct.HitData* %hitData, i32 0, i32 2
  %res = load float, float* %resPtr, align 4
  ret float %res
}

define void @MyRayGen8() !continuation.maxHitAttributeBytes !36 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = bitcast %struct.RayPayload* %3 to i8*
  call void @llvm.lifetime.start.p0i8(i64 16, i8* %4) #1
  %5 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  store <4 x float> zeroinitializer, <4 x float>* %5, align 4, !tbaa !37
  %6 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)
  %7 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %6, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %7, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)
  %8 = load <4 x float>, <4 x float>* %5, align 4, !tbaa !37
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

define void @MyRayGen24() !continuation.maxHitAttributeBytes !40 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = bitcast %struct.RayPayload* %3 to i8*
  call void @llvm.lifetime.start.p0i8(i64 16, i8* %4) #1
  %5 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  store <4 x float> zeroinitializer, <4 x float>* %5, align 4, !tbaa !37
  %6 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)
  %7 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %6, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %7, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)
  %8 = load <4 x float>, <4 x float>* %5, align 4, !tbaa !37
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

; Function Attrs: nounwind
define void @MyClosestHitShader(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #2 !types !41 {
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

; Function Attrs: nounwind
define void @MyAnyHitShader(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readnone %attr) #2 !types !41 {
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
  store <4 x float> %2, <4 x float>* %1, align 4
  br i1 %9, label %12, label %13

12:                                               ; preds = %11
  call void @dx.op.acceptHitAndEndSearch(i32 156)
  unreachable

13:                                               ; preds = %11
  call void @dx.op.acceptHitAndEndSearch(i32 156)
  ret void

14:                                               ; preds = %0
  br i1 %10, label %15, label %18

15:                                               ; preds = %14
  br i1 %9, label %16, label %17

16:                                               ; preds = %15
  call void @dx.op.ignoreHit(i32 155)
  unreachable

17:                                               ; preds = %15
  call void @dx.op.ignoreHit(i32 155)
  ret void

18:                                               ; preds = %14
  store <4 x float> %2, <4 x float>* %1, align 4
  ret void
}

; Function Attrs: nounwind
declare !types !44 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #1

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
declare !types !45 i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes*) #1

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #3

; Function Attrs: nounwind memory(read)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #4

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !types !46 void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #5

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !types !46 void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #5

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
!dx.entryPoints = !{!10, !12, !15}

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
!10 = !{null, !"", null, !3, !11}
!11 = !{i32 0, i64 65536}
!12 = !{void ()* @MyRayGen24, !"MyRayGen24", null, null, !13}
!13 = !{i32 8, i32 7, i32 5, !14}
!14 = !{i32 0}
!15 = !{void ()* @MyRayGen8, !"MyRayGen8", null, null, !13}
!16 = !{!"function", %struct.HitData poison, !17}
!17 = !{i32 0, %struct.AnyHitTraversalData poison}
!18 = !{!"function", %struct.HitData poison, !19}
!19 = !{i32 0, %struct.SystemData poison}
!20 = !{!"function", %struct.BuiltInTriangleIntersectionAttributes poison, !19}
!21 = !{!"function", !"void", !19, %struct.BuiltInTriangleIntersectionAttributes poison}
!22 = !{!"function", !"i32", !23}
!23 = !{i32 0, %struct.DispatchSystemData poison}
!24 = !{!"function", i1 poison, !25}
!25 = !{i32 0, %struct.TraversalData poison}
!26 = !{!"function", !"i32", !19}
!27 = !{!"function", !"void", !23}
!28 = !{!"function", !"void", !17}
!29 = !{!"function", !"void", !23, i64 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison}
!30 = !{!"function", !"void", !23, i32 poison}
!31 = !{!"function", !"i1", !17, float poison, i32 poison}
!32 = !{!"function", i32 poison, !23, i32 poison}
!33 = !{!"function", float poison, !23, !34, i32 poison}
!34 = !{i32 0, %struct.HitData poison}
!35 = !{!"function", float poison, !23, !34}
!36 = !{i32 8}
!37 = !{!38, !38, i64 0}
!38 = !{!"omnipotent char", !39, i64 0}
!39 = !{!"Simple C/C++ TBAA"}
!40 = !{i32 24}
!41 = !{!"function", !"void", !42, !43}
!42 = !{i32 0, %struct.RayPayload poison}
!43 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
!44 = !{!"function", !"void", i32 poison, %dx.types.Handle poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, !42}
!45 = !{!"function", !"void", i32 poison, float poison, i32 poison, !43}
!46 = !{!"function", !"void", i64 poison, !47}
!47 = !{i32 0, i8 poison}
