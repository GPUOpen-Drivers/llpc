; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -passes='dxil-cont-intrinsic-prepare,lint,dxil-cont-lgc-rt-op-converter,lint,inline,lint,lower-raytracing-pipeline,lint,sroa,lint,lower-await,lint,coro-early,dxil-coro-split,coro-cleanup,lint,legacy-cleanup-continuations,lint,dxil-cont-post-process,lint,continuations-lint,remove-types-metadata' \
; RUN:     -S %s --lint-abort-on-error | FileCheck -check-prefix=POSTPROCESS %s

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { %struct.SystemData }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.MyParams = type { i32 }
%dx.types.fouri32 = type { i32, i32, i32, i32 }
%dx.types.ResourceProperties = type { i32, i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

; Need _cont_ReportHit to get system data type
declare !pointeetys !25 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

declare i32 @_cont_GetContinuationStackAddr()

declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData)

declare %struct.DispatchSystemData @_AmdAwaitShader(i64, i64, %struct.DispatchSystemData)

declare !pointeetys !14 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*)

; Function Attrs: nounwind memory(none)
declare !pointeetys !16 <3 x i32> @_cont_DispatchRaysIndex3(%struct.DispatchSystemData* nocapture readnone) #0

; Function Attrs: nounwind memory(none)
declare !pointeetys !18 void @_AmdRestoreSystemData(%struct.DispatchSystemData*) #0

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) !pointeetys !19 {
  ret i32 5
}

define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32 %0) !pointeetys !20 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, i64 poison, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

define void @called(%struct.MyParams* %params) !pointeetys !21 {
  %i = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)
  %unpacked = call %dx.types.fouri32 @dx.op.unpack4x8.i32(i32 219, i8 1, i32 %i)
  %params_i = getelementptr %struct.MyParams, %struct.MyParams* %params, i32 0, i32 0
  %handle0 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %handle1 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %handle0)
  %handle2 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %handle1, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.callShader.struct.MyParams(i32 159, i32 2, %struct.MyParams* nonnull %params)
  %a = extractvalue %dx.types.fouri32 %unpacked, 0
  %b = extractvalue %dx.types.fouri32 %unpacked, 1
  %c = extractvalue %dx.types.fouri32 %unpacked, 2
  %d = extractvalue %dx.types.fouri32 %unpacked, 3
  %packed = call i32 @dx.op.pack4x8.i32(i32 220, i8 0, i32 %a, i32 %b, i32 %c, i32 %d)
  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %handle2, i32 0, i32 0, i32 undef, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00, float 1.000000e+00, i8 15)
  store i32 %packed, i32* %params_i, align 4
  ret void
}

; Function Attrs: nounwind
declare !pointeetys !23 void @dx.op.callShader.struct.MyParams(i32, i32, %struct.MyParams*) #1

; Function Attrs: nounwind memory(none)
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #0

; Function Attrs: nounwind memory(none)
declare %dx.types.fouri32 @dx.op.unpack4x8.i32(i32, i8, i32) #0

; Function Attrs: nounwind memory(none)
declare i32 @dx.op.pack4x8.i32(i32, i8, i32, i32, i32, i32) #0

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #0

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.f32(i32, %dx.types.Handle, i32, i32, i32, float, float, float, float, i8) #1

attributes #0 = { nounwind memory(none) }
attributes #1 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.entryPoints = !{!3, !6}
!continuation.maxPayloadRegisterCount = !{!13}
!lgc.rt.max.attribute.size = !{!26}

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
!1 = !{i32 1, i32 6}
!2 = !{!"lib", i32 6, i32 6}
!3 = !{null, !"", null, !4, !12}
!4 = !{!5, !9, null, null}
!5 = !{!6}
!6 = !{void (%struct.MyParams*)* @called, !"called", null, null, !7}
!7 = !{i32 8, i32 12, i32 6, i32 16, i32 7, i32 8, i32 5, !8}
!8 = !{i32 0}
!9 = !{!10}
!10 = !{i32 0, %"class.RWTexture2D<vector<float, 4> >"* bitcast (%dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" to %"class.RWTexture2D<vector<float, 4> >"*), !"RenderTarget", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !11}
!11 = !{i32 0, i32 9}
!12 = !{i32 0, i64 65536}
!13 = !{i32 30}
!14 = !{%struct.SystemData poison}
!15 = !{i32 0, %struct.SystemData poison}
!16 = !{%struct.DispatchSystemData poison}
!17 = !{i32 0, %struct.DispatchSystemData poison}
!18 = !{%struct.DispatchSystemData poison}
!19 = !{%struct.DispatchSystemData poison}
!20 = !{%struct.DispatchSystemData poison}
!21 = !{%struct.MyParams poison}
!22 = !{i32 0, %struct.MyParams poison}
!23 = !{%struct.MyParams poison}
!24 = !{i32 0, %struct.TraversalData poison}
!25 = !{%struct.TraversalData poison}
!26 = !{i32 8}
; POSTPROCESS-LABEL: define i32 @_cont_GetLocalRootIndex(
; POSTPROCESS-SAME: ptr [[DATA:%.*]]) #[[ATTR1:[0-9]+]] {
; POSTPROCESS-NEXT:    ret i32 5
;
;
; POSTPROCESS-LABEL: define void @called(
; POSTPROCESS-SAME: i32 [[CSPINIT:%.*]], i64 [[RETURNADDR:%.*]], [[STRUCT_DISPATCHSYSTEMDATA:%.*]] [[TMP0:%.*]], [2 x i32] [[PADDING:%.*]], [1 x i32] [[PAYLOAD:%.*]]) !continuation [[META17:![0-9]+]] !lgc.rt.shaderstage [[META18:![0-9]+]] !continuation.stacksize [[META14:![0-9]+]] {
; POSTPROCESS-NEXT:  AllocaSpillBB:
; POSTPROCESS-NEXT:    [[SYSTEM_DATA_ALLOCA:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; POSTPROCESS-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; POSTPROCESS-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], ptr [[SYSTEM_DATA_ALLOCA]], align 4
; POSTPROCESS-NEXT:    [[TMP1:%.*]] = load i32, ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    [[TMP2:%.*]] = add i32 [[TMP1]], 8
; POSTPROCESS-NEXT:    store i32 [[TMP2]], ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    [[TMP3:%.*]] = inttoptr i32 [[TMP1]] to ptr addrspace(21)
; POSTPROCESS-NEXT:    [[TMP4:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP3]], i32 0
; POSTPROCESS-NEXT:    store i64 [[RETURNADDR]], ptr addrspace(21) [[TMP4]], align 4
; POSTPROCESS-NEXT:    [[PAYLOAD_FCA_0_EXTRACT:%.*]] = extractvalue [1 x i32] [[PAYLOAD]], 0
; POSTPROCESS-NEXT:    [[DOTFCA_0_EXTRACT9:%.*]] = extractvalue [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP0]], 0
; POSTPROCESS-NEXT:    call void @amd.dx.setLocalRootIndex(i32 5)
; POSTPROCESS-NEXT:    [[TMP6:%.*]] = call <3 x i32> @_cont_DispatchRaysIndex3(ptr [[SYSTEM_DATA_ALLOCA]])
; POSTPROCESS-NEXT:    [[I:%.*]] = extractelement <3 x i32> [[TMP6]], i8 0
; POSTPROCESS-NEXT:    [[UNPACKED:%.*]] = call [[DX_TYPES_FOURI32:%.*]] @[[DX_OP_UNPACK4X8_I32:[a-zA-Z0-9_$\"\\.-]*[a-zA-Z_$\"\\.-][a-zA-Z0-9_$\"\\.-]*]](i32 219, i8 1, i32 [[I]])
; POSTPROCESS-NEXT:    [[HANDLE0:%.*]] = load [[DX_TYPES_HANDLE:%.*]], ptr @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
; POSTPROCESS-NEXT:    [[HANDLE1:%.*]] = call [[DX_TYPES_HANDLE]] @[[DX_OP_CREATEHANDLEFORLIB_DX_TYPES_HANDLE:[a-zA-Z0-9_$\"\\.-]*[a-zA-Z_$\"\\.-][a-zA-Z0-9_$\"\\.-]*]](i32 160, [[DX_TYPES_HANDLE]] [[HANDLE0]])
; POSTPROCESS-NEXT:    [[HANDLE2:%.*]] = call [[DX_TYPES_HANDLE]] @[[DX_OP_ANNOTATEHANDLE:[a-zA-Z0-9_$\"\\.-]*[a-zA-Z_$\"\\.-][a-zA-Z0-9_$\"\\.-]*]](i32 216, [[DX_TYPES_HANDLE]] [[HANDLE1]], [[DX_TYPES_RESOURCEPROPERTIES:%.*]] { i32 16, i32 0 })
; POSTPROCESS-NEXT:    [[DIS_DATA_I_FCA_0_INSERT:%.*]] = insertvalue [[STRUCT_DISPATCHSYSTEMDATA]] poison, i32 [[DOTFCA_0_EXTRACT9]], 0
; POSTPROCESS-NEXT:    [[DOTFCA_0_INSERT4:%.*]] = insertvalue [1 x i32] poison, i32 [[PAYLOAD_FCA_0_EXTRACT]], 0
; POSTPROCESS-NEXT:    [[TMP8:%.*]] = call i64 @continuation.getAddrAndMD(ptr @called.resume.0)
; POSTPROCESS-NEXT:    [[TMP7:%.*]] = load i32, ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    call void (...) @lgc.ilcps.continue(i64 2, i32 [[TMP7]], i64 [[TMP8]], i64 poison, [[STRUCT_DISPATCHSYSTEMDATA]] [[DIS_DATA_I_FCA_0_INSERT]], [2 x i32] poison, [1 x i32] [[DOTFCA_0_INSERT4]])
; POSTPROCESS-NEXT:    unreachable
;
;
; POSTPROCESS-LABEL: define dso_local void @called.resume.0(
; POSTPROCESS-SAME: i32 [[CSPINIT:%.*]], i64 [[TMP0:%.*]], { [[STRUCT_DISPATCHSYSTEMDATA:%.*]], [2 x i32], [1 x i32] } [[TMP1:%.*]]) !continuation [[META17]] !lgc.rt.shaderstage [[META18]] {
; POSTPROCESS-NEXT:  entryresume.0:
; POSTPROCESS-NEXT:    [[TMP16:%.*]] = alloca [[STRUCT_DISPATCHSYSTEMDATA]], align 8
; POSTPROCESS-NEXT:    [[CSP:%.*]] = alloca i32, align 4
; POSTPROCESS-NEXT:    store i32 [[CSPINIT]], ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    [[TMP3:%.*]] = extractvalue { [[STRUCT_DISPATCHSYSTEMDATA]], [2 x i32], [1 x i32] } [[TMP1]], 0
; POSTPROCESS-NEXT:    store [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP3]], ptr [[TMP16]], align 4
; POSTPROCESS-NEXT:    [[TMP13:%.*]] = load i32, ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    [[TMP2:%.*]] = add i32 [[TMP13]], -8
; POSTPROCESS-NEXT:    [[TMP4:%.*]] = extractvalue { [[STRUCT_DISPATCHSYSTEMDATA]], [2 x i32], [1 x i32] } [[TMP1]], 2
; POSTPROCESS-NEXT:    [[DOTFCA_0_EXTRACT:%.*]] = extractvalue [1 x i32] [[TMP4]], 0
; POSTPROCESS-NEXT:    [[TMP15:%.*]] = extractvalue { [[STRUCT_DISPATCHSYSTEMDATA]], [2 x i32], [1 x i32] } [[TMP1]], 0
; POSTPROCESS-NEXT:    [[DOTFCA_0_EXTRACT3:%.*]] = extractvalue [[STRUCT_DISPATCHSYSTEMDATA]] [[TMP15]], 0
; POSTPROCESS-NEXT:    call void @amd.dx.setLocalRootIndex(i32 5)
; POSTPROCESS-NEXT:    [[TMP5:%.*]] = inttoptr i32 [[TMP2]] to ptr addrspace(21)
; POSTPROCESS-NEXT:    [[TMP6:%.*]] = getelementptr i8, ptr addrspace(21) [[TMP5]], i32 0
; POSTPROCESS-NEXT:    [[RETURNADDR_RELOAD:%.*]] = load i64, ptr addrspace(21) [[TMP6]], align 4
; POSTPROCESS-NEXT:    [[HANDLE011:%.*]] = load [[DX_TYPES_HANDLE:%.*]], ptr @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
; POSTPROCESS-NEXT:    [[HANDLE110:%.*]] = call [[DX_TYPES_HANDLE]] @[[DX_OP_CREATEHANDLEFORLIB_DX_TYPES_HANDLE]](i32 160, [[DX_TYPES_HANDLE]] [[HANDLE011]])
; POSTPROCESS-NEXT:    [[HANDLE29:%.*]] = call [[DX_TYPES_HANDLE]] @[[DX_OP_ANNOTATEHANDLE]](i32 216, [[DX_TYPES_HANDLE]] [[HANDLE110]], [[DX_TYPES_RESOURCEPROPERTIES:%.*]] { i32 16, i32 0 })
; POSTPROCESS-NEXT:    [[TMP17:%.*]] = call <3 x i32> @_cont_DispatchRaysIndex3(ptr [[TMP16]])
; POSTPROCESS-NEXT:    [[I8:%.*]] = extractelement <3 x i32> [[TMP17]], i8 0
; POSTPROCESS-NEXT:    [[UNPACKED7:%.*]] = call [[DX_TYPES_FOURI32:%.*]] @[[DX_OP_UNPACK4X8_I32]](i32 219, i8 1, i32 [[I8]])
; POSTPROCESS-NEXT:    [[TMP7:%.*]] = call <3 x i32> @_cont_DispatchRaysIndex3(ptr [[TMP16]])
; POSTPROCESS-NEXT:    [[I6:%.*]] = extractelement <3 x i32> [[TMP7]], i8 0
; POSTPROCESS-NEXT:    [[UNPACKED5:%.*]] = call [[DX_TYPES_FOURI32]] @[[DX_OP_UNPACK4X8_I32]](i32 219, i8 1, i32 [[I6]])
; POSTPROCESS-NEXT:    [[TMP8:%.*]] = call <3 x i32> @_cont_DispatchRaysIndex3(ptr [[TMP16]])
; POSTPROCESS-NEXT:    [[I4:%.*]] = extractelement <3 x i32> [[TMP8]], i8 0
; POSTPROCESS-NEXT:    [[UNPACKED3:%.*]] = call [[DX_TYPES_FOURI32]] @[[DX_OP_UNPACK4X8_I32]](i32 219, i8 1, i32 [[I4]])
; POSTPROCESS-NEXT:    [[TMP9:%.*]] = call <3 x i32> @_cont_DispatchRaysIndex3(ptr [[TMP16]])
; POSTPROCESS-NEXT:    [[I2:%.*]] = extractelement <3 x i32> [[TMP9]], i8 0
; POSTPROCESS-NEXT:    [[UNPACKED1:%.*]] = call [[DX_TYPES_FOURI32]] @[[DX_OP_UNPACK4X8_I32]](i32 219, i8 1, i32 [[I2]])
; POSTPROCESS-NEXT:    [[A:%.*]] = extractvalue [[DX_TYPES_FOURI32]] [[UNPACKED7]], 0
; POSTPROCESS-NEXT:    [[B:%.*]] = extractvalue [[DX_TYPES_FOURI32]] [[UNPACKED5]], 1
; POSTPROCESS-NEXT:    [[C:%.*]] = extractvalue [[DX_TYPES_FOURI32]] [[UNPACKED3]], 2
; POSTPROCESS-NEXT:    [[D:%.*]] = extractvalue [[DX_TYPES_FOURI32]] [[UNPACKED1]], 3
; POSTPROCESS-NEXT:    [[PACKED:%.*]] = call i32 @dx.op.pack4x8.i32(i32 220, i8 0, i32 [[A]], i32 [[B]], i32 [[C]], i32 [[D]])
; POSTPROCESS-NEXT:    call void @dx.op.textureStore.f32(i32 67, [[DX_TYPES_HANDLE]] [[HANDLE29]], i32 0, i32 0, i32 undef, float 1.000000e+00, float 2.000000e+00, float 3.000000e+00, float 1.000000e+00, i8 15)
; POSTPROCESS-NEXT:    [[DOTFCA_0_INSERT:%.*]] = insertvalue [[STRUCT_DISPATCHSYSTEMDATA]] poison, i32 [[DOTFCA_0_EXTRACT3]], 0
; POSTPROCESS-NEXT:    [[DOTFCA_0_INSERT1:%.*]] = insertvalue [1 x i32] poison, i32 [[PACKED]], 0
; POSTPROCESS-NEXT:    [[TMP10:%.*]] = load i32, ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    [[TMP11:%.*]] = add i32 [[TMP10]], -8
; POSTPROCESS-NEXT:    store i32 [[TMP11]], ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    [[TMP12:%.*]] = load i32, ptr [[CSP]], align 4
; POSTPROCESS-NEXT:    call void (...) @lgc.ilcps.continue(i64 [[RETURNADDR_RELOAD]], i32 [[TMP12]], i64 poison, [[STRUCT_DISPATCHSYSTEMDATA]] [[DOTFCA_0_INSERT]], [2 x i32] poison, [1 x i32] [[DOTFCA_0_INSERT1]])
; POSTPROCESS-NEXT:    unreachable
;
