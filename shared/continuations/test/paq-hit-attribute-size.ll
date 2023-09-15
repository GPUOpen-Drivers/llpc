; Test payload serialization layouts in presence of different max hit attribute
; size metadata.
;
; Default run checking serialization layouts and their usage:
; RUN: grep -v INVALID %s | opt -debug-only=lower-raytracing-pipeline --opaque-pointers=0 --enforce-pointer-metadata=1 --verify-each -passes='add-types-metadata,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S 2>&1 | FileCheck %s
; Check that hit attributes violating the max size are detected and crash:
; RUN: not --crash opt --opaque-pointers=0 --enforce-pointer-metadata=1 --verify-each -passes='add-types-metadata,dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S %s 2>&1 | FileCheck %s --check-prefix INVALID

; INVALID: Hit attributes are too large!

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.MyPayload = type { float, i32, double }
%struct.Attributes2DWords = type { [2 x i32] }
%struct.Attributes4DWords = type { [4 x i32] }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?myAccelerationStructure@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?gOutput@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

; CHECK: %struct.MyPayload.attr_max_4_i32s.layout_0_caller_out = type { [6 x i32] }
; CHECK: %struct.MyPayload.attr_max_8_i32s.layout_0_caller_out = type { [10 x i32] }
; CHECK: %struct.MyPayload.attr_max_2_i32s.layout_0_caller_out = type { [4 x i32] }

; The actual size matches the max size for this one, so the layout_2_anyhit_out_accept layout
; is not specialized, thus no payload_attr_N_i32s suffix.
; CHECK-LABEL: define {{.*}} @AnyHit2DWordsMax2DWords(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_2_i32s.layout_2_anyhit_out_accept
define void @AnyHit2DWordsMax2DWords(%struct.MyPayload* %payload, %struct.Attributes2DWords* %attrs) !continuation.maxHitAttributeBytes !13 { ret void }
; The actual size is 2 DWords smaller than the max size.
; There are 2 unused DWords in the layout.
; CHECK-LABEL: define {{.*}} @AnyHit2DWordsMax4DWords(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_4_i32s.layout_2_anyhit_out_accept.payload_attr_0_i32s
define void @AnyHit2DWordsMax4DWords(%struct.MyPayload* %payload, %struct.Attributes2DWords* %attrs) !continuation.maxHitAttributeBytes !10 { ret void }
; CHECK-LABEL: define {{.*}} @AnyHit2DWordsMax8DWords(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_8_i32s.layout_2_anyhit_out_accept.payload_attr_0_i32s
define void @AnyHit2DWordsMax8DWords(%struct.MyPayload* %payload, %struct.Attributes2DWords* %attrs) !continuation.maxHitAttributeBytes !12 { ret void }
; CHECK-LABEL: define {{.*}} @AnyHit2DWordsNoLimit(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_8_i32s.layout_2_anyhit_out_accept.payload_attr_0_i32s
define void @AnyHit2DWordsNoLimit   (%struct.MyPayload* %payload, %struct.Attributes2DWords* %attrs)                                        { ret void }

; CHECK-LABEL: define {{.*}} @AnyHit4DWordsMax4DWords(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_4_i32s.layout_2_anyhit_out_accept
define void @AnyHit4DWordsMax4DWords(%struct.MyPayload* %payload, %struct.Attributes4DWords* %attrs) !continuation.maxHitAttributeBytes !10 { ret void }
; CHECK-LABEL: define {{.*}} @AnyHit4DWordsMax8DWords(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_8_i32s.layout_2_anyhit_out_accept.payload_attr_2_i32s
define void @AnyHit4DWordsMax8DWords(%struct.MyPayload* %payload, %struct.Attributes4DWords* %attrs) !continuation.maxHitAttributeBytes !12 { ret void }
; CHECK-LABEL: define {{.*}} @AnyHit4DWordsNoLimit(
; CHECK:       {{.*}}%struct.MyPayload.attr_max_8_i32s.layout_2_anyhit_out_accept.payload_attr_2_i32s
define void @AnyHit4DWordsNoLimit   (%struct.MyPayload* %payload, %struct.Attributes4DWords* %attrs)                                        { ret void }

; The following one violates the limit and should crash:
define void @AnyHit4DWordsMax2DWords(%struct.MyPayload* %payload, %struct.Attributes4DWords* %attrs) !continuation.maxHitAttributeBytes !13 { ret void } ; INVALID

; Function Attrs: nounwind
declare void @llvm.lifetime.start(i64, i8* nocapture) #0

; Function Attrs: nounwind
declare void @llvm.lifetime.end(i64, i8* nocapture) #0

; Function Attrs: nounwind
declare void @dx.op.traceRay.struct.MyPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.MyPayload*) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.f32(i32, %dx.types.Handle, i32, i32, i32, float, float, float, float, i8) #0

; Function Attrs: nounwind readnone
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #1

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #2

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; BEGIN manually added gpurt content
%struct.HitData = type { float, i32 }
%struct.DispatchSystemData = type { i32 }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.PrimitiveSystemState = type { float, i32, i32, i32 }
%struct.AnyHitSystemData = type { %struct.SystemData, %struct.PrimitiveSystemState }
%struct.TraversalData = type { %struct.SystemData }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
declare %struct.DispatchSystemData @_cont_SetupRayGen() #4
declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #4
declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #4
declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #4
declare %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #4
declare void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #4
declare i1 @_cont_IsEndSearch(%struct.TraversalData*) #4
declare i32 @_cont_HitKind(%struct.SystemData* nocapture readnone %data, %struct.HitData*) #2
declare void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data) #1
declare void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData* %data) #1
declare i1 @_cont_ReportHit(%struct.AnyHitSystemData*  %data, float %THit, i32 %HitKind)
declare void @_cont_AcceptHit(%struct.AnyHitSystemData*%data)

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
; END manually added manually gpurt content
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }
attributes #4 = { alwaysinline }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
!dx.entryPoints = !{!19,
  !32, ; INVALID
  !33, !34, !35, !36, !37, !38, !39}

!0 = !{!"dxcoob 2019.05.00"}
!1 = !{i32 1, i32 7}
!2 = !{!"lib", i32 6, i32 7}
!3 = !{!4, !7, null, null}
!4 = !{!5}
!5 = !{i32 0, %struct.RaytracingAccelerationStructure* bitcast (%dx.types.Handle* @"\01?myAccelerationStructure@@3URaytracingAccelerationStructure@@A" to %struct.RaytracingAccelerationStructure*), !"myAccelerationStructure", i32 0, i32 3, i32 1, i32 16, i32 0, !6}
!6 = !{i32 0, i32 4}
!7 = !{!8}
!8 = !{i32 0, %"class.RWTexture2D<vector<float, 4> >"* bitcast (%dx.types.Handle* @"\01?gOutput@@3V?$RWTexture2D@V?$vector@M$03@@@@A" to %"class.RWTexture2D<vector<float, 4> >"*), !"gOutput", i32 0, i32 0, i32 1, i32 2, i1 false, i1 false, i1 false, !9}
!9 = !{i32 0, i32 9}
!10 = !{i32 16}
!12 = !{i32 32}
!13 = !{i32 8}
!19 = !{null, !"", null, !3, !20}
!20 = !{i32 0, i64 65540}
!22 = !{i32 8, i32 9, i32 5, !23}
!23 = !{i32 0}
!24 = !{!25, !25, i64 0}
!25 = !{!"float", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C/C++ TBAA"}
!28 = !{!29, !29, i64 0}
!29 = !{!"int", !26, i64 0}
!30 = !{!31, !31, i64 0}
!31 = !{!"double", !26, i64 0}
!32 = !{void (%struct.MyPayload* , %struct.Attributes4DWords*)* @AnyHit4DWordsMax2DWords, !"AnyHit4DWordsMax2DWords", null, null, !22} ; INVALID
!33 = !{void (%struct.MyPayload* , %struct.Attributes4DWords*)* @AnyHit4DWordsMax4DWords, !"AnyHit4DWordsMax4DWords", null, null, !22}
!34 = !{void (%struct.MyPayload* , %struct.Attributes4DWords*)* @AnyHit4DWordsMax8DWords, !"AnyHit4DWordsMax8DWords", null, null, !22}
!35 = !{void (%struct.MyPayload* , %struct.Attributes4DWords*)* @AnyHit4DWordsNoLimit,    !"AnyHit4DWordsNoLimit", null, null, !22}
!36 = !{void (%struct.MyPayload* , %struct.Attributes2DWords*)* @AnyHit2DWordsMax2DWords, !"AnyHit2DWordsMax2DWords", null, null, !22}
!37 = !{void (%struct.MyPayload* , %struct.Attributes2DWords*)* @AnyHit2DWordsMax4DWords, !"AnyHit2DWordsMax4DWords", null, null, !22}
!38 = !{void (%struct.MyPayload* , %struct.Attributes2DWords*)* @AnyHit2DWordsMax8DWords, !"AnyHit2DWordsMax8DWords", null, null, !22}
!39 = !{void (%struct.MyPayload* , %struct.Attributes2DWords*)* @AnyHit2DWordsNoLimit,    !"AnyHit2DWordsNoLimit", null, null, !22}
