; RUN: not opt --verify-each -passes='continuations-lint,continuations-lint,remove-types-metadata' -S %s --lint-abort-on-error 2>&1 | FileCheck %s

; CHECK: Jump has undefined jump target

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

%struct.DispatchSystemData = type { i32 }

declare void @lgc.ilcps.continue(...)

define void @RayGen(i64 %dummyRetAddr, %struct.DispatchSystemData %0) !lgc.rt.shaderstage !0 !continuation.entry !1 !continuation !2 {
  call void (...) @lgc.ilcps.continue(i64 undef, i32 undef, i64 undef), !continuation.registercount !0
  unreachable
}

!continuation.stackAddrspace = !{!3}

!0 = !{i32 0}
!1 = !{}
!2 = !{void ()* @RayGen}
!3 = !{i32 21}
