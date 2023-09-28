; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint' -S %s 2>%t0.stderr | FileCheck -check-prefix=PAYLOADTYPE-OPAQUE %s
; RUN: count 0 < %t0.stderr
; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint' -S %s 2>%t1.stderr | FileCheck -check-prefix=PAYLOADTYPE2-OPAQUE %s
; RUN: count 0 < %t1.stderr
; RUN: opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint' -S %s 2>%t2.stderr | FileCheck -check-prefix=PAYLOADTYPE3-OPAQUE %s
; RUN: count 0 < %t2.stderr

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.MyParams = type { [48 x i32] }
%struct.TheirParams = type { [64 x i32] }
%struct.TheirParams2 = type { [65 x i32] }
%struct.RayPayload = type { [68 x i32] }
%struct.RayPayload2 = type { [64 x i32] }
%dx.types.Handle = type { i8* }
%dx.types.ResourceProperties = type { i32, i32 }
%"class.RWTexture2D<vector<float, 4> >" = type { <4 x float> }

@"\01?Scene@@3URaytracingAccelerationStructure@@A" = external constant %dx.types.Handle, align 4
@"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A" = external constant %dx.types.Handle, align 4

%struct.DispatchSystemData = type { i32 }
%struct.SystemData = type { %struct.DispatchSystemData }
%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }

; Function Attrs: nounwind
declare !types !39 void @dx.op.traceRay.struct.RayPayload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload*) #0
declare !types !49 void @dx.op.traceRay.struct.RayPayload2(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.RayPayload2*) #0

; Function Attrs: nounwind readnone
declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #1

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32, %dx.types.Handle) #2

; Function Attrs: nounwind
declare !types !40 void @dx.op.callShader.struct.TheirParams(i32, i32, %struct.TheirParams*) #0
declare !types !41 void @dx.op.callShader.struct.TheirParams2(i32, i32, %struct.TheirParams2*) #0

declare float @dx.op.rayTCurrent.f32(i32) #1
declare float @dx.op.rayTMin.f32(i32) #2
declare i32 @dx.op.hitKind.i32(i32) #2
declare i32 @dx.op.instanceID.i32(i32) #2
declare !types !42 i1 @dx.op.reportHit.struct.BuiltInTriangleIntersectionAttributes(i32, float, i32, %struct.BuiltInTriangleIntersectionAttributes*) #5

define void @main() {
; PAYLOADTYPE-LABEL: define void @main
; PAYLOADTYPE:    call void (...) @lgc.rt.call.callable.shader(i32 1, %struct.TheirParams* %{{.*}}, i32 256), !dxil.payload.type ![[call_callable_shader_payload_type:[0-9]+]]
; PAYLOADTYPE-NEXT:    call void (...) @lgc.rt.call.callable.shader(i32 1, %struct.TheirParams* %{{.*}}, i32 256), !dxil.payload.type ![[call_callable_shader_payload_type]]
; PAYLOADTYPE-NEXT:    call void (...) @lgc.rt.call.callable.shader(i32 2, %struct.TheirParams2* %{{.*}}, i32 260), !dxil.payload.type ![[call_callable_shader_payload_type2:[0-9]+]]
; PAYLOADTYPE: ![[call_callable_shader_payload_type]] = !{%struct.TheirParams poison}
; PAYLOADTYPE: ![[call_callable_shader_payload_type2]] = !{%struct.TheirParams2 poison}

; PAYLOADTYPE-OPAQUE-LABEL: define void @main
; PAYLOADTYPE-OPAQUE:    call void (...) @lgc.rt.call.callable.shader(i32 1, ptr %{{.*}}, i32 256), !dxil.payload.type ![[call_callable_shader_payload_type:[0-9]+]]
; PAYLOADTYPE-OPAQUE-NEXT:    call void (...) @lgc.rt.call.callable.shader(i32 1, ptr %{{.*}}, i32 256), !dxil.payload.type ![[call_callable_shader_payload_type]]
; PAYLOADTYPE-OPAQUE-NEXT:    call void (...) @lgc.rt.call.callable.shader(i32 2, ptr %{{.*}}, i32 260), !dxil.payload.type ![[call_callable_shader_payload_type2:[0-9]+]]
; PAYLOADTYPE-OPAQUE: ![[call_callable_shader_payload_type]] = !{%struct.TheirParams poison}
; PAYLOADTYPE-OPAQUE: ![[call_callable_shader_payload_type2]] = !{%struct.TheirParams2 poison}
;
  %params = alloca %struct.TheirParams, align 4
  %params2 = alloca %struct.TheirParams2, align 4
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
  call void @dx.op.callShader.struct.TheirParams(i32 159, i32 1, %struct.TheirParams* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
  call void @dx.op.callShader.struct.TheirParams2(i32 159, i32 2, %struct.TheirParams2* nonnull %params2)  ; CallShader(ShaderIndex,Parameter)
  ret void
}

define void @mainTrace() {
; PAYLOADTYPE2-LABEL: define void @mainTrace
; PAYLOADTYPE2:    call void (...) @lgc.rt.trace.ray(i64 %{{.*}}, i32 16, i32 -1, i32 0, i32 1, i32 0, <3 x float> zeroinitializer, float 0x3F50624DE0000000, <3 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00>, float 1.000000e+04, %struct.RayPayload* %{{.*}}, [1 x i32] [i32 272]), !dxil.payload.type ![[traceray_payload_type:[0-9]+]]
; PAYLOADTYPE2:    call void (...) @lgc.rt.trace.ray(i64 %{{.*}}, i32 16, i32 -1, i32 0, i32 1, i32 0, <3 x float> zeroinitializer, float 0x3F50624DE0000000, <3 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00>, float 1.000000e+04, %struct.RayPayload2* %{{.*}}, [1 x i32] [i32 256]), !dxil.payload.type ![[traceray_payload_type2:[0-9]+]]
; PAYLOADTYPE2: ![[traceray_payload_type]] = !{%struct.RayPayload poison}
; PAYLOADTYPE2: ![[traceray_payload_type2]] = !{%struct.RayPayload2 poison}

; PAYLOADTYPE2-OPAQUE-LABEL: define void @mainTrace
; PAYLOADTYPE2-OPAQUE:    call void (...) @lgc.rt.trace.ray(i64 %{{.*}}, i32 16, i32 -1, i32 0, i32 1, i32 0, <3 x float> zeroinitializer, float 0x3F50624DE0000000, <3 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00>, float 1.000000e+04, ptr %{{.*}}, [1 x i32] [i32 272]), !dxil.payload.type ![[traceray_payload_type:[0-9]+]]
; PAYLOADTYPE2-OPAQUE:    call void (...) @lgc.rt.trace.ray(i64 %{{.*}}, i32 16, i32 -1, i32 0, i32 1, i32 0, <3 x float> zeroinitializer, float 0x3F50624DE0000000, <3 x float> <float 1.000000e+00, float 0.000000e+00, float 0.000000e+00>, float 1.000000e+04, ptr %{{.*}}, [1 x i32] [i32 256]), !dxil.payload.type ![[traceray_payload_type2:[0-9]+]]
; PAYLOADTYPE2-OPAQUE: ![[traceray_payload_type]] = !{%struct.RayPayload poison}
; PAYLOADTYPE2-OPAQUE: ![[traceray_payload_type2]] = !{%struct.RayPayload2 poison}
;
  %1 = load %dx.types.Handle, %dx.types.Handle* @"\01?Scene@@3URaytracingAccelerationStructure@@A", align 4
  %2 = load %dx.types.Handle, %dx.types.Handle* @"\01?RenderTarget@@3V?$RWTexture2D@V?$vector@M$03@@@@A", align 4
  %3 = alloca %struct.RayPayload, align 4
  %4 = alloca %struct.RayPayload2, align 4
  %5 = getelementptr inbounds %struct.RayPayload, %struct.RayPayload* %3, i32 0, i32 0
  %6 = getelementptr inbounds %struct.RayPayload2, %struct.RayPayload2* %4, i32 0, i32 0
  %7 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)  ; CreateHandleForLib(Resource)
  %8 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %7, %dx.types.ResourceProperties { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure
  call void @dx.op.traceRay.struct.RayPayload(i32 157, %dx.types.Handle %8, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload* nonnull %3)  ; TraceRay(AccelerationStructure,RayFlags,InstanceInclusionMask,RayContributionToHitGroupIndex,MultiplierForGeometryContributionToShaderIndex,MissShaderIndex,Origin_X,Origin_Y,Origin_Z,TMin,Direction_X,Direction_Y,Direction_Z,TMax,payload)
  call void @dx.op.traceRay.struct.RayPayload2(i32 157, %dx.types.Handle %8, i32 16, i32 -1, i32 0, i32 1, i32 0, float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 0x3F50624DE0000000, float 1.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+04, %struct.RayPayload2* nonnull %4)  ; TraceRay(AccelerationStructure,RayFlags,InstanceInclusionMask,RayContributionToHitGroupIndex,MultiplierForGeometryContributionToShaderIndex,MissShaderIndex,Origin_X,Origin_Y,Origin_Z,TMin,Direction_X,Direction_Y,Direction_Z,TMax,payload)
  ret void
}

define void @called(%struct.MyParams* %arg) !types !38 {
; PAYLOADTYPE3-LABEL: define void @called
; PAYLOADTYPE3:    call void (...) @lgc.rt.call.callable.shader(i32 2, %struct.TheirParams2* %{{.*}}, i32 260), !dxil.payload.type ![[call_callable_shader_payload_type:[0-9]+]]
; PAYLOADTYPE3: ![[call_callable_shader_payload_type]] = !{%struct.TheirParams2 poison}
; PAYLOADTYPE3-OPAQUE-LABEL: define void @called
; PAYLOADTYPE3-OPAQUE:    call void (...) @lgc.rt.call.callable.shader(i32 2, ptr %{{.*}}, i32 260), !dxil.payload.type ![[call_callable_shader_payload_type:[0-9]+]]
; PAYLOADTYPE3-OPAQUE: ![[call_callable_shader_payload_type]] = !{%struct.TheirParams2 poison}
;
  %params = alloca %struct.TheirParams2, align 4
  call void @dx.op.callShader.struct.TheirParams2(i32 159, i32 2, %struct.TheirParams2* nonnull %params)  ; CallShader(ShaderIndex,Parameter)
  ret void
}

attributes #0 = { nounwind }
attributes #1 = { nounwind readnone }
attributes #2 = { nounwind readonly }

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
!5 = !{void ()* @"main", !"main", null, null, !21}
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
!38 = !{!"function", !"void", !43}
!39 = !{!"function", !"void", i32 poison, %dx.types.Handle poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, !44}
!40 = !{!"function", !"void", i32 poison, i32 poison, !45}
!41 = !{!"function", !"void", i32 poison, i32 poison, !46}
!42 = !{!"function", !"void", i32 poison, float poison, i32 poison, !47}
!43 = !{i32 0, %struct.MyParams poison}
!44 = !{i32 0, %struct.RayPayload poison}
!45 = !{i32 0, %struct.TheirParams poison}
!46 = !{i32 0, %struct.TheirParams2 poison}
!47 = !{i32 0, %struct.BuiltInTriangleIntersectionAttributes poison}
!48 = !{i32 0, %struct.RayPayload2 poison}
!49 = !{!"function", !"void", i32 poison, %dx.types.Handle poison, i32 poison, i32 poison, i32 poison, i32 poison, i32 poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, float poison, !48}
