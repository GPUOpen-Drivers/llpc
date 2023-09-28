; RUN: opt --report-cont-state-sizes       --verify-each -passes='dxil-cont-post-process,lint,remove-types-metadata' -S %s 2>&1 | FileCheck %s --check-prefix=REPORT-CONT-SIZES
; RUN: opt --report-payload-register-sizes --verify-each -passes='dxil-cont-post-process,lint,remove-types-metadata' -S %s 2>&1 | FileCheck %s --check-prefix=REPORT-PAYLOAD-SIZES
; RUN: opt --report-system-data-sizes      --verify-each -passes='dxil-cont-post-process,lint,remove-types-metadata' -S %s 2>&1 | FileCheck %s --check-prefix=REPORT-SYSTEM-DATA-SIZES

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-i1:32-i8:8-i16:32-i32:32-i64:32-f16:32-f32:32-f64:32-v16:32-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.DispatchSystemData = type { i32 }
%struct.CHSSystemData = type { [100 x i32] }

declare i32 @continuation.initialContinuationStackPtr()
declare i32 @_cont_GetContinuationStackAddr()
declare i32 @_cont_GetLocalRootIndex(%struct.DispatchSystemData*)
declare %struct.DispatchSystemData @_cont_SetupRayGen()
declare void @continuation.continue()

; REPORT-CONT-SIZES: Continuation state size of "RayGen" (raygeneration): 108 bytes
; REPORT-PAYLOAD-SIZES: Incoming and max outgoing payload VGPR size of "RayGen" (raygeneration): 28 and 24 bytes
define void @RayGen() !continuation.entry !0 !continuation !3 !continuation.state !5 !continuation.registercount !7 {
  %csp = alloca i32, align 4
  %cspInit = call i32 @continuation.initialContinuationStackPtr()
  store i32 %cspInit, i32* %csp
  call void @continuation.continue(), !continuation.registercount !6
  ret void
}

; This is needed as fake continuation of RayGen, because we only report continuation state sizes
; if we find a continuation function using !continuation metadata.
; REPORT-SYSTEM-DATA-SIZES-DAG: Incoming system data of "RayGen.resume.0" (raygeneration) is "struct.DispatchSystemData", size: 4 bytes
define void @RayGen.resume.0(i32 %0, %struct.DispatchSystemData %1) !continuation !3 {
  ret void
}

; REPORT-PAYLOAD-SIZES: Incoming and max outgoing payload VGPR size of "CHS" (closesthit): 32 and 36 bytes
; REPORT-SYSTEM-DATA-SIZES-DAG: Incoming system data of "CHS" (closesthit) is "struct.CHSSystemData", size: 400 bytes
define void @CHS(i32 %cspInit, i64 %returnAddr, %struct.CHSSystemData %0) !continuation.registercount !8 {
  call void @continuation.continue(), !continuation.registercount !9
  ret void
}

!dx.entryPoints = !{!1, !10}
!continuation.stackAddrspace = !{!4}

!0 = !{}
!1 = !{void ()* @RayGen, !"RayGen", null, null, !2}
!2 = !{i32 8, i32 7}
!3 = !{void ()* @RayGen}
!4 = !{i32 21}
!5 = !{i32 108}
!6 = !{i32 6}
!7 = !{i32 7}
!8 = !{i32 8}
!9 = !{i32 9}
!10 = !{void ()* @CHS, !"CHS", null, null, !11}
!11 = !{i32 8, i32 10}
