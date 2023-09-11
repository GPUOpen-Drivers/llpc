; RUN: opt --opaque-pointers=0 --enforce-pointer-metadata=1 --verify-each -passes='add-types-metadata,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,inline,lint,dxil-cont-pre-coroutine,lint,sroa,lint,lower-await,lint,coro-early,coro-split,coro-cleanup,lint,cleanup-continuations,lint,register-buffer,lint,save-continuation-state,lint,remove-types-metadata' \
; RUN:     -S %s 2>%t.stderr | FileCheck %s
; RUN: count 0 < %t.stderr

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.MyParams = type { [48 x i32] }
%struct.TheirParams = type { [64 x i32] }
%struct.TheirParams2 = type { [65 x i32] }
%struct.RayPayload = type { [68 x i32] }
%dx.types.Handle = type { i8* }
%dx.types.ResourceProperties = type { i32, i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

%struct.DispatchSystemData = type { i32 }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.TraversalData = type { %struct.SystemData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }

declare i32 @_cont_GetContinuationStackAddr() #4
declare %struct.DispatchSystemData @_cont_SetupRayGen() #4
declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #4
declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #4
declare %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #4
declare void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data) #1

; CHECK-DAG: @main = alias void (), void ()* @"85355B95AFDCDC6D:main"
@"main" = alias void (), void ()* @"85355B95AFDCDC6D:main"
; CHECK-DAG: @mainTrace = alias void (), void ()* @"85355B95AFDCDC6D:mainTrace"
@"mainTrace" = alias void (), void ()* @"85355B95AFDCDC6D:mainTrace"
; CHECK-DAG: @called = alias void (%struct.MyParams*), bitcast (void (i32, i64, %struct.DispatchSystemData)* @"85355B95AFDCDC6D:called" to void (%struct.MyParams*)*)
@"called" = alias void (%struct.MyParams*), void (%struct.MyParams*)* @"85355B95AFDCDC6D:called"

define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #4 {
  ret i32 5
}

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float) #4 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data
  ret void
}

define void @_cont_CallShader(%struct.DispatchSystemData* %data, i32) #4 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data
  %newdata = call %struct.DispatchSystemData @_AmdAwaitShader(i64 2, %struct.DispatchSystemData %dis_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

; CHECK: define void @"85355B95AFDCDC6D:main"()
; CHECK: define void @"85355B95AFDCDC6D:main.resume.0"(i32 %0, %struct.DispatchSystemData %1)
define void @"85355B95AFDCDC6D:main"() {
  %params = alloca %struct.TheirParams, align 4
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
  ret void
}

; CHECK: define void @"85355B95AFDCDC6D:mainTrace"()
; CHECK: define void @"85355B95AFDCDC6D:mainTrace.resume.0"(i32 %0, %struct.DispatchSystemData %1)
define void @"85355B95AFDCDC6D:mainTrace"() {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  %5 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)  ; CreateHandleForLib(Resource)
  %6 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %5, %dx.types.ResourceProperties { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %6, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)  ; TraceRay(AccelerationStructure,RayFlags,InstanceInclusionMask,RayContributionToHitGroupIndex,MultiplierForGeometryContributionToShaderIndex,MissShaderIndex,Origin_X,Origin_Y,Origin_Z,TMin,Direction_X,Direction_Y,Direction_Z,TMax,payload)
  ret void
}

; CHECK: define void @"85355B95AFDCDC6D:called"(i32 %cspInit, i64 %returnAddr, %struct.DispatchSystemData %0)
define void @"85355B95AFDCDC6D:called"(%struct.MyParams* %arg) {
  %params = alloca %struct.TheirParams2, align 4
  call void @dx.op.callShader.struct.TheirParams2(i32 159, i32 2, %struct.TheirParams2* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
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

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }
attributes #4 = { alwaysinline }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.entryPoints = !{!18, !5, !34, !36}

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
!1 = !{i32 1, i32 6}
!2 = !{!"lib", i32 6, i32 6}
!3 = !{!4, !7, null, null}
!4 = !{!5}
!5 = !{void ()* @"85355B95AFDCDC6D:main", !"main", null, null, !21}
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
!34 = !{void (%struct.MyParams*)* @"85355B95AFDCDC6D:called", !"called", null, null, !35}
!35 = !{i32 8, i32 12}
!36 = !{void ()* @"85355B95AFDCDC6D:mainTrace", !"mainTrace", null, null, !37}
!37 = !{i32 8, i32 7}
