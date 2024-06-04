; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -passes="lower-raytracing-pipeline,lint,remove-types-metadata" -S %s --lint-abort-on-error | FileCheck -check-prefix=LOWERRAYTRACINGPIPELINE %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { <3 x i32> }
%struct.TraversalData = type { %struct.SystemData, %struct.HitData, <3 x float>, <3 x float>, float, i64 }
%struct.SystemData = type { %struct.DispatchSystemData, %struct.BuiltInTriangleIntersectionAttributes }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.HitData = type { float, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

declare void @continuation.waitContinue(i64, i64, ...) noreturn

declare !types !24 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data)

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !types !{!"function", !"void", !{i32 0, %struct.DispatchSystemData poison}} {
  %dispatchPayloadPtr = getelementptr inbounds %struct.DispatchSystemData, ptr %data, i32 0, i32 0
  %dispatchPayload = load <3 x i32>, ptr %dispatchPayloadPtr, align 4
  %deadLaneDispatchPayload = insertelement <3 x i32> %dispatchPayload, i32 -11, i32 0
  %systemData = insertvalue %struct.SystemData poison, <3 x i32> %deadLaneDispatchPayload, 0, 0
  %addrSuffix = load i32, ptr %data, align 4
  %addr = zext i32 %addrSuffix to i64
  call void @continuation.waitContinue(i64 %addr, i64 -1, %struct.SystemData %systemData)
  unreachable
}

; Function Attrs: nounwind
define void @MyRayGen() #0 !lgc.rt.shaderstage !20 {
  ret void
}

attributes #0 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.typeAnnotations = !{!10}
!dx.entryPoints = !{!21}

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
!20 = !{i32 0}
!21 = !{void ()* @MyRayGen, !"MyRayGen", null, null, !22}
!22 = !{i32 8, i32 7, i32 5, !23}
!23 = !{i32 0}
!24 = !{!"function", i32 poison, !25}
!25 = !{i32 0, %struct.DispatchSystemData poison}
; LOWERRAYTRACINGPIPELINE-LABEL: define void @MyRayGen(
; LOWERRAYTRACINGPIPELINE-SAME: i64 [[RETURNADDR:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[TMP0:%.*]]) #[[ATTR1:[0-9]+]] !lgc.rt.shaderstage [[META16:![0-9]+]] !continuation.entry [[META13:![0-9]+]] !continuation.registercount [[META16]] !continuation [[META19:![0-9]+]] {
; LOWERRAYTRACINGPIPELINE-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; LOWERRAYTRACINGPIPELINE-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[DISPATCHPAYLOAD_I:%.*]] = load <3 x i32>, ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[DEADLANEDISPATCHPAYLOAD_I:%.*]] = insertelement <3 x i32> [[DISPATCHPAYLOAD_I]], i32 -11, i32 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[SYSTEMDATA_I:%.*]] = insertvalue [[STRUCT_SYSTEMDATA:%.*]] poison, <3 x i32> [[DEADLANEDISPATCHPAYLOAD_I]], 0, 0
; LOWERRAYTRACINGPIPELINE-NEXT:    [[ADDRSUFFIX_I:%.*]] = load i32, ptr [[SYSTEM_DATA_ALLOCA]], align 4
; LOWERRAYTRACINGPIPELINE-NEXT:    [[ADDR_I:%.*]] = zext i32 [[ADDRSUFFIX_I]] to i64
; LOWERRAYTRACINGPIPELINE-NEXT:    call void @continuation.waitContinue(i64 [[ADDR_I]], i64 -1, [[STRUCT_SYSTEMDATA]] [[SYSTEMDATA_I]]) #[[ATTR3:[0-9]+]]
; LOWERRAYTRACINGPIPELINE-NEXT:    unreachable
; LOWERRAYTRACINGPIPELINE:       _cont_ExitRayGen.exit:
; LOWERRAYTRACINGPIPELINE-NEXT:    ret void
;
