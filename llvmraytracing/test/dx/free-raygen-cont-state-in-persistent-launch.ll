; Tests that if _cont_ExitRayGen ends with an enqueue, then we still free RayGen continuation state.
; This is a regression test, in an earlier version we only freed for returns and missed this case.
; RUN: grep -v "lgc.cps.module" %s | opt --verify-each -passes="dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,remove-types-metadata" -S --lint-abort-on-error | FileCheck %s
; RUN: opt --verify-each -passes="dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,dxil-cleanup-continuations,lint,remove-types-metadata" -S %s --lint-abort-on-error | FileCheck %s

; There is just a single RayGen shader in this module, so any free must come from it.
; CHECK: call void @lgc.cps.free

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

declare void @_AmdEnqueue(i64, i64, %struct.SystemData)

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !{%struct.DispatchSystemData poison} {
  call void @_AmdEnqueue(i64 1, i64 1, %struct.SystemData poison)
  unreachable
}

declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #0

declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #0

declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #0

declare !pointeetys !32 %struct.HitData @_cont_GetCandidateState(%struct.AnyHitTraversalData* %data) #0

declare !pointeetys !34 %struct.HitData @_cont_GetCommittedState(%struct.SystemData*) #0

declare !pointeetys !36 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

declare !pointeetys !37 void @_cont_SetTriangleHitAttributes(%struct.SystemData* %data, %struct.BuiltInTriangleIntersectionAttributes %val)

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
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %addr = call i64 @_AmdGetResumePointAddr() #3
  %trav_data2 = insertvalue %struct.TraversalData %trav_data, i64 %addr, 5
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data2)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

declare !pointeetys !46 void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) #0;

declare !pointeetys !47 i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) #0

declare !pointeetys !48 <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData* %data)

declare !pointeetys !49 <3 x float> @_cont_ObjectRayOrigin3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData)

declare  !pointeetys !49 <3 x float> @_cont_ObjectRayDirection3(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData)

declare !pointeetys !51 float @_cont_RayTCurrent(%struct.DispatchSystemData* nocapture readnone %data, %struct.HitData* %hitData)

declare i32 @opaque()
declare void @use(i32)

; Function Attrs: nounwind
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
  ; Ensure continuation state
  %cont.state = call i32 @opaque()
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %7, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)
  call void @use(i32 %cont.state)
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
!dx.entryPoints = !{!18, !29 }
!lgc.cps.module = !{}

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
!10 = !{i32 1, void ()* @MyRayGen, !11}
!11 = !{!12}
!12 = !{i32 1, !13, !13}
!13 = !{}
!14 = !{!12, !15, !16}
!15 = !{i32 2, !13, !13}
!16 = !{i32 0, !13, !13}
!17 = !{!12, !15}
!18 = !{null, !"", null, !3, !19}
!19 = !{i32 0, i64 65536}
!21 = !{i32 8, i32 9, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!22 = !{i32 0}
!24 = !{i32 8, i32 10, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!26 = !{i32 8, i32 8, i32 5, !22}
!28 = !{i32 8, i32 11, i32 6, i32 16, i32 5, !22}
!29 = !{void ()* @MyRayGen, !"MyRayGen", null, null, !30}
!30 = !{i32 8, i32 7, i32 5, !22}
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
