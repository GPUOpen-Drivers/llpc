; RUN: grep -v SKIP_LINE_BY_DEFAULT %s | \
; RUN:    opt --opaque-pointers=0 --enforce-pointer-metadata=1 --verify-each -passes='add-types-metadata,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,inline,lint,dxil-cont-pre-coroutine,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,cleanup-continuations,lint,register-buffer,lint,save-continuation-state,lint,dxil-cont-post-process,lint,remove-types-metadata' -S 2>%t0.stderr | \
; RUN:    FileCheck -check-prefix=POSTPROCESS-REGCOUNT %s
; RUN: count 0 < %t0.stderr
;
; RUN: grep -v SKIP_LINE_BY_DEFAULT %s | \
; RUN:    opt --opaque-pointers=0 --enforce-pointer-metadata=1 --verify-each -passes='add-types-metadata,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,inline,lint,dxil-cont-pre-coroutine,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,cleanup-continuations,lint,register-buffer,lint,save-continuation-state,lint,dxil-cont-post-process,lint,remove-types-metadata' -S 2>%t1.stderr | \
; RUN:    FileCheck -check-prefix=POSTPROCESS-REGCOUNT2 %s
; RUN: count 0 < %t1.stderr
;
; RUN: opt --opaque-pointers=0 --enforce-pointer-metadata=1 --verify-each -passes='add-types-metadata,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,inline,lint,dxil-cont-pre-coroutine,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,cleanup-continuations,lint,register-buffer,lint,save-continuation-state,lint,dxil-cont-post-process,lint,remove-types-metadata' -S %s 2>%t2.stderr | \
; RUN:    FileCheck -check-prefix=POSTPROCESS-REGCOUNT-FEWREGS %s
; RUN: count 0 < %t2.stderr

; The order of metadata on functions is non-deterministic, so make two different runs to match both of them.
; The 'grep' commands filter out a metadata node that reduces the payload register count.

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.MyParams = type { [26 x i32] }
%struct.TheirParams = type { [10 x i32] }
%struct.TheirParams2 = type { [27 x i32] }
%struct.RayPayload = type { [9 x i32] }
%dx.types.Handle = type { i8* }
%dx.types.ResourceProperties = type { i32, i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

%struct.HitData = type { float, i32 }
%struct.DispatchSystemData = type { i32 }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.SystemData = type { %struct.DispatchSystemData, %struct.BuiltInTriangleIntersectionAttributes }
%struct.TraversalData = type { %struct.SystemData }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct._AmdPrimitiveSystemState = type { float, i32, i32, i32 }
%struct._AmdSystemData = type { i32 }
%struct._AmdAnyHitSystemData = type { %struct._AmdSystemData, %struct._AmdPrimitiveSystemState }
%struct._AmdTraversalResultData = type { %struct._AmdPrimitiveSystemState, <2 x float>, i32 }

declare i32 @_cont_GetContinuationStackAddr() #4
declare %struct.DispatchSystemData @_cont_SetupRayGen() #4
declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #4
declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #4
declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #4
declare i32 @_cont_HitKind(%struct.SystemData* nocapture readnone %data, %struct.HitData*) #2
declare void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data) #1
declare void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData* %data) #1
declare void @_cont_AcceptHit(%struct.AnyHitTraversalData* nocapture readnone %data) #1

declare i1 @opaqueIsEnd() #4
define i1 @_cont_IsEndSearch(%struct.TraversalData* %data) #4 {
  %isEnd = call i1 @opaqueIsEnd()
  ret i1 %isEnd
}

define %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData* %data) #4 {
  %addr = getelementptr %struct.SystemData, %struct.SystemData* %data, i32 0, i32 1
  %val = load %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %addr
  ret %struct.BuiltInTriangleIntersectionAttributes %val
}

define void @_cont_SetTriangleHitAttributes(%struct.SystemData* %data, %struct.BuiltInTriangleIntersectionAttributes %val) #4 {
  %addr = getelementptr %struct.SystemData, %struct.SystemData* %data, i32 0, i32 1
  store %struct.BuiltInTriangleIntersectionAttributes %val, %struct.BuiltInTriangleIntersectionAttributes* %addr
  ret void
}

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #4 {
  ret i32 5
}

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float) #4 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32) #4 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind) #4 {
  %trav_data = load %struct.AnyHitTraversalData, %struct.AnyHitTraversalData* %data
  %newdata = call %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64 3, %struct.AnyHitTraversalData %trav_data, float %t, i32 %hitKind)
  store %struct.AnyHitTraversalData %newdata, %struct.AnyHitTraversalData* %data
  call void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData* %data)
  ret i1 1
}

; POSTPROCESS-REGCOUNT-DAG: call void (i64, ...) @continuation.continue(i64 2, {{.*}}, %struct.DispatchSystemData %{{[^ ]+}}), !continuation.registercount ![[callshader_registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT-DAG: ![[callshader_registercount]] = !{i32 10}

define void @main() {
  %params = alloca %struct.TheirParams, align 4
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
  ret void
}

; POSTPROCESS-REGCOUNT-DAG: call void (i64, ...) @continuation.continue(i64 4, {{.*}} %struct.TraversalData %{{[^ ]+}}), !continuation.registercount ![[traceray_registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT-DAG: ![[traceray_registercount]] = !{i32 15}

define void @mainTrace() {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  %5 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)  ; CreateHandleForLib(Resource)
  %6 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %5, %dx.types.ResourceProperties { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %6, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)  ; TraceRay(AccelerationStructure,RayFlags,InstanceInclusionMask,RayContributionToHitGroupIndex,MultiplierForGeometryContributionToShaderIndex,MissShaderIndex,Origin_X,Origin_Y,Origin_Z,TMin,Direction_X,Direction_Y,Direction_Z,TMax,payload)
  ret void
}

; POSTPROCESS-REGCOUNT-DAG: define void @called({{.*}}%struct.DispatchSystemData %0){{.*}} !continuation.registercount ![[called_registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT-DAG: define void @called.resume.0({{.*}}%struct.DispatchSystemData %1){{.*}} !continuation.registercount ![[called_resume_registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT-DAG: ![[called_registercount]] = !{i32 26}
; POSTPROCESS-REGCOUNT-DAG: ![[called_resume_registercount]] = !{i32 27}

; If we set maxPayloadRegisterCount to 10, both functions use only 10 payload registers.
; Note that due to metadata uniquing, both use the same metadata node.
; POSTPROCESS-REGCOUNT-FEWREGS-DAG: define void @called({{.*}}%struct.DispatchSystemData %0){{.*}} !continuation.registercount ![[registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT-FEWREGS-DAG: define void @called.resume.0({{.*}}%struct.DispatchSystemData %1){{.*}} !continuation.registercount ![[registercount]]
; POSTPROCESS-REGCOUNT-FEWREGS-DAG: ![[registercount]] = !{i32 10}

define void @called(%struct.MyParams* %arg) {
  %params = alloca %struct.TheirParams2, align 4
  call void @dx.op.callShader.struct.TheirParams2(i32 159, i32 2, %struct.TheirParams2* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
  ret void
}

; POSTPROCESS-REGCOUNT-DAG: define void @Intersection({{.*}}%struct.AnyHitTraversalData %0){{.*}} !continuation.registercount ![[intersection_registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT-DAG: define void @Intersection.resume.0({{.*}}%struct.AnyHitTraversalData %1){{.*}} !continuation.registercount ![[intersection_registercount]]
; POSTPROCESS-REGCOUNT-DAG: call void (i64, ...) @continuation.continue(i64 3, {{.*}} float 4.000000e+00, i32 0, %struct.BuiltInTriangleIntersectionAttributes {{.*}}), !continuation.registercount ![[intersection_registercount]]
; POSTPROCESS-REGCOUNT-DAG: ![[intersection_registercount]] = !{i32 30}

; Function Attrs: nounwind
define void @Intersection() #0 {
  %a = alloca %struct.BuiltInTriangleIntersectionAttributes, align 4
  %b = call i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32 158, float 4.0, i32 0, %struct.BuiltInTriangleIntersectionAttributes* nonnull %a)  ; ReportHit(THit,HitKind,Attributes)
  ret void
}

; POSTPROCESS-REGCOUNT2-DAG: define void @AnyHit({{.*}}%struct.AnyHitTraversalData %0, %struct.BuiltInTriangleIntersectionAttributes %1){{.*}} !continuation.registercount ![[anyhit_registercount:[0-9]+]]
; POSTPROCESS-REGCOUNT2-DAG: ![[anyhit_registercount]] = !{i32 15}

; Function Attrs: nounwind
define void @AnyHit(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #0 {
  ret void
}

; With fixed hit attribute registers and without PAQs, ClosestHitOut also contains storage for hit attributes,
; so we re-used the anyhit_registercount metadata for the match.
; POSTPROCESS-REGCOUNT2-DAG: define void @ClosestHit({{.*}}%struct.SystemData %0){{.*}} !continuation.registercount ![[anyhit_registercount]]

; Function Attrs: nounwind
define void @ClosestHit(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #0 {
  ret void
}

declare void @continuation.continue(i64, ...)

; POSTPROCESS-REGCOUNT-FEWREGS-DAG: define void @TraversalImpl1_1({{.*}} !continuation.registercount ![[registercount]]
;                                                                                                       ^--- this MD node has value 10
; POSTPROCESS-REGCOUNT-FEWREGS-DAG: call {{.*}} @continuation.continue({{.*}} !continuation.registercount ![[registercount]]
; POSTPROCESS-REGCOUNT-DAG: define void @TraversalImpl1_1({{.*}} !continuation.registercount ![[intersection_registercount]]
;                                                                                               ^--- this MD node has value 30
; POSTPROCESS-REGCOUNT-DAG: call {{.*}} @continuation.continue({{.*}} !continuation.registercount ![[intersection_registercount]]
define void @TraversalImpl1_1(%struct._AmdTraversalResultData* noalias nocapture sret(%struct._AmdTraversalResultData) %agg.result, i32 %csp, %struct._AmdSystemData* noalias %data)  {
  call void (i64, ...) @continuation.continue(i64 0, i8 addrspace(21)* undef)
  ret void
}

; Function Attrs: nounwind
declare void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #0

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #2

; Function Attrs: nounwind
declare void @dx.op.callShader.struct.TheirParams(i32, i32, %struct.TheirParams*) #0
declare void @dx.op.callShader.struct.TheirParams2(i32, i32, %struct.TheirParams2*) #0
declare i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes*) #5

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }
attributes #4 = { alwaysinline }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.entryPoints = !{!18, !5, !34, !36, !38, !40, !42}
!continuation.maxPayloadRegisterCount = !{!45} ; SKIP_LINE_BY_DEFAULT

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
!1 = !{i32 1, i32 6}
!2 = !{!"lib", i32 6, i32 6}
!3 = !{!4, !7, null, null}
!4 = !{!5}
!5 = !{void ()* @main, !"main", null, null, !21}
!6 = !{i32 0, i32 4}
!7 = !{!8}
!8 = !{i32 0, %"class.RWTexture2D<vector<float, 4> >"* bitcast (%dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" to %"class.RWTexture2D<vector<float, 4> >"*), !"RenderTarget", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !9}
!9 = !{i32 0, i32 9}
!11 = !{!12}
!12 = !{i32 1, !13, !13}
!13 = !{}
!14 = !{!12, !15, !16}
!15 = !{i32 2, !13, !13}
!16 = !{i32 0, !13, !13}
!17 = !{!12, !15}
!18 = !{null, !"", null, !3, !19}
!19 = !{i32 0, i64 65536}
!21 = !{i32 8, i32 7, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!22 = !{i32 0}
!24 = !{i32 8, i32 10, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!26 = !{i32 8, i32 8, i32 5, !22}
!28 = !{i32 8, i32 11, i32 6, i32 16, i32 5, !22}
!30 = !{i32 8, i32 7, i32 5, !22}
!31 = !{!32, !32, i64 0}
!32 = !{!"omnipotent char", !33, i64 0}
!33 = !{!"Simple C/C++ TBAA"}
!34 = !{void (%struct.MyParams*)* @called, !"called", null, null, !35}
!35 = !{i32 8, i32 12}
!36 = !{void ()* @mainTrace, !"mainTrace", null, null, !37}
!37 = !{i32 8, i32 7}
!38 = !{void ()* @Intersection, !"Intersection", null, null, !39}
!39 = !{i32 8, i32 8, i32 5, !44}
!40 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @AnyHit, !"AnyHit", null, null, !41}
!41 = !{i32 8, i32 9, i32 5, !44}
!42 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !"ClosestHit", null, null, !43}
!43 = !{i32 8, i32 10, i32 5, !44}
!44 = !{i32 0}
!45 = !{i32 10}
