; RUN: grep -v MAX_REG_10 %s | \
; RUN:    opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,continuations-lint,remove-types-metadata' -S --lint-abort-on-error | \
; RUN:    FileCheck -check-prefixes=COMMON,MAX30 %s
;
; RUN: grep -v MAX_REG_30 %s | \
; RUN:    opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,continuations-lint,remove-types-metadata' -S --lint-abort-on-error | \
; RUN:    FileCheck -check-prefixes=COMMON,MAX10 %s

; The order of metadata on functions is non-deterministic, so make two different runs to match both of them.
; The 'grep' commands filter out a metadata node that reduces the payload register count.

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { %struct.SystemData }
%struct.SystemData = type { %struct.DispatchSystemData, %struct.BuiltInTriangleIntersectionAttributes }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.HitData = type { float, i32 }
%struct.TheirParams = type { [10 x i32] }
%struct.RayPayload = type { [15 x i32] }
%struct.PayloadWithI16 = type { i16, i16 }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.MyParams = type { [26 x i32] }
%struct.TheirParams2 = type { [27 x i32] }
%struct._AmdTraversalResultData = type { %struct._AmdPrimitiveSystemState, <2 x float>, i32 }
%struct._AmdPrimitiveSystemState = type { float, i32, i32, i32 }
%struct._AmdSystemData = type { %struct._AmdTraversalResultData }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

; Function Attrs: alwaysinline
declare i32 @_cont_GetContinuationStackAddr() #0

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #0

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #0

; Function Attrs: alwaysinline
declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #0

; Function Attrs: nounwind memory(read)
declare !pointeetys !24 i32 @_cont_HitKind(%struct.SystemData* nocapture readnone, %struct.HitData*) #1

; Function Attrs: nounwind memory(none)
declare !pointeetys !27 void @_AmdRestoreSystemData(%struct.DispatchSystemData*) #2

; Function Attrs: nounwind memory(none)
declare !pointeetys !29 void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData*) #2

; Function Attrs: nounwind memory(none)
declare !pointeetys !29 void @_cont_AcceptHit(%struct.AnyHitTraversalData* nocapture readnone) #2

; Function Attrs: alwaysinline
declare i1 @opaqueIsEnd() #0

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !{%struct.DispatchSystemData poison} {
  ret void
}

; Function Attrs: alwaysinline
define i1 @_cont_IsEndSearch(%struct.TraversalData* %data) #0 !pointeetys !31 {
  %isEnd = call i1 @opaqueIsEnd()
  ret i1 %isEnd
}

; Function Attrs: alwaysinline
define %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData* %data) #0 !pointeetys !33 {
  %addr = getelementptr %struct.SystemData, %struct.SystemData* %data, i32 0, i32 1
  %val = load %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %addr, align 4
  ret %struct.BuiltInTriangleIntersectionAttributes %val
}

; Function Attrs: alwaysinline
define void @_cont_SetTriangleHitAttributes(%struct.SystemData* %data, %struct.BuiltInTriangleIntersectionAttributes %val) #0 !pointeetys !34 {
  %addr = getelementptr %struct.SystemData, %struct.SystemData* %data, i32 0, i32 1
  store %struct.BuiltInTriangleIntersectionAttributes %val, %struct.BuiltInTriangleIntersectionAttributes* %addr, align 4
  ret void
}

; Function Attrs: alwaysinline
define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #0 !pointeetys !35 {
  ret i32 5
}

; Function Attrs: alwaysinline
define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #0 !pointeetys !36 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

; Function Attrs: alwaysinline
define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) #0 !pointeetys !37 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

; Function Attrs: alwaysinline
define i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) #0 !pointeetys !38 {
  %trav_data = load %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data, align 4
  %newdata = call %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64 3, %struct.AnyHitTraversalData %trav_data, float %t, i32 %hitKind)
  store %struct.AnyHitTraversalData %newdata, %struct.AnyHitTraversalData* %data, align 4
  call void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData* %data)
  ret i1 true
}

; COMMON-DAG: define void @main(
; COMMON-DAG: call void (...) @lgc.cps.jump(i64 2, {{.*}} %struct.DispatchSystemData %{{.*}}, [10 x i32] %{{.*}})

define void @main() {
  %params = alloca %struct.TheirParams, align 4
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)
  ret void
}

; COMMON-DAG: define void @mainTrace(
; MAX10-DAG: call void (...) @lgc.cps.jump(i64 4, {{.*}} %struct.TraversalData %{{.*}}, [10 x i32] %{{.*}})
; MAX30-DAG: call void (...) @lgc.cps.jump(i64 4, {{.*}} %struct.TraversalData %{{.*}}, [15 x i32] %{{.*}})
define void @mainTrace() {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  %5 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)
  %6 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %5, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %6, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)
  ret void
}

; If we set maxPayloadRegisterCount to 10, both functions use only 10 payload registers.
; MAX10-DAG: define void @called({{.*}}%struct.DispatchSystemData %0{{.*}}, [10 x i32] %payload)
; MAX10-DAG: define dso_local void @called.resume.0({{.*}}%struct.DispatchSystemData{{.*}}, [10 x i32] }{{.*}})
; MAX30-DAG: define void @called({{.*}}%struct.DispatchSystemData %0{{.*}}, [26 x i32] %payload)
; MAX30-DAG: define dso_local void @called.resume.0({{.*}}%struct.DispatchSystemData{{.*}}, [27 x i32] }{{.*}})

define void @called(%struct.MyParams* %arg) !pointeetys !39 {
  %params = alloca %struct.TheirParams2, align 4
  call void @dx.op.callShader.struct.TheirParams2(i32 159, i32 2, %struct.TheirParams2* nonnull %params)
  ret void
}

; MAX10-DAG: define void @Intersection({{.*}}%struct.AnyHitTraversalData %0{{.*}}, [10 x i32] %payload)
; MAX10-DAG: define dso_local void @Intersection.resume.0({{.*}}%struct.AnyHitTraversalData{{.*}}, [10 x i32] }{{.*}})
; MAX10-DAG: call void (...) @lgc.cps.jump(i64 3, {{.*}} float 4.000000e+00, i32 0, %struct.BuiltInTriangleIntersectionAttributes {{.*}}, [10 x i32] %{{.*}})
; MAX30-DAG: define void @Intersection({{.*}}%struct.AnyHitTraversalData %0{{.*}}, [30 x i32] %payload)
; MAX30-DAG: define dso_local void @Intersection.resume.0({{.*}}%struct.AnyHitTraversalData{{.*}}, [30 x i32] }{{.*}})
; MAX30-DAG: call void (...) @lgc.cps.jump(i64 3, {{.*}} float 4.000000e+00, i32 0, %struct.BuiltInTriangleIntersectionAttributes {{.*}}, [30 x i32] %{{.*}})

define void @Intersection() #3 {
  %a = alloca %struct.BuiltInTriangleIntersectionAttributes, align 4
  %b = call i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32 158, float 4.000000e+00, i32 0, %struct.BuiltInTriangleIntersectionAttributes* nonnull %a)
  ret void
}

; MAX10-DAG: define void @AnyHit({{.*}}%struct.AnyHitTraversalData %0, %struct.BuiltInTriangleIntersectionAttributes %1{{.*}}, [10 x i32] %payload)
; MAX30-DAG: define void @AnyHit({{.*}}%struct.AnyHitTraversalData %0, %struct.BuiltInTriangleIntersectionAttributes %1{{.*}}, [15 x i32] %payload)

define void @AnyHit(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #3 !pointeetys !41 {
  ret void
}

; With fixed hit attribute registers and without PAQs, ClosestHitOut also contains storage for hit attributes
; MAX10-DAG: define void @ClosestHit({{.*}}%struct.SystemData %0{{.*}}, [10 x i32] %payload)
; MAX30-DAG: define void @ClosestHit({{.*}}%struct.SystemData %0{{.*}}, [15 x i32] %payload)

define void @ClosestHit(%struct.RayPayload* noalias nocapture %payload, %struct.AnyHitTraversalData* nocapture readonly %attr) #3 !pointeetys !41 {
  ret void
}

; COMMON-DAG: define void @Miss16({{.*}}%struct.SystemData %0{{.*}}, [1 x i32] %payload)
define void @Miss16(%struct.PayloadWithI16* noalias nocapture %payload) !pointeetys !55 {
  ret void
}

declare void @_AmdEnqueueAnyHit(i64, i64, %struct._AmdSystemData, <2 x float>) #0

; MAX10-DAG: define void @_cont_Traversal({{.*}}, [10 x i32] %payload)
; MAX10-DAG: call {{.*}} @lgc.cps.jump({{.*}}, [10 x i32] %{{.*}})
; MAX30-DAG: define void @_cont_Traversal({{.*}}, [27 x i32] %payload)
; MAX30-DAG: call {{.*}} @lgc.cps.jump({{.*}}, [27 x i32] %{{.*}})

define void @_cont_Traversal(%struct._AmdTraversalResultData* noalias nocapture sret(%struct._AmdTraversalResultData) %agg.result, %struct._AmdSystemData* noalias %data) !pointeetys !44 {
  call void @_AmdEnqueueAnyHit(i64 0, i64 poison, %struct.BuiltInTriangleIntersectionAttributes undef, <2 x float> undef)
  unreachable
}

; Function Attrs: nounwind
declare !pointeetys !47 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #3

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #2

; Function Attrs: nounwind memory(read)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #1

; Function Attrs: nounwind
declare !pointeetys !48 void @dx.op.callShader.struct.TheirParams(i32, i32, %struct.TheirParams*) #3

; Function Attrs: nounwind
declare !pointeetys !50 void @dx.op.callShader.struct.TheirParams2(i32, i32, %struct.TheirParams2*) #3

declare !pointeetys !52 i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes*)

attributes #0 = { alwaysinline }
attributes #1 = { nounwind memory(read) }
attributes #2 = { nounwind memory(none) }
attributes #3 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.entryPoints = !{!3, !6, !13, !15, !17, !19, !21, !57}
!continuation.maxPayloadRegisterCount = !{!23} ; 10; only for MAX_REG_10
!continuation.maxPayloadRegisterCount = !{!53} ; 30; only for MAX_REG_30
!continuation.preservedPayloadRegisterCount = !{!23} ; 10; only for MAX_REG_10
!continuation.preservedPayloadRegisterCount = !{!54} ; 27; only for MAX_REG_30
!lgc.rt.max.attribute.size = !{!60}

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
!1 = !{i32 1, i32 6}
!2 = !{!"lib", i32 6, i32 6}
!3 = !{null, !"", null, !4, !12}
!4 = !{!5, !9, null, null}
!5 = !{!6}
!6 = !{void ()* @main, !"main", null, null, !7}
!7 = !{i32 8, i32 7, i32 6, i32 16, i32 7, i32 8, i32 5, !8}
!8 = !{i32 0}
!9 = !{!10}
!10 = !{i32 0, %"class.RWTexture2D<vector<float, 4> >"* bitcast (%dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" to %"class.RWTexture2D<vector<float, 4> >"*), !"RenderTarget", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !11}
!11 = !{i32 0, i32 9}
!12 = !{i32 0, i64 65536}
!13 = !{void (%struct.MyParams*)* @called, !"called", null, null, !14}
!14 = !{i32 8, i32 12}
!15 = !{void ()* @mainTrace, !"mainTrace", null, null, !16}
!16 = !{i32 8, i32 7}
!17 = !{void ()* @Intersection, !"Intersection", null, null, !18}
!18 = !{i32 8, i32 8, i32 5, !8}
!19 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @AnyHit, !"AnyHit", null, null, !20}
!20 = !{i32 8, i32 9, i32 5, !8}
!21 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !"ClosestHit", null, null, !22}
!22 = !{i32 8, i32 10, i32 5, !8}
!23 = !{i32 10}
!24 = !{null, %struct.SystemData poison, %struct.HitData poison}
!25 = !{i32 0, %struct.SystemData poison}
!26 = !{i32 0, %struct.HitData poison}
!27 = !{%struct.DispatchSystemData poison}
!28 = !{i32 0, %struct.DispatchSystemData poison}
!29 = !{%struct.AnyHitTraversalData poison}
!30 = !{i32 0, %struct.AnyHitTraversalData poison}
!31 = !{%struct.TraversalData poison}
!32 = !{i32 0, %struct.TraversalData poison}
!33 = !{%struct.SystemData poison}
!34 = !{%struct.SystemData poison}
!35 = !{%struct.DispatchSystemData poison}
!36 = !{%struct.DispatchSystemData poison}
!37 = !{%struct.DispatchSystemData poison}
!38 = !{%struct.AnyHitTraversalData poison}
!39 = !{%struct.MyParams poison}
!40 = !{i32 0, %struct.MyParams poison}
!41 = !{null, %struct.RayPayload poison, %struct.BuiltInTriangleIntersectionAttributes poison}
!42 = !{i32 0, %struct.RayPayload poison}
!43 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
!44 = !{null, %struct._AmdTraversalResultData poison, %struct._AmdSystemData poison}
!45 = !{i32 0, %struct._AmdTraversalResultData poison}
!46 = !{i32 0, %struct._AmdSystemData poison}
!47 = !{%struct.RayPayload poison}
!48 = !{%struct.TheirParams poison}
!49 = !{i32 0, %struct.TheirParams poison}
!50 = !{%struct.TheirParams2 poison}
!51 = !{i32 0, %struct.TheirParams2 poison}
!52 = !{%struct.BuiltInTriangleIntersectionAttributes poison}
!53 = !{i32 30}
!54 = !{i32 27}
!55 = !{%struct.PayloadWithI16 poison}
!56 = !{i32 0, %struct.PayloadWithI16 poison}
!57 = !{void (%struct.PayloadWithI16*)* @Miss16, !"Miss16", null, null, !58}
!58 = !{i32 8, i32 11, i32 6, i32 24, i32 5, !59}
!59 = !{i32 0}
!60 = !{i32 8}
