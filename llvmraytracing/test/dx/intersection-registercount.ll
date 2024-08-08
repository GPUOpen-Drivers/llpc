; RUN: opt --verify-each --report-payload-register-sizes -passes='dxil-cont-intrinsic-prepare,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,continuations-stats-report,lint,dxil-cont-post-process,lint,continuations-lint,remove-types-metadata' -S %s --lint-abort-on-error 2>&1 | FileCheck %s

; CHECK: Incoming and max outgoing payload VGPR size of "Intersection" (intersection): 100 and 100 bytes

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.DispatchSystemData = type { <3 x i32> }
%struct.TraversalData = type { %struct.SystemData, %struct.HitData, <3 x float>, <3 x float>, float }
%struct.HitData = type { float, i32 }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.RayPayload = type { <4 x float> }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

declare i64 @_cont_GetTraversalAddr() #0

declare i32 @_cont_GetContinuationStackAddr() #0

declare !pointeetys !16 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

declare !pointeetys !18 void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #0

declare !pointeetys !19 i1 @_cont_IsEndSearch(%struct.TraversalData*) #0

declare %struct.DispatchSystemData @_cont_Traversal(%struct.TraversalData) #0

declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #0

declare !pointeetys !21 %struct.HitData @_cont_GetCandidateState(%struct.AnyHitTraversalData*) #0

declare !pointeetys !23 %struct.HitData @_cont_GetCommittedState(%struct.SystemData*) #0

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #0 !pointeetys !24 {
  ret i32 5
}

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #0 !pointeetys !26 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_cont_Traversal(%struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  ret void
}

define i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) #0 !pointeetys !27 {
  %trav_data = load %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, align 4
  %newdata = call %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64 3, %struct.AnyHitTraversalData %trav_data, float %t, i32 %hitKind)
  store %struct.AnyHitTraversalData %newdata, %struct.AnyHitTraversalData* %data, align 4
  ret i1 true
}

; Function Attrs: nounwind memory(none)
declare !pointeetys !28 i32 @_cont_DispatchRaysIndex(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !28 i32 @_cont_DispatchRaysDimensions(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !29 float @_cont_WorldRayOrigin(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !29 float @_cont_WorldRayDirection(%struct.DispatchSystemData* nocapture readnone, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !30 float @_cont_RayTMin(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind memory(read)
declare !pointeetys !31 float @_cont_RayTCurrent(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #2

; Function Attrs: nounwind memory(none)
declare !pointeetys !24 i32 @_cont_RayFlags(%struct.DispatchSystemData* nocapture readnone) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !33 i32 @_cont_InstanceIndex(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !33 i32 @_cont_InstanceID(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !33 i32 @_cont_PrimitiveIndex(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !34 float @_cont_ObjectRayOrigin(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !34 float @_cont_ObjectRayDirection(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !35 float @_cont_ObjectToWorld(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !35 float @_cont_WorldToObject(%struct.DispatchSystemData* nocapture readnone, %struct.HitData*, i32, i32) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !36 i32 @_cont_HitKind(%struct.SystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind
define void @Intersection() #3 !lgc.rt.shaderstage !41 {
  ret void
}

; Function Attrs: nounwind memory(read)
declare !pointeetys !37 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #2

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !pointeetys !39 void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #4

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !pointeetys !39 void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #4

attributes #0 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind memory(none) }
attributes #2 = { nounwind memory(read) }
attributes #3 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #4 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.typeAnnotations = !{}
!dx.entryPoints = !{!10, !12}
!continuation.maxPayloadRegisterCount = !{!15}
!continuation.minPayloadRegisterCount = !{!14}

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
!12 = !{void ()* @Intersection, !"Intersection", null, null, !13}
!13 = !{i32 8, i32 8, i32 5, !14}
!14 = !{i32 0}
!15 = !{i32 25}
!16 = !{%struct.SystemData poison}
!17 = !{i32 0, %struct.SystemData poison}
!18 = !{%struct.SystemData poison}
!19 = !{%struct.TraversalData poison}
!20 = !{i32 0, %struct.TraversalData poison}
!21 = !{%struct.AnyHitTraversalData poison}
!22 = !{i32 0, %struct.AnyHitTraversalData poison}
!23 = !{%struct.SystemData poison}
!24 = !{%struct.DispatchSystemData poison}
!25 = !{i32 0, %struct.DispatchSystemData poison}
!26 = !{%struct.DispatchSystemData poison}
!27 = !{%struct.AnyHitTraversalData poison}
!28 = !{%struct.DispatchSystemData poison}
!29 = !{%struct.DispatchSystemData poison}
!30 = !{%struct.DispatchSystemData poison}
!31 = !{null, %struct.DispatchSystemData poison, %struct.HitData poison}
!32 = !{i32 0, %struct.HitData poison}
!33 = !{null, %struct.DispatchSystemData poison, %struct.HitData poison}
!34 = !{null, %struct.DispatchSystemData poison, %struct.HitData poison}
!35 = !{null, %struct.DispatchSystemData poison, %struct.HitData poison}
!36 = !{null, %struct.SystemData poison, %struct.HitData poison}
!37 = !{%struct.RayPayload poison}
!38 = !{i32 0, %struct.RayPayload poison}
!39 = !{i8 poison}
!40 = !{i32 0, i8 poison}
!41 = !{i32 1}
