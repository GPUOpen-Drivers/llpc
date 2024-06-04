; Test payload serialization layouts in presence of different max hit attribute
; size metadata.
;
; Default run checking serialization layouts and their usage:
; RUN: grep -v 'NOT-1' %s | opt -debug-only=lower-raytracing-pipeline --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S --lint-abort-on-error 2>&1 | FileCheck %s --check-prefixes=CHECK,CHECK-MAX-1
; RUN: grep -v 'NOT-2' %s | opt -debug-only=lower-raytracing-pipeline --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S --lint-abort-on-error 2>&1 | FileCheck %s --check-prefixes=CHECK,CHECK-MAX-2
; RUN: grep -v 'NOT-4' %s | opt -debug-only=lower-raytracing-pipeline --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S --lint-abort-on-error 2>&1 | FileCheck %s --check-prefixes=CHECK,CHECK-MAX-4
; RUN: grep -v 'NOT-8' %s | opt -debug-only=lower-raytracing-pipeline --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S --lint-abort-on-error 2>&1 | FileCheck %s --check-prefixes=CHECK,CHECK-MAX-8

; Check that hit attributes violating the max size (here: 2 Dwords, set by removing lines containing NOT-2) are detected and crash:
; RUN: grep -v 'NOT-INVALID' %s | not --crash opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint,lower-raytracing-pipeline,lint,remove-types-metadata' -S --lint-abort-on-error 2>&1 | FileCheck %s --check-prefix INVALID
; REQUIRES: assertions

; INVALID: Hit attributes are too large!

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%dx.types.Handle = type { i8* }
%struct.MyPayload = type { float, i32, double }
%struct.Attributes1DWords = type { [1 x i32] }
%struct.Attributes2DWords = type { [2 x i32] }
%struct.Attributes4DWords = type { [4 x i32] }
%struct.Attributes8DWords = type { [8 x i32] }
%dx.types.ResourceProperties = type { i32, i32 }
%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { %struct.SystemData }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.AnyHitTraversalData = type { %struct.TraversalData, %struct.HitData }
%struct.HitData = type { float, i32 }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }
%struct.AnyHitSystemData = type { %struct.SystemData, %struct.PrimitiveSystemState }
%struct.PrimitiveSystemState = type { float, i32, i32, i32 }
%struct.RaytracingAccelerationStructure = type { i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?myAccelerationStructure@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?gOutput@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

; If the app uses only 1 DWord for hit attributes, then the layout does not get smaller.
; Instead, one 1 DWord in system data is unused.
; CHECK-MAX-1-DAG: %struct.MyPayload.attr_max_1_i32s.layout_0_caller_out = type { [4 x i32] }
; CHECK-MAX-2-DAG: %struct.MyPayload.attr_max_2_i32s.layout_0_caller_out = type { [4 x i32] }
; CHECK-MAX-4-DAG: %struct.MyPayload.attr_max_4_i32s.layout_0_caller_out = type { [6 x i32] }
; CHECK-MAX-8-DAG: %struct.MyPayload.attr_max_8_i32s.layout_0_caller_out = type { [10 x i32] }

; CHECK-LABEL: define {{.*}} @AnyHit1DWords(
define void @AnyHit1DWords(%struct.MyPayload* %payload, %struct.Attributes1DWords* %attrs) !types !60 {
  ret void
}

; CHECK-LABEL: define {{.*}} @AnyHit2DWords(
define void @AnyHit2DWords(%struct.MyPayload* %payload, %struct.Attributes2DWords* %attrs) !types !23 {
  ret void
}

; CHECK-LABEL: define {{.*}} @AnyHit4DWords(
define void @AnyHit4DWords(%struct.MyPayload* %payload, %struct.Attributes4DWords* %attrs) !types !28 {
  ret void
}

; CHECK-LABEL: define {{.*}} @AnyHit8DWords(
define void @AnyHit8DWords(%struct.MyPayload* %payload, %struct.Attributes8DWords* %attrs) !types !63 {
  ret void
}

; Function Attrs: nounwind
declare !types !30 void @dx.op.traceRay.struct.MyPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.MyPayload*) #0

; Function Attrs: nounwind
declare void @dx.op.textureStore.f32(i32, %dx.types.Handle, i32, i32, i32, float, float, float, float, i8) #0

; Function Attrs: nounwind memory(none)
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #1

; Function Attrs: nounwind memory(none)
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

; Function Attrs: nounwind memory(read)
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #2

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_cont_SetupRayGen() #3

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_AmdAwaitTraversal(i64, %struct.TraversalData) #3

; Function Attrs: alwaysinline
declare %struct.DispatchSystemData @_AmdAwaitShader(i64, %struct.DispatchSystemData) #3

; Function Attrs: alwaysinline
declare %struct.AnyHitTraversalData @_AmdAwaitAnyHit(i64, %struct.AnyHitTraversalData, float, i32) #3

; Function Attrs: alwaysinline
declare !types !31 %struct.BuiltInTriangleIntersectionAttributes @_cont_GetTriangleHitAttributes(%struct.SystemData*) #3

; Function Attrs: alwaysinline
declare !types !33 void @_cont_SetTriangleHitAttributes(%struct.SystemData*, %struct.BuiltInTriangleIntersectionAttributes) #3

; Function Attrs: alwaysinline
declare !types !34 i1 @_cont_IsEndSearch(%struct.TraversalData*) #3

; Function Attrs: nounwind memory(read)
declare !types !36 i32 @_cont_HitKind(%struct.SystemData* nocapture readnone, %struct.HitData*) #2

; Function Attrs: nounwind memory(none)
declare !types !38 void @_AmdRestoreSystemData(%struct.DispatchSystemData*) #1

; Function Attrs: nounwind memory(none)
declare !types !40 void @_AmdRestoreSystemDataAnyHit(%struct.AnyHitTraversalData*) #1

declare !types !42 i1 @_cont_ReportHit(%struct.AnyHitSystemData*, float, i32)

declare !types !44 void @_cont_AcceptHit(%struct.AnyHitSystemData*)

; Function Attrs: alwaysinline
define i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData* %data) #3 !types !45 {
  ret i32 5
}

; Function Attrs: alwaysinline
define void @_cont_TraceRay(%struct.DispatchSystemData* %data, i64 %0, i32 %1, i32 %2, i32 %3, i32 %4, i32 %5, float %6, float %7, float %8, float %9, float %10, float %11, float %12, float %13) #3 !types !46 {
  %dis_data = load %struct.DispatchSystemData, %struct.DispatchSystemData* %data, align 4
  %sys_data = insertvalue %struct.SystemData undef, %struct.DispatchSystemData %dis_data, 0
  %trav_data = insertvalue %struct.TraversalData undef, %struct.SystemData %sys_data, 0
  %newdata = call %struct.DispatchSystemData @_AmdAwaitTraversal(i64 4, %struct.TraversalData %trav_data)
  store %struct.DispatchSystemData %newdata, %struct.DispatchSystemData* %data, align 4
  call void @_AmdRestoreSystemData(%struct.DispatchSystemData* %data)
  ret void
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !types !47 void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #4

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare !types !47 void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #4

attributes #0 = { nounwind }
attributes #1 = { nounwind memory(none) }
attributes #2 = { nounwind memory(read) }
attributes #3 = { alwaysinline }
attributes #4 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!1}
!dx.shaderModel = !{!2}
!dx.resources = !{!3}
; DX entry points. We use grep filters on NOT-{maxSize} to only enable compatible shaders.
!dx.entryPoints = !{
  !10
  , !14 ; AHS using 1 Dword attributes.
  , !15 ; AHS using 2 Dword attributes.  NOT-1
  , !16 ; AHS using 4 Dword attributes.  NOT-1 NOT-2
  , !17 ; AHS using 8 Dword attributes.  NOT-1 NOT-2 NOT-4
}

; We filter out one of the following lines using a grep in the RUN line.
; The NOT-{maxSize} patterns are used to run a test with the max hit attribute size to
; maxSize, and only enabling compatible shaders.
; The NOT-INVALID pattern is used to run all shaders with a max attribute size of 2 dwords,
; which is expected to fail.
!lgc.rt.max.attribute.size = !{!49} ; 1 DWord(s).       NOT-2 NOT-4 NOT-8 NOT-INVALID
!lgc.rt.max.attribute.size = !{!22} ; 2 DWord(s). NOT-1       NOT-4 NOT-8
!lgc.rt.max.attribute.size = !{!26} ; 4 DWord(s). NOT-1 NOT-2       NOT-8 NOT-INVALID
!lgc.rt.max.attribute.size = !{!27} ; 8 DWord(s). NOT-1 NOT-2 NOT-4       NOT-INVALID

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
!10 = !{null, !"", null, !3, !11}
!11 = !{i32 0, i64 65540}
!12 = !{i32 8, i32 9, i32 5, !13}
!13 = !{i32 0}
!14 = !{void (%struct.MyPayload*, %struct.Attributes1DWords*)* @AnyHit1DWords, !"AnyHit1DWords", null, null, !12}
!15 = !{void (%struct.MyPayload*, %struct.Attributes4DWords*)* @AnyHit2DWords, !"AnyHit2DWords", null, null, !12}
!16 = !{void (%struct.MyPayload*, %struct.Attributes4DWords*)* @AnyHit4DWords, !"AnyHit4DWords", null, null, !12}
!17 = !{void (%struct.MyPayload*, %struct.Attributes4DWords*)* @AnyHit8DWords, !"AnyHit8DWords", null, null, !12}
!22 = !{i32 8}
!23 = !{!"function", !"void", !24, !25}
!24 = !{i32 0, %struct.MyPayload poison}
!25 = !{i32 0, %struct.Attributes2DWords poison}
!26 = !{i32 16}
!27 = !{i32 32}
!28 = !{!"function", !"void", !24, !29}
!29 = !{i32 0, %struct.Attributes4DWords poison}
!30 = !{!"function", !"void", i32 poison, %dx.types.Handle poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, !24}
!31 = !{!"function", %struct.BuiltInTriangleIntersectionAttributes poison, !32}
!32 = !{i32 0, %struct.SystemData poison}
!33 = !{!"function", !"void", !32, %struct.BuiltInTriangleIntersectionAttributes poison}
!34 = !{!"function", i1 poison, !35}
!35 = !{i32 0, %struct.TraversalData poison}
!36 = !{!"function", i32 poison, !32, !37}
!37 = !{i32 0, %struct.HitData poison}
!38 = !{!"function", !"void", !39}
!39 = !{i32 0, %struct.DispatchSystemData poison}
!40 = !{!"function", !"void", !41}
!41 = !{i32 0, %struct.AnyHitTraversalData poison}
!42 = !{!"function", i1 poison, !43, float poison, i32 poison}
!43 = !{i32 0, %struct.AnyHitSystemData poison}
!44 = !{!"function", !"void", !43}
!45 = !{!"function", i32 poison, !39}
!46 = !{!"function", !"void", !39, i64 poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison}
!47 = !{!"function", !"void", i64 poison, !48}
!48 = !{i32 0, i8 poison}
!49 = !{i32 4}
!60 = !{!"function", !"void", !61, !62}
!61 = !{i32 0, %struct.MyPayload poison}
!62 = !{i32 0, %struct.Attributes1DWords poison}
!63 = !{!"function", !"void", !24, !64}
!64 = !{i32 0, %struct.Attributes8DWords poison}
