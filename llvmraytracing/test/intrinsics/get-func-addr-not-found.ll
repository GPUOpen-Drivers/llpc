; NOTE: Do not autogenerate
;;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
 ;
 ;  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 ;
 ;  Permission is hereby granted, free of charge, to any person obtaining a copy
 ;  of this software and associated documentation files (the "Software"), to
 ;  deal in the Software without restriction, including without limitation the
 ;  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 ;  sell copies of the Software, and to permit persons to whom the Software is
 ;  furnished to do so, subject to the following conditions:
 ;
 ;  The above copyright notice and this permission notice shall be included in all
 ;  copies or substantial portions of the Software.
 ;
 ;  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ;  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ;  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 ;  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 ;  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 ;  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 ;  IN THE SOFTWARE.
 ;
 ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; RUN: not --crash opt --verify-each -passes='dxil-cont-lgc-rt-op-converter,lint<abort-on-error>,lower-raytracing-pipeline,lint<abort-on-error>' -S %s 2>&1 | FileCheck %s

; CHECK: ERROR: Did not find function '' requested by _AmdGetFuncAddr

%struct.DispatchSystemData = type { i32 }
%struct.TraversalData = type { }

declare i32 @_AmdGetFuncAddr()

declare !pointeetys !11 i1 @_cont_ReportHit(%struct.TraversalData* %data, float %t, i32 %hitKind)

declare !pointeetys !{%struct.DispatchSystemData poison} void @_cont_DispatchRaysIndex3(%struct.DispatchSystemData*)

define void @_cont_ExitRayGen(ptr nocapture readonly %data) alwaysinline nounwind !pointeetys !8 {
  ret void
}

define i32 @main() {
entry:
  %val = call i32 @_AmdGetFuncAddr()
  ret i32 %val
}

!dx.entryPoints = !{!0, !3}
!continuation.stackAddrspace = !{!7}

!0 = !{null, !"", null, !1, !6}
!1 = !{!2, null, null, null}
!2 = !{!3}
!3 = !{i1 ()* @main, !"main", null, null, !4}
!4 = !{i32 8, i32 7, i32 6, i32 16, i32 7, i32 8, i32 5, !5}
!5 = !{i32 0}
!6 = !{i32 0, i64 65536}
!7 = !{i32 21}
!8 = !{%struct.DispatchSystemData poison}
!9 = !{i32 0, %struct.DispatchSystemData poison}
!10 = !{i32 0, %struct.TraversalData poison}
!11 = !{%struct.TraversalData poison}
