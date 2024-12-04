; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt --verify-each -passes="dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata" -S %s --lint-abort-on-error | FileCheck -check-prefix=LOWERRAYTRACINGPIPELINE-CPS %s
; RUN: opt --verify-each -passes="dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,lgc-cps-jump-inliner,lint,remove-types-metadata" -S %s --lint-abort-on-error | FileCheck -check-prefix=JUMP-INLINER-CPS %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { i32 }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.TraversalData = type { %struct.SystemData, %struct.HitData, <3 x float>, <3 x float>, float }
%struct.HitData = type { float, i32 }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.TheirParams = type { i32 }
%struct.Payload = type {}
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@debug_global = external global i32

@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

declare i32 @lgc.rt.shader.index()

declare i32 @_cont_GetContinuationStackAddr()

declare !pointeetys !13 <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData*)

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) !pointeetys !13 {
; LOWERRAYTRACINGPIPELINE-CPS-LABEL: define i32 @_cont_GetLocalRootIndex(
; LOWERRAYTRACINGPIPELINE-CPS-SAME: ptr [[DATA:%.*]]) {
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    ret i32 5
;
; JUMP-INLINER-CPS-LABEL: define i32 @_cont_GetLocalRootIndex(
; JUMP-INLINER-CPS-SAME: ptr [[DATA:%.*]]) {
; JUMP-INLINER-CPS-NEXT:    ret i32 5
;
  ret i32 5
}

; Need _cont_ReportHit to get system data type
declare !pointeetys !21 i1 @_cont_ReportHit(%struct.AnyHitTraversalData* %data, float %t, i32 %hitKind)

declare void @lgc.cps.jump(...) #1
declare i32 @get.ret.addr()

declare !pointeetys !15 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*)

declare !pointeetys !13 void @_AmdRestoreSystemData(%struct.DispatchSystemData*)
declare i32 @_AmdGetFuncAddrCallable()

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !13 {
  ret void
}

define internal void @Callable(%struct.Payload* %payload) !pointeetys !23 !lgc.rt.shaderstage !25 {
; LOWERRAYTRACINGPIPELINE-CPS-LABEL: define internal void @Callable(
; LOWERRAYTRACINGPIPELINE-CPS-SAME: i32 [[RETURNADDR:%.*]], i32 [[SHADER_INDEX:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[SYSTEM_DATA:%.*]], {} [[HIT_ATTRS:%.*]], [0 x i32] [[PADDING:%.*]], [0 x i32] [[PAYLOAD:%.*]]) !lgc.rt.shaderstage [[META15:![0-9]+]] !lgc.cps [[META16:![0-9]+]] !continuation.registercount [[META8:![0-9]+]] !continuation [[META17:![0-9]+]] {
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:  entry:
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [0 x i32], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[TMP0:%.*]] = alloca [[STRUCT_PAYLOAD:%.*]], align 8
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[SYSTEM_DATA]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    store i32 [[SHADER_INDEX]], ptr @debug_global, align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[TMP1:%.*]] = load [[STRUCT_DISPATCHSYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    call void (...) @lgc.cps.jump(i32 [[RETURNADDR]], i32 6, i32 poison, i32 poison, i32 poison, [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP1]]), !continuation.registercount [[META8]]
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    unreachable
;
entry:
  %val = call i32 @lgc.rt.shader.index()
  store i32 %val, ptr @debug_global
  ret void
}

define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) !pointeetys !13 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %callable.addr = call i32 @_AmdGetFuncAddrCallable()
  %ret.addr = call i32 @get.ret.addr()
  call void (...) @lgc.cps.jump(i32 %callable.addr, i32 2, i32 poison, i32 %ret.addr, i32 999, %struct.DispatchSystemData %dis_data, {} poison, [0 x i32] poison, [0 x i32] poison)
  unreachable
}

define void @main() {
; LOWERRAYTRACINGPIPELINE-CPS-LABEL: define void @main(
; LOWERRAYTRACINGPIPELINE-CPS-SAME: i32 [[RETURNADDR:%.*]], i32 [[SHADER_INDEX:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[TMP0:%.*]]) !lgc.rt.shaderstage [[META8]] !lgc.cps [[META18:![0-9]+]] !continuation.registercount [[META8]] !continuation [[META19:![0-9]+]] {
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[PARAMS:%.*]] = alloca [[STRUCT_THEIRPARAMS:%.*]], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [1 x i32], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    call void @amd.dx.setLocalRootIndex(i32 0)
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[DIS_DATA_I:%.*]] = load [[STRUCT_DISPATCHSYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[TMP2:%.*]] = call i32 (...) @lgc.cps.as.continuation.reference(ptr @Callable)
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    [[RET_ADDR_I:%.*]] = call i32 @get.ret.addr()
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    call void (...) @lgc.cps.jump(i32 [[TMP2]], i32 2, i32 poison, i32 [[RET_ADDR_I]], i32 999, [[STRUCT_DISPATCHSYSTEMDATA]] [[DIS_DATA_I]], {} poison, [0 x i32] poison, [0 x i32] poison)
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    unreachable
; LOWERRAYTRACINGPIPELINE-CPS:       _cont_CallShader.exit:
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    call void @lgc.cps.complete()
; LOWERRAYTRACINGPIPELINE-CPS-NEXT:    unreachable
;
; JUMP-INLINER-CPS-LABEL: define void @main(
; JUMP-INLINER-CPS-SAME: i32 [[RETURNADDR:%.*]], i32 [[SHADER_INDEX:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[TMP0:%.*]]) !lgc.rt.shaderstage [[META8:![0-9]+]] !lgc.cps [[META15:![0-9]+]] !continuation.registercount [[META8]] !continuation [[META16:![0-9]+]] {
; JUMP-INLINER-CPS-NEXT:    [[SYSTEM_DATA_ALLOCA_I:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; JUMP-INLINER-CPS-NEXT:    [[PARAMS:%.*]] = alloca [[STRUCT_THEIRPARAMS:%.*]], align 4
; JUMP-INLINER-CPS-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; JUMP-INLINER-CPS-NEXT:    [[PAYLOAD_SERIALIZATION_ALLOCA:%.*]] = alloca [1 x i32], align 4
; JUMP-INLINER-CPS-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; JUMP-INLINER-CPS-NEXT:    call void @amd.dx.setLocalRootIndex(i32 0)
; JUMP-INLINER-CPS-NEXT:    [[DIS_DATA_I:%.*]] = load [[STRUCT_DISPATCHSYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; JUMP-INLINER-CPS-NEXT:    [[RET_ADDR_I:%.*]] = call i32 @get.ret.addr()
; JUMP-INLINER-CPS-NEXT:    call void @llvm.lifetime.start.p0(i64 4, ptr [[SYSTEM_DATA_ALLOCA_I]])
; JUMP-INLINER-CPS-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[DIS_DATA_I]], ptr [[SYSTEM_DATA_ALLOCA_I]], align 4
; JUMP-INLINER-CPS-NEXT:    store i32 999, ptr @debug_global, align 4
; JUMP-INLINER-CPS-NEXT:    [[TMP2:%.*]] = load [[STRUCT_DISPATCHSYSTEMDATA]], ptr [[SYSTEM_DATA_ALLOCA_I]], align 4
; JUMP-INLINER-CPS-NEXT:    call void (...) @lgc.cps.jump(i32 [[RET_ADDR_I]], i32 6, i32 poison, i32 poison, i32 poison, [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP2]]), !continuation.registercount [[META8]]
; JUMP-INLINER-CPS-NEXT:    unreachable
; JUMP-INLINER-CPS:       Callable.exit:
; JUMP-INLINER-CPS-NEXT:    unreachable
; JUMP-INLINER-CPS:       _cont_CallShader.exit:
; JUMP-INLINER-CPS-NEXT:    call void @lgc.cps.complete()
; JUMP-INLINER-CPS-NEXT:    unreachable
;
  %params = alloca %struct.TheirParams, align 4
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)
  ret void
}

; Function Attrs: nounwind
declare !pointeetys !19 void @dx.op.callShader.struct.TheirParams(i32, i32, %struct.TheirParams*) #0

attributes #0 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.entryPoints = !{!3, !6}
!lgc.cps.module = !{}

attributes #1 = { noreturn }

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
!13 = !{%struct.DispatchSystemData poison}
!15 = !{%struct.SystemData poison}
!19 = !{%struct.TheirParams poison}
!21 = !{%struct.AnyHitTraversalData poison}
!23 = !{%struct.Payload poison}
!25 = !{i32 5}
