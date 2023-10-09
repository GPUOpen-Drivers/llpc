; RUN: not --crash opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,remove-types-metadata' -S %s 2>&1 | FileCheck %s

; CHECK: Invalid system data struct: Did not contain the needed struct type

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.TraversalData = type { %struct.SystemData }
%struct.SystemData = type { i32 }
%struct.DispatchSystemData = type { i32 }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.RayPayload = type { <4 x float> }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

declare i64 @_AmdGetTraversalAddr() #0

declare !types !31 %struct.TraversalData @_AmdAnyHit(i64, %struct.TraversalData*) #0

declare i32 @_cont_GetContinuationStackAddr() #0

declare !types !33 i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*) #0

declare %struct.DispatchSystemData @_AmdTraversal(%struct.TraversalData) #0

declare %struct.DispatchSystemData @_cont_SetupRayGen() #0

declare void @_AmdEnqueue(i64, %struct.SystemData) #0

declare void @_AmdWaitEnqueue(i64, i64, %struct.SystemData) #0

declare void @_AmdEnqueueAnyHit(i64, %struct.TraversalData) #0

declare !types !35 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #0

declare !types !37 void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #0

declare !types !38 i1 @_cont_IsEndSearch(%struct.TraversalData*) #0

declare !types !39 i32 @_cont_HitKind(%struct.SystemData*) #0

define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #0 !types !40 {
  %sys_data = insertvalue %struct.SystemData undef, i32 1, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_AmdTraversal(%struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  ret void
}

define i1 @_cont_ReportHit(%struct.TraversalData* %data, float %0, i32 %1) #0 !types !41 {
  ret i1 true
}

; Function Attrs: nounwind
define void @"\01?MyRaygenShader@@YAXXZ"() #1 {
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = bitcast %struct.RayPayload* %3 to i8*
  %5 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  store <4 x float> zeroinitializer, <4 x float>* %5, align 4, !tbaa !42
  %6 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)
  %7 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %6, %dx.types.ResourceProperties { i32 16, i32 0 })
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %7, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)
  ret void
}

; Function Attrs: nounwind
define void @"\01?MyClosestHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z"(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #1 !types !45 {
  %1 = getelementptr inbounds %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %attr, i32 0, i32 0
  %2 = load <2 x float>, <2 x float>* %1, align 4
  %3 = extractelement <2 x float> %2, i32 0
  %4 = fsub fast float 1.000000e+00, %3
  %5 = extractelement <2 x float> %2, i32 1
  %6 = fsub fast float %4, %5
  %7 = insertelement <4 x float> undef, float %6, i64 0
  %8 = insertelement <4 x float> %7, float %3, i64 1
  %9 = insertelement <4 x float> %8, float %5, i64 2
  %10 = insertelement <4 x float> %9, float 1.000000e+00, i64 3
  %11 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  store <4 x float> %10, <4 x float>* %11, align 4
  ret void
}

; Function Attrs: nounwind
define void @"\01?MyAnyHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z"(%struct.RayPayload* noalias nocapture %payload, %struct.BuiltInTriangleIntersectionAttributes* nocapture readnone %attr) #1 !types !45 {
  %1 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  %2 = load <4 x float>, <4 x float>* %1, align 4
  %3 = call float @dx.op.objectRayOrigin.f32(i32 149, i8 0)
  %4 = call float @dx.op.objectRayDirection.f32(i32 150, i8 0)
  %5 = call float @dx.op.rayTCurrent.f32(i32 154)
  %6 = fmul fast float %5, %4
  %7 = fadd fast float %6, %3
  %8 = fcmp fast ogt float %7, 0.000000e+00
  br i1 %8, label %9, label %10

9:                                                ; preds = %0
  store <4 x float> %2, <4 x float>* %1, align 4
  call void @dx.op.acceptHitAndEndSearch(i32 156)
  unreachable

10:                                               ; preds = %0
  store <4 x float> %2, <4 x float>* %1, align 4
  ret void
}

; Function Attrs: nounwind
define void @"\01?MyIntersectionShader@@YAXXZ"() #1 {
  %1 = alloca %struct.BuiltInTriangleIntersectionAttributes, align 4
  %2 = call float @dx.op.rayTCurrent.f32(i32 154)
  %3 = bitcast %struct.BuiltInTriangleIntersectionAttributes* %1 to i8*
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %3) #2
  %4 = call i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32 158, float %2, i32 0, %struct.BuiltInTriangleIntersectionAttributes* nonnull %1)
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %3) #2
  ret void
}

; Function Attrs: nounwind
define void @"\01?MyMissShader@@YAXURayPayload@@@Z"(%struct.RayPayload* noalias nocapture %payload) #1 !types !48 {
  %1 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %payload, i32 0, i32 0
  store <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x float>* %1, align 4
  ret void
}

; Function Attrs: nounwind
declare !types !49 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #2

; Function Attrs: nounwind
declare void @dx.op.textureStore.f32(i32, %dx.types.Handle, i32, i32, i32, float, float, float, float, i8) #2

; Function Attrs: nounwind memory(none)
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #3

; Function Attrs: nounwind memory(none)
declare float @dx.op.objectRayDirection.f32(i32, i8) #3

; Function Attrs: nounwind memory(none)
declare float @dx.op.objectRayOrigin.f32(i32, i8) #3

; Function Attrs: nounwind memory(read)
declare float @dx.op.rayTCurrent.f32(i32) #4

declare void @dx.op.acceptHitAndEndSearch(i32) #0

; Function Attrs: nounwind
declare !types !50 i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes*) #2

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #3

; Function Attrs: nounwind memory(read)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #4

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !types !51 void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #5

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !types !51 void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #5

attributes #0 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="0" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { nounwind }
attributes #3 = { nounwind memory(none) }
attributes #4 = { nounwind memory(read) }
attributes #5 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.typeAnnotations = !{!10}
!dx.entryPoints = !{!18, !20, !23, !25, !27, !29}

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
!10 = !{i32 1, void ()* @"\01?MyRaygenShader@@YAXXZ", !11, void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @"\01?MyClosestHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", !14, void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @"\01?MyAnyHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", !14, void ()* @"\01?MyIntersectionShader@@YAXXZ", !11, void (%struct.RayPayload*)* @"\01?MyMissShader@@YAXURayPayload@@@Z", !17}
!11 = !{!12}
!12 = !{i32 1, !13, !13}
!13 = !{}
!14 = !{!12, !15, !16}
!15 = !{i32 2, !13, !13}
!16 = !{i32 0, !13, !13}
!17 = !{!12, !15}
!18 = !{null, !"", null, !3, !19}
!19 = !{i32 0, i64 65536}
!20 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @"\01?MyAnyHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", !"\01?MyAnyHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", null, null, !21}
!21 = !{i32 8, i32 9, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!22 = !{i32 0}
!23 = !{void (%struct.RayPayload*, %struct.BuiltInTriangleIntersectionAttributes*)* @"\01?MyClosestHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", !"\01?MyClosestHitShader@@YAXURayPayload@@UBuiltInTriangleIntersectionAttributes@@@Z", null, null, !24}
!24 = !{i32 8, i32 10, i32 6, i32 16, i32 7, i32 8, i32 5, !22}
!25 = !{void ()* @"\01?MyIntersectionShader@@YAXXZ", !"\01?MyIntersectionShader@@YAXXZ", null, null, !26}
!26 = !{i32 8, i32 8, i32 5, !22}
!27 = !{void (%struct.RayPayload*)* @"\01?MyMissShader@@YAXURayPayload@@@Z", !"\01?MyMissShader@@YAXURayPayload@@@Z", null, null, !28}
!28 = !{i32 8, i32 11, i32 6, i32 16, i32 5, !22}
!29 = !{void ()* @"\01?MyRaygenShader@@YAXXZ", !"\01?MyRaygenShader@@YAXXZ", null, null, !30}
!30 = !{i32 8, i32 7, i32 5, !22}
!31 = !{!"function", %struct.TraversalData poison, i64 poison, !32}
!32 = !{i32 0, %struct.TraversalData poison}
!33 = !{!"function", i32 poison, !34}
!34 = !{i32 0, %struct.DispatchSystemData poison}
!35 = !{!"function", %struct.BuiltInTriangleIntersectionAttributes poison, !36}
!36 = !{i32 0, %struct.SystemData poison}
!37 = !{!"function", !"void", !36, %struct.BuiltInTriangleIntersectionAttributes poison}
!38 = !{!"function", i1 poison, !32}
!39 = !{!"function", i32 poison, !36}
!40 = !{!"function", !"void", !34, i64 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison}
!41 = !{!"function", i1 poison, !32, float poison, i32 poison}
!42 = !{!43, !43, i64 0}
!43 = !{!"omnipotent char", !44, i64 0}
!44 = !{!"Simple C/C++ TBAA"}
!45 = !{!"function", !"void", !46, !47}
!46 = !{i32 0, %struct.RayPayload poison}
!47 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
!48 = !{!"function", !"void", !46}
!49 = !{!"function", !"void", i32 poison, %dx.types.Handle poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, !46}
!50 = !{!"function", i1 poison, i32 poison, float poison, i32 poison, !47}
!51 = !{!"function", !"void", i64 poison, !52}
!52 = !{i32 0, i8 poison}
