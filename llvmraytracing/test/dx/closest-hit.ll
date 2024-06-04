; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S %s --lint-abort-on-error | FileCheck -check-prefix=LOWERRAYTRACINGPIPELINE %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.DispatchSystemData = type { <3 x i32> }
%struct.TraversalData = type { %struct.SystemData, %struct.HitData, <3 x float>, <3 x float>, float }
%struct.HitData = type { float, i32 }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.RayPayload = type { <2 x float> }

declare i64 @_cont_GetTraversalAddr() #0

declare i32 @_cont_GetContinuationStackAddr() #0

declare !types !9 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

declare !types !11 void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #0

declare !types !12 i1 @_cont_IsEndSearch(%struct.TraversalData*) #0

declare %struct.DispatchSystemData @_cont_Traversal(%struct.TraversalData) #0

declare %struct.DispatchSystemData @_cont_SetupRayGen() #0

declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #0

declare !types !14 %struct.HitData @_cont_GetCandidateState(%struct.AnyHitTraversalData*) #0

declare !types !16 %struct.HitData @_cont_GetCommittedState(%struct.SystemData*) #0

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #0 !types !17 {
; LOWERRAYTRACINGPIPELINE-LABEL: define i32 @_cont_GetLocalRootIndex(
; LOWERRAYTRACINGPIPELINE-SAME: ptr [[DATA:%.*]]) #[[ATTR0:[0-9]+]] {
; LOWERRAYTRACINGPIPELINE-NEXT:    ret i32 5
;
  ret i32 5
}

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #0 !types !19 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_cont_Traversal(%struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  ret void
}

define i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) #0 !types !20 {
  %trav_data = load %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, align 4
  %newdata = call %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64 3, %struct.AnyHitTraversalData %trav_data, float %t, i32 %hitKind)
  store %struct.AnyHitTraversalData %newdata, %struct.AnyHitTraversalData* %data, align 4
  ret i1 true
}

; Function Attrs: nounwind memory(none)
declare !types !21 i32 @_cont_DispatchRaysIndex(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !21 i32 @_cont_DispatchRaysDimensions(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !22 float @_cont_WorldRayOrigin(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !22 float @_cont_WorldRayDirection(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !23 float @_cont_RayTMin(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind memory(read)
declare !types !24 float @_cont_RayTCurrent(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #2

; Function Attrs: nounwind memory(none)
declare !types !17 i32 @_cont_RayFlags(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind memory(none)
declare !types !26 i32 @_cont_InstanceIndex(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !types !26 i32 @_cont_InstanceID(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !types !26 i32 @_cont_PrimitiveIndex(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !types !27 float @_cont_ObjectRayOrigin(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !27 float @_cont_ObjectRayDirection(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !28 float @_cont_ObjectToWorld(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !28 float @_cont_WorldToObject(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32, i32) #1

; Function Attrs: nounwind memory(none)
declare !types !29 i32 @_cont_HitKind(%struct.SystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind
define void @ClosestHit(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #3 !types !30 {
; LOWERRAYTRACINGPIPELINE-LABEL: define %struct.DispatchSystemData @ClosestHit(
; LOWERRAYTRACINGPIPELINE-SAME: i64 [[RETURNADDR:%.*]], [[STRUCT_SYSTEMDATA:%.*]] [[TMP0:%.*]]) #[[ATTR4:[0-9]+]] !lgc.rt.shaderstage [[META13:![0-9]+]] !continuation [[META14:![0-9]+]] !continuation.registercount [[META10:![0-9]+]] {
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP2:%.*]] = alloca [[STRUCT_BUILTINTRIANGLEINTERSECTIONATTRIBUTES:%.*]], align 8
; LOWERRAYTRACINGPIPELINE-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_SYSTEMDATA]], align 8
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP3:%.*]] = alloca [[STRUCT_RAYPAYLOAD:%.*]], align 8
; LOWERRAYTRACINGPIPELINE-NEXT:    [[HITATTRS:%.*]] = alloca [[STRUCT_BUILTINTRIANGLEINTERSECTIONATTRIBUTES]], align 8
; LOWERRAYTRACINGPIPELINE-NEXT:    store [[STRUCT_SYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP4:%.*]] = getelementptr inbounds [[STRUCT_SYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA]], i32 0, i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP5:%.*]] = getelementptr inbounds [[STRUCT_RAYPAYLOAD]], ptr [[TMP3]], i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP8:%.*]] = load i32, ptr addrspace(20) @PAYLOAD, align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[TMP8]], ptr [[TMP5]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP9:%.*]] = getelementptr inbounds i32, ptr [[TMP5]], i32 1
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP11:%.*]] = load i32, ptr addrspace(20) getelementptr inbounds (i32, ptr addrspace(20) @PAYLOAD, i32 7), align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[TMP11]], ptr [[TMP9]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP12:%.*]] = call [[STRUCT_BUILTINTRIANGLEINTERSECTIONATTRIBUTES]] [[_CONT_GETTRIANGLEHITATTRIBUTES:@[a-zA-Z0-9_$\"\\.-]*[a-zA-Z_$\"\\.-][a-zA-Z0-9_$\"\\.-]*]](ptr [[SYSTEM_DATA_ALLOCA]])
; LOWERRAYTRACINGPIPELINE-NEXT:    store [[STRUCT_BUILTINTRIANGLEINTERSECTIONATTRIBUTES]] [[TMP12]], ptr [[TMP2]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP10:%.*]] = load i32, ptr [[TMP2]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[TMP10]], ptr [[HITATTRS]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP16:%.*]] = getelementptr inbounds i32, ptr [[HITATTRS]], i32 1
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP17:%.*]] = getelementptr inbounds i32, ptr [[TMP2]], i32 1
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP18:%.*]] = load i32, ptr [[TMP17]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[TMP18]], ptr [[TMP16]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    call void @amd.dx.setLocalRootIndex(i32 5)
; LOWERRAYTRACINGPIPELINE-NEXT:    [[PTR:%.*]] = getelementptr inbounds [[STRUCT_RAYPAYLOAD]], ptr [[TMP3]], i32 0, i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[BARYPTR:%.*]] = getelementptr inbounds [[STRUCT_BUILTINTRIANGLEINTERSECTIONATTRIBUTES]], ptr [[HITATTRS]], i32 0, i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[BARY:%.*]] = load <2 x float>, ptr [[BARYPTR]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store <2 x float> [[BARY]], ptr [[PTR]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP19:%.*]] = getelementptr inbounds [[STRUCT_RAYPAYLOAD]], ptr [[TMP3]], i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP22:%.*]] = load i32, ptr [[TMP19]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[TMP22]], ptr addrspace(20) @PAYLOAD, align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP23:%.*]] = getelementptr inbounds i32, ptr [[TMP19]], i32 1
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP25:%.*]] = load i32, ptr [[TMP23]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    store i32 [[TMP25]], ptr addrspace(20) getelementptr inbounds (i32, ptr addrspace(20) @PAYLOAD, i32 7), align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP26:%.*]] = getelementptr inbounds [[STRUCT_SYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA]], i32 0, i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[TMP27:%.*]] = load [[STRUCT_DISPATCHSYSTEMDATA:%.*]], ptr [[TMP26]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    call void (...) @lgc.ilcps.return(i64 [[RETURNADDR]], [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP27]]), !continuation.registercount [[META10]]
; LOWERRAYTRACINGPIPELINE-NEXT:    unreachable
;
  %ptr = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  %baryPtr = getelementptr inbounds %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %attr, i32 0, i32 0
  %bary = load <2 x float>, <2 x float>* %baryPtr, align 4
  store <2 x float> %bary, <2 x float>* %ptr, align 4
  ret void
}

attributes #0 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind memory(none) }
attributes #2 = { nounwind memory(read) }
attributes #3 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.typeAnnotations = !{}
!dx.entryPoints = !{!4, !6}

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
!1 = !{i32 1, i32 6}
!2 = !{!"lib", i32 6, i32 6}
!3 = !{null, null, null, null}
!4 = !{null, !"", null, !3, !5}
!5 = !{i32 0, i64 65536}
!6 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !"ClosestHit", null, null, !7}
!7 = !{i32 8, i32 10, i32 5, !8}
!8 = !{i32 0}
!9 = !{!"function", %struct.BuiltInTriangleIntersectionAttributes poison, !10}
!10 = !{i32 0, %struct.SystemData poison}
!11 = !{!"function", !"void", !10, %struct.BuiltInTriangleIntersectionAttributes poison}
!12 = !{!"function", i1 poison, !13}
!13 = !{i32 0, %struct.TraversalData poison}
!14 = !{!"function", %struct.HitData poison, !15}
!15 = !{i32 0, %struct.AnyHitTraversalData poison}
!16 = !{!"function", %struct.HitData poison, !10}
!17 = !{!"function", i32 poison, !18}
!18 = !{i32 0, %struct.DispatchSystemData poison}
!19 = !{!"function", !"void", !18, i64 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison}
!20 = !{!"function", i1 poison, !15, float poison, i32 poison}
!21 = !{!"function", i32 poison, !18, i32 poison}
!22 = !{!"function", float poison, !18, i32 poison}
!23 = !{!"function", float poison, !18}
!24 = !{!"function", float poison, !18, !25}
!25 = !{i32 0, %struct.HitData poison}
!26 = !{!"function", i32 poison, !18, !25}
!27 = !{!"function", float poison, !18, !25, i32 poison}
!28 = !{!"function", float poison, !18, !25, i32 poison, i32 poison}
!29 = !{!"function", i32 poison, !10, !25}
!30 = !{!"function", !"void", !31, !32}
!31 = !{i32 0, %struct.RayPayload poison}
!32 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
