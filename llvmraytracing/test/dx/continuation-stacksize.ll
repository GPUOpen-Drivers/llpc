; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,dxil-cont-post-process,lint,continuations-lint,remove-types-metadata' \
; RUN:     -S %s --lint-abort-on-error | FileCheck -check-prefix=POSTPROCESS-STACKSIZE %s
; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,remove-types-metadata' \
; RUN:     -S %s --lint-abort-on-error | FileCheck -check-prefix=CLEANUP-STATESIZE %s

; The order of metadata on functions is non-deterministic, so make two different runs to match both of them.

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { %struct.SystemData }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.TheirParams = type { [64 x i32] }
%struct.RayPayload = type { [68 x i32] }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.MyParams = type { [48 x i32] }
%struct.TheirParams2 = type { [65 x i32] }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

; Function Attrs: alwaysinline
declare i32 @_cont_GetContinuationStackAddr() #0

declare !pointeetys !33 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #0

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #0

; Function Attrs: alwaysinline
declare !pointeetys !17 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

; Function Attrs: nounwind memory(none)
declare !pointeetys !19 void @_AmdRestoreSystemData(%struct.DispatchSystemData*) #1

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !{%struct.DispatchSystemData poison} {
  ret void
}

; Function Attrs: alwaysinline
define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #0 !pointeetys !21 {
  ret i32 5
}

; Function Attrs: alwaysinline
define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #0 !pointeetys !22 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  ret void
}

; Function Attrs: alwaysinline
define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) #0 !pointeetys !23 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

; LOWERRAYTRACINGPIPELINE-STACKSIZE-DAG: define void @main(%struct.DispatchSystemData %0){{.*}} !continuation.stacksize ![[main_stacksize:[0-9]+]]
; LOWERRAYTRACINGPIPELINE-STACKSIZE-DAG: ![[main_stacksize]] = !{i32 140}

; POSTPROCESS-STACKSIZE-DAG: define void @main({{.*}} !continuation.stacksize ![[main_stacksize:[0-9]+]]
; POSTPROCESS-STACKSIZE-DAG: ![[main_stacksize]] = !{i32 140}
; CLEANUP-STATESIZE-DAG: define void @main({{.*}} !continuation.state ![[main_state:[0-9]+]]
; CLEANUP-STATESIZE-DAG: ![[main_state]] = !{i32 0}

define void @main() {
  %params = alloca %struct.TheirParams, align 4
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)
  ret void
}

; LOWERRAYTRACINGPIPELINE-STACKSIZE-DAG: define void @mainTrace(%struct.DispatchSystemData %0){{.*}} !continuation.stacksize ![[maintrace_stacksize:[0-9]+]]
; LOWERRAYTRACINGPIPELINE-STACKSIZE-DAG: ![[maintrace_stacksize]] = !{i32 180}

; CLEANUP-STACKSIZE-DAG: define void @mainTrace{{.*}}%struct.DispatchSystemData{{.*}} !continuation.stacksize ![[maintrace_stacksize:[0-9]+]]
; CLEANUP-STACKSIZE-DAG: ![[maintrace_stacksize]] = !{i32 180}
; CLEANUP-STATESIZE-DAG: define void @mainTrace{{.*}}%struct.DispatchSystemData{{.*}} !continuation.state ![[main_state]]

; SAVESTATE-STACKSIZE-DAG: define void @mainTrace(%struct.DispatchSystemData %0){{.*}} !continuation.stacksize ![[maintrace_stacksize:[0-9]+]]
; SAVESTATE-STACKSIZE-DAG: ![[maintrace_stacksize]] = !{i32 180}
; SAVESTATE-STATESIZE-DAG: define void @mainTrace(%struct.DispatchSystemData %0){{.*}} !continuation.state ![[main_state]]

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

; LOWERRAYTRACINGPIPELINE-STACKSIZE-DAG: define %struct.DispatchSystemData @called({{.*}}%struct.DispatchSystemData %0){{.*}} !continuation.stacksize ![[called_stacksize:[0-9]+]]
; LOWERRAYTRACINGPIPELINE-STACKSIZE-DAG: ![[called_stacksize]] = !{i32 144}

; CLEANUP-STACKSIZE-DAG: define void @called({{.*}}%struct.DispatchSystemData %0){{.*}} !continuation.stacksize ![[called_stacksize:[0-9]+]]
; CLEANUP-STACKSIZE-DAG: ![[called_stacksize]] = !{i32 348}
; CLEANUP-STATESIZE-DAG: define void @called{{.*}}%struct.DispatchSystemData{{.*}} !continuation.state ![[called_state:[0-9]+]]
; CLEANUP-STATESIZE-DAG: ![[called_state]] = !{i32 204}

; SAVESTATE-STACKSIZE-DAG: define void @called({{.*}}%struct.DispatchSystemData %0){{.*}} !continuation.stacksize ![[called_stacksize:[0-9]+]]
; SAVESTATE-STACKSIZE-DAG: ![[called_stacksize]] = !{i32 348}
; SAVESTATE-STATESIZE-DAG: define void @called{{.*}}%struct.DispatchSystemData{{.*}} !continuation.state ![[called_state:[0-9]+]]
; SAVESTATE-STATESIZE-DAG: ![[called_state]] = !{i32 204}

define void @called(%struct.MyParams* %arg) !pointeetys !24 {
  %params = alloca %struct.TheirParams2, align 4
  call void @dx.op.callShader.struct.TheirParams2(i32 159, i32 2, %struct.TheirParams2* nonnull %params)
  ret void
}

; Function Attrs: nounwind
declare !pointeetys !26 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #2

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

; Function Attrs: nounwind memory(read)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #3

; Function Attrs: nounwind
declare !pointeetys !28 void @dx.op.callShader.struct.TheirParams(i32, i32, %struct.TheirParams*) #2

; Function Attrs: nounwind
declare !pointeetys !30 void @dx.op.callShader.struct.TheirParams2(i32, i32, %struct.TheirParams2*) #2

attributes #0 = { alwaysinline }
attributes #1 = { nounwind memory(none) }
attributes #2 = { nounwind }
attributes #3 = { nounwind memory(read) }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.entryPoints = !{!3, !6, !13, !15}

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
!17 = !{%struct.SystemData poison}
!18 = !{i32 0, %struct.SystemData poison}
!19 = !{%struct.DispatchSystemData poison}
!20 = !{i32 0, %struct.DispatchSystemData poison}
!21 = !{%struct.DispatchSystemData poison}
!22 = !{%struct.DispatchSystemData poison}
!23 = !{%struct.DispatchSystemData poison}
!24 = !{%struct.MyParams poison}
!25 = !{i32 0, %struct.MyParams poison}
!26 = !{%struct.RayPayload poison}
!27 = !{i32 0, %struct.RayPayload poison}
!28 = !{%struct.TheirParams poison}
!29 = !{i32 0, %struct.TheirParams poison}
!30 = !{%struct.TheirParams2 poison}
!31 = !{i32 0, %struct.TheirParams2 poison}
!32 = !{i32 0, %struct.TraversalData poison}
!33 = !{%struct.TraversalData poison}
