; RUN: opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint' -S %s 2>%t0.stderr | FileCheck -check-prefix=LOWERRAYTRACINGPIPELINE-DECL %s
; RUN: count 0 < %t0.stderr
; RUN: opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,inline,lint,dxil-cont-pre-coroutine,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,register-buffer,lint,save-continuation-state,lint,dxil-cont-post-process,lint' -S %s 2>%t1.stderr | FileCheck -check-prefix=DXILCONTPOSTPROCESS-DECL %s
; RUN: count 0 < %t1.stderr

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.HitData = type { float, i32 }
%struct.DispatchSystemData = type { <3 x i32> }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.TraversalData = type { %struct.SystemData, <3 x float>, <3 x float>, float }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }

declare i64 @_cont_GetTraversalAddr() #4
declare i32 @_cont_GetContinuationStackAddr() #4
declare !types !31 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.DispatchSystemData*) #4
declare !types !32 void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #4
declare %struct.DispatchSystemData @_cont_Traversal(%struct.TraversalData) #4
declare %struct.DispatchSystemData @_cont_SetupRayGen() #4
declare !types !33 %struct.HitData @_cont_GetCandidateState(%struct.AnyHitTraversalData*) #4
declare !types !34 %struct.HitData @_cont_GetCommittedState(%struct.DispatchSystemData*) #4

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #4 !types !37 {
  ret i32 5
}

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float) #4 !types !38 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_cont_Traversal(%struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data
  ret void
}

; Function Attrs: nounwind readnone
declare !types !40 <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData* nocapture readnone %data) #2
declare !types !40 <3 x i32> @_cont_DispatchRaysDimensions3(%struct.DispatchSystemData* nocapture readnone %data) #2
declare !types !41 <3 x float> @_cont_WorldRayOrigin3(%struct.DispatchSystemData* nocapture readnone %data) #2
declare !types !41 <3 x float> @_cont_WorldRayDirection3(%struct.DispatchSystemData* nocapture readnone %data) #2
declare !types !42 float @_cont_RayTMin(%struct.DispatchSystemData* nocapture readnone %data) #2
declare !types !43 float @_cont_RayTCurrent(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #1
declare !types !51 i32 @_cont_RayFlags(%struct.DispatchSystemData* nocapture readnone %data) #2
declare !types !52 i32 @_cont_InstanceIndex(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !52 i32 @_cont_InstanceID(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !52 i32 @_cont_PrimitiveIndex(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !46 <3 x float> @_cont_ObjectRayOrigin3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !46 <3 x float> @_cont_ObjectRayDirection3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !47 [4 x <3 x float>] @_cont_ObjectToWorld4x3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !47 [4 x <3 x float>] @_cont_WorldToObject4x3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData*) #2
declare !types !45 i32 @_cont_HitKind(%struct.SystemData* nocapture readnone %data, %struct.HitData*) #2

%dx.types.Handle = type { i8* }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }
%struct.RayPayload = type { float, float, i32, i32 }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

; Function Attrs: nounwind
define void @ClosestHit(%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*) #0 !types !48 {
  %a = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)  ; DispatchRaysIndex(col)
  %b = call i32 @dx.op.dispatchRaysDimensions.i32(i32 146, i8 0)  ; DispatchRaysDimensions(col)
  %c = call float @dx.op.worldRayOrigin.f32(i32 147, i8 0)  ; WorldRayOrigin(col)
  %d = call float @dx.op.worldRayDirection.f32(i32 148, i8 0)  ; WorldRayDirection(col)
  %e = call float @dx.op.rayTMin.f32(i32 153)  ; RayTMin()
  %f = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()
  %g = call i32 @dx.op.rayFlags.i32(i32 144)  ; RayFlags()
  %h = call i32 @dx.op.instanceIndex.i32(i32 142)  ; InstanceIndex()
  %i = call i32 @dx.op.instanceID.i32(i32 141)  ; InstanceID()
  %j = call i32 @dx.op.primitiveIndex.i32(i32 161)  ; PrimitiveIndex()
  %k = call float @dx.op.objectRayOrigin.f32(i32 149, i8 0)  ; ObjectRayOrigin(col)
  %l = call float @dx.op.objectRayDirection.f32(i32 150, i8 0)  ; ObjectRayDirection(col)
  %m = call float @dx.op.objectToWorld.f32(i32 151, i32 0, i8 0)  ; ObjectToWorld(row,col)
  %n = call float @dx.op.worldToObject.f32(i32 152, i32 0, i8 0)  ; WorldToObject(row,col)
  %o = call i32 @dx.op.hitKind.i32(i32 143)  ; HitKind()
  ret void
}

; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.dispatchRaysDimensions.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.dispatchRaysIndex.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.objectRayDirection.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.objectRayOrigin.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.worldRayDirection.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.worldRayOrigin.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.rayTCurrent.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.rayTMin.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.hitKind.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.primitiveIndex.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.instanceID.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.instanceIndex.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.rayFlags.i32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.worldToObject.f32(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @dx.op.objectToWorld.f32(
; LOWERRAYTRACINGPIPELINE-DECL: declare <3 x i32> @lgc.rt.dispatch.rays.dimensions(
; LOWERRAYTRACINGPIPELINE-DECL: declare <3 x i32> @lgc.rt.dispatch.rays.index(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare <3 x float> @lgc.rt.object.ray.direction(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare <3 x float> @lgc.rt.object.ray.origin(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare <3 x float> @lgc.rt.world.ray.direction(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare <3 x float> @lgc.rt.world.ray.origin(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare float @lgc.rt.ray.tmin(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @lgc.rt.instance.id(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare float @lgc.rt.ray.tcurrent(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @lgc.rt.hit.kind(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @lgc.rt.primitive.index(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @lgc.rt.instance.index(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare i32 @lgc.rt.ray.flags(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare [4 x <3 x float>] @lgc.rt.object.to.world(
; LOWERRAYTRACINGPIPELINE-DECL-NOT: declare [4 x <3 x float>] @lgc.rt.world.to.object(
; DXILCONTPOSTPROCESS-DECL-NOT: declare <3 x i32> @lgc.rt.dispatch.rays.dimensions(
; DXILCONTPOSTPROCESS-DECL-NOT: declare <3 x i32> @lgc.rt.dispatch.rays.index(
declare i32 @dx.op.dispatchRaysDimensions.i32(i32, i8) #2
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #2
declare float @dx.op.objectRayDirection.f32(i32, i8) #2
declare float @dx.op.objectRayOrigin.f32(i32, i8) #2
declare float @dx.op.worldRayDirection.f32(i32, i8) #2
declare float @dx.op.worldRayOrigin.f32(i32, i8) #2
declare float @dx.op.rayTCurrent.f32(i32) #1
declare float @dx.op.rayTMin.f32(i32) #2
declare i32 @dx.op.hitKind.i32(i32) #2
declare i32 @dx.op.primitiveIndex.i32(i32) #2
declare i32 @dx.op.instanceID.i32(i32) #2
declare i32 @dx.op.instanceIndex.i32(i32) #2
declare i32 @dx.op.rayFlags.i32(i32) #2
declare float @dx.op.worldToObject.f32(i32, i32, i8) #2
declare float @dx.op.objectToWorld.f32(i32, i32, i8) #2

attributes #0 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readonly }
attributes #2 = { nounwind readnone }
attributes #4 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.typeAnnotations = !{!10}
!dx.entryPoints = !{!18, !29}

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
!10 = !{i32 1, void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !11}
!11 = !{!12}
!12 = !{i32 1, !13, !13}
!13 = !{}
!18 = !{null, !"", null, !3, !19}
!19 = !{i32 0, i64 65536}
!22 = !{i32 0}
!29 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !"ClosestHit", null, null, !30}
!30 = !{i32 8, i32 10, i32 5, !22}
!31 = !{!"function", %struct.BuiltInTriangleIntersectionAttributes poison, !39}
!32 = !{!"function", !"void", !36, %struct.BuiltInTriangleIntersectionAttributes poison}
!33 = !{!"function", %struct.HitData poison, !35}
!34 = !{!"function", %struct.HitData poison, !39}
!35 = !{i32 0, %struct.AnyHitTraversalData poison}
!36 = !{i32 0, %struct.SystemData poison}
!37 = !{!"function", i32 poison, !39}
!38 = !{!"function", !"void", !39, i64 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison}
!39 = !{i32 0, %struct.DispatchSystemData poison}
!40 = !{!"function", <3 x i32> poison, !39}
!41 = !{!"function", <3 x float> poison, !39}
!42 = !{!"function", float poison, !39}
!43 = !{!"function", float poison, !39, !44}
!44 = !{i32 0, %struct.HitData poison}
!45 = !{!"function", i32 poison, !36, !44}
!46 = !{!"function", <3 x float> poison, !39, !44}
!47 = !{!"function", [4 x <3 x float>] poison, !39, !44}
!48 = !{!"function", !"void", !49, !50}
!49 = !{i32 0, %struct.RayPayload poison}
!50 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
!51 = !{!"function", i32 poison, !39}
!52 = !{!"function", i32 poison, !39, !44}
