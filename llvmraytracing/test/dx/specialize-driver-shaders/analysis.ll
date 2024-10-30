; RUN: opt --verify-each -passes='specialize-driver-shaders' -S %s -debug-only='specialize-driver-shaders' 2>&1 | FileCheck %s
;
; REQUIRES: assertions

; Intentionally align i64 to 64 bits so we can test analysis of args that contain padding in memory,
; where the in-register layout in the calling convention does not match the memory layout.
target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:64-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"

; This tests focuses on the preserved-argument analysis for different shader kinds, including await handling.
; For that, we don't care about specific argument details, and thus use an i32 array most of the time.
%args.type = type { [31 x i32] }
; Awaits wrap args into a struct, even if it is just a single one
%awaited.args.type = type { %args.type }
%args.with.padding = type { i32, i64, { i32, i64 } }

; Ignored prefix args: shaderAddr, levels, state, returnAddr, shaderRecIdx
declare void @lgc.cps.jump(...)
; Ignored prefix args: shaderAddr, levels, shaderRecIdx
; The __ suffix is required to let the dialect visitor detect this as an overload of the await op.
declare %awaited.args.type @lgc.cps.await__(...)
declare %args.with.padding @lgc.cps.await__p(...)
declare { <2 x i16> } @lgc.cps.await__2xi16(...)
declare { i16, i16 } @lgc.cps.await__i16i16(...)
declare { i32 } @lgc.cps.await__i32(...)

; Legacy await:
declare %awaited.args.type @await(...)
declare %args.type @opaque(...)

; Simple AHS that just forwards args
; CHECK-LABEL: [SDS] Analyzing function AnyHit1
define void @AnyHit1({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !2 {
; CHECK-NEXT: [SDS] Analyzed outgoing call {{.*}} @lgc.cps.jump({{.*}} %args)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %args)
  unreachable
; CHECK-NEXT: [SDS] Finished analysis of function AnyHit1
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
}

; Single-jump AHS that:
;  * swaps the first two dwords (dynamic)
;  * writes constant to dword 10 (constant)
;  * writes poison to dword 11 (undef)
;  * condiditionally writes constant to dword 20 (constant)
;  * condiditionally writes undef to dword 21 (undef)
;  * condiditionally writes undef or constant to dword 22 (constant)
;  * condiditionally writes constant or dynamic to dword 23 (dynamic)
;  * writes same constants to dword 25 (constant)
;  * writes different constants to dword 26 (dynamic)
; CHECK-LABEL: [SDS] Analyzing function AnyHit2
define void @AnyHit2({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !2 {
entry:
  %dw0 = extractvalue %args.type %args, 0, 0
  %dw1 = extractvalue %args.type %args, 0, 1
  %tmp0 = insertvalue %args.type %args, i32 %dw1, 0, 0
  %tmp1 = insertvalue %args.type %tmp0, i32 %dw0, 0, 1
  %tmp2 = insertvalue %args.type %tmp1, i32 -1, 0, 10
  %tmp3 = insertvalue %args.type %tmp2, i32 poison, 0, 11
  %tmp4 = insertvalue %args.type %tmp3, i32 undef, 0, 22
  %dynamic = load i32, ptr null
  %tmp5 = insertvalue %args.type %tmp4, i32 %dynamic, 0, 23
  %tmp6 = insertvalue %args.type %tmp5, i32 -1, 0, 25
  %tmp7 = insertvalue %args.type %tmp6, i32 0, 0, 26
  %cond = trunc i32 %dw0 to i1
  br i1 %cond, label %conditional, label %exit
conditional:
  %tmp8 = insertvalue %args.type %tmp7, i32 0, 0, 20
  %tmp9 = insertvalue %args.type %tmp8, i32 undef, 0, 21
  %tmp10 = insertvalue %args.type %tmp9, i32 -1, 0, 22
  %tmp11 = insertvalue %args.type %tmp10, i32 -1, 0, 23
  %tmp12 = insertvalue %args.type %tmp11, i32 -1, 0, 25
  %tmp13 = insertvalue %args.type %tmp12, i32 -1, 0, 26
  br label %exit
exit:
  %args.final = phi %args.type [ %tmp13, %conditional ], [ %tmp7, %entry ]
; CHECK-NEXT: [SDS] Analyzed outgoing call {{.*}} @lgc.cps.jump({{.*}} %args.final)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] DDPPPPPPPPCUPPPPPPPPCUCDPCDPPPP{{$}}
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %args.final)
  unreachable
}

; Two-jump AHS that does different things on the two jumps, testing merging of states
; across jumps works correctly:
;  * write constant to dword 0 only on Jump0
;  * write constant to dword 1 only on Jump1
;  * write matching constants to dword 2
; CHECK-LABEL: [SDS] Analyzing function AnyHit3
;  * write non-matching constants to dword 3
define void @AnyHit3({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !2 {
entry:
  %dw0 = extractvalue %args.type %args, 0, 0
  %cond = trunc i32 %dw0 to i1
  br i1 %cond, label %exit0, label %exit1
exit0:
  %tmp0 = insertvalue %args.type %args, i32 -1, 0, 0
  %tmp1 = insertvalue %args.type %tmp0, i32 -1, 0, 2
  %tmp2 = insertvalue %args.type %tmp1, i32 -1, 0, 3
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %tmp2)
  unreachable
exit1:
  %tmp3 = insertvalue %args.type %args, i32 -1, 0, 1
  %tmp4 = insertvalue %args.type %tmp3, i32 -1, 0, 2
  %tmp5 = insertvalue %args.type %tmp4, i32 -2, 0, 3
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %tmp5)
  unreachable
; CHECK:      [SDS] Finished analysis of function AnyHit3
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] CCCDPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
}

; Intersection with an await call simulating a ReportHit call.
; Check that values passed to await are checked and accounted for in the preserved state,
; and that using values returned from await counts as preserved.
; Also check that using original argument values in awaits after awaits still count as preserved.
; Note: This is only possible because we run before coro passes, after coro passes such values
; would be loaded from continuation state and their origin unknown.
; This uses lgc.cps.await.
; CHECK-LABEL: [SDS] Analyzing function Intersection1
define void @Intersection1({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !1 {
entry:
  %dw0 = extractvalue %args.type %args, 0, 0
  %cond = trunc i32 %dw0 to i1
  br i1 %cond, label %conditional, label %exit
conditional:
; Pass through args, trivially all-preserve
; CHECK-NEXT: [SDS] Analyzed outgoing call   %awaited.0.struct {{.*}}lgc.cps.await{{.*}} %args)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  %awaited.0.struct = call %awaited.args.type (...) @lgc.cps.await__(i32 poison, i32 poison, i32 poison, %args.type %args)
  %awaited.0 = extractvalue %awaited.args.type %awaited.0.struct, 0
; Pass awaited results. Should still be all-preserve. This tests awaited results are correctly handled.
; CHECK-NEXT: [SDS] Analyzed outgoing call   %awaited.1.struct {{.*}}lgc.cps.await{{.*}} %awaited.0)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  %awaited.1.struct = call %awaited.args.type (...) @lgc.cps.await__(i32 poison, i32 poison, i32 poison, %args.type %awaited.0)
  %awaited.1 = extractvalue %awaited.args.type %awaited.1.struct, 0
  %awaited.merged = insertvalue %args.type %awaited.1, i32 %dw0, 0, 0
; Reuse incoming dword 0. Should still be preserved.
; CHECK-NEXT: [SDS] Analyzed outgoing call   %awaited.2.struct {{.*}}lgc.cps.await{{.*}} %awaited.merged)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  %awaited.2.struct = call %awaited.args.type (...) @lgc.cps.await__(i32 poison, i32 poison, i32 poison, %args.type %awaited.merged)
  %awaited.2 = extractvalue %awaited.args.type %awaited.2.struct, 0
  br label %exit
exit:
  %args.final = phi %args.type [ %awaited.2, %conditional ], [ %args, %entry ]
; CHECK-NEXT: [SDS] Analyzed outgoing call {{.*}} @lgc.cps.jump({{.*}} %args.final)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %args.final)
  unreachable
}

; Basic test that legacy await is also handled.
; Note: This test is a bit odd, because this is an lgc.cps module, and we only expect legacy awaits in non-lgc.cps modules.
; Thus, we use the lgc.cps mode version of lgc.cps.jump including a to-be-ignored shader record index.
; CHECK-LABEL: [SDS] Analyzing function Intersection2
define void @Intersection2({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !1 {
  %handle = call ptr inttoptr (i32 poison to ptr)(%args.type %args)
  %awaited = call %args.type @await(ptr %handle)
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %awaited)
  ret void
; CHECK:      [SDS] Finished analysis of function Intersection2
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
}

; Check that other function calls to non-await functions are not accidentally considered as preserved.
; CHECK-LABEL: [SDS] Analyzing function Intersection3
define void @Intersection3({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !1 {
  %not.awaited = call %args.type @opaque(%args.type %args)
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %not.awaited)
  ret void
; CHECK:      [SDS] Finished analysis of function Intersection3
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD{{$}}
}

; Check that with awaits and phi nodes, we apply the value origin analysis to incoming values of phi nodes,
; and not directly compare against incoming args and await results.
; Check both the dynamic and constant case: Change dw0 dynamically, and dw1 to a constant.
; Then conditionally await, and at the end jump using either the modified %args value or the await result.
; The jump argument will be a phi result, and the incoming value
; needs to go through value origin tracking to determine it's in fact
; the incoming function argument, except for the modified dword.
; We use two conditional awaits so also in the constant case (dw1), there are multiple
; dynamic values coming into the phi node. With just a single one, value origin tracking
; can see through the phi node and our phi node handling is not triggered.
; CHECK-LABEL: [SDS] Analyzing function Intersection4
define void @Intersection4({}, i32, i32, %args.type %args) !lgc.rt.shaderstage !1 {
entry:
  %dw1 = extractvalue %args.type %args, 0, 1
  %args.modified.0 = insertvalue %args.type %args, i32 %dw1, 0, 0
  %args.modified = insertvalue %args.type %args.modified.0, i32 0, 0, 1
  ;%args.modified = insertvalue %args.type %args, i32 1337, 0, 0
  %cond = trunc i32 %dw1 to i1
  switch i32 %dw1, label %exit [
    i32 0, label %conditional.0
    i32 1, label %conditional.1
  ]
conditional.0:
; CHECK-NEXT: [SDS] Analyzed outgoing call   %awaited.0.struct {{.*}}lgc.cps.await{{.*}} %args.modified)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] DCPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  %awaited.0.struct = call %awaited.args.type (...) @lgc.cps.await__(i32 poison, i32 poison, i32 poison, %args.type %args.modified)
  %awaited.0 = extractvalue %awaited.args.type %awaited.0.struct, 0
  br label %exit
conditional.1:
; CHECK-NEXT: [SDS] Analyzed outgoing call   %awaited.1.struct {{.*}}lgc.cps.await{{.*}} %args.modified)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] DCPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  %awaited.1.struct = call %awaited.args.type (...) @lgc.cps.await__(i32 poison, i32 poison, i32 poison, %args.type %args.modified)
  %awaited.1 = extractvalue %awaited.args.type %awaited.1.struct, 0
  br label %exit
exit:
  %args.final = phi %args.type [ %awaited.0, %conditional.0 ], [ %awaited.1, %conditional.1 ], [ %args.modified, %entry ]
; CHECK:      [SDS] Analyzed outgoing call {{.*}} @lgc.cps.jump({{.*}} %args.final)
; CHECK-NEXT: [SDS] 0         1         2         3{{$}}
; CHECK-NEXT: [SDS] 0123456789012345678901234567890{{$}}
; CHECK-NEXT: [SDS] DCPPPPPPPPPPPPPPPPPPPPPPPPPPPPP{{$}}
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %args.final)
  unreachable
}

declare [4 x i32] @opaqueCandidate()

; Traversal shader that contains jumps to an AHS setting a dynamic candidate, and a return back to raygen that preserves only parts of the args.
; CHECK-LABEL: [SDS] Analyzing function Traversal1 (shader stage compute)
define void @Traversal1({}, i32 %ret.addr, i32, { [2 x i32], [8 x i32] } %system.data, [4 x i32] %padding, [8 x i32] %payload) !lgc.rt.shaderstage !6 {
  %cond = trunc i32 %ret.addr to i1
  br i1 %cond, label %rgs.resume, label %ahs
ahs:
  %ahs.system.data.0 = insertvalue { { [2 x i32], [8 x i32] }, [4 x i32] } poison, { [2 x i32], [8 x i32] } %system.data, 0
  %candidate = call [4 x i32] @opaqueCandidate()
  %ahs.system.data = insertvalue { { [2 x i32], [8 x i32] }, [4 x i32] } %ahs.system.data.0, [4 x i32] %candidate, 1
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, { { [2 x i32], [8 x i32] }, [4 x i32] } %ahs.system.data, [8 x i32] %payload)
  unreachable
rgs.resume:
  %dispatch.system.data = extractvalue { [2 x i32], [8 x i32] } %system.data, 0
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, [2 x i32] %dispatch.system.data, [12 x i32] poison, [8 x i32] %payload)
  unreachable
; CHECK-LABEL: [SDS] Finished analysis of function Traversal1
; CHECK-NEXT:  [SDS] 0         1         2
; CHECK-NEXT:  [SDS] 0123456789012345678901
; CHECK-NEXT:  [SDS] PPUUUUUUUUDDDDPPPPPPPP
}

; Same as above, but without padding args.
; Hypothetical traversal calling an AHS with a larger arg size, and a RGS with smaller arg size.
; This tests mismatching incoming vs outgoing arg sizes.
; CHECK-LABEL: [SDS] Analyzing function Traversal2 (shader stage compute)
define void @Traversal2({}, i32 %ret.addr, i32, { [2 x i32], [8 x i32] } %system.data, [8 x i32] %payload) !lgc.rt.shaderstage !6 {
  %cond = trunc i32 %ret.addr to i1
  br i1 %cond, label %rgs.resume, label %ahs
ahs:
  %ahs.system.data.0 = insertvalue { { [2 x i32], [8 x i32] }, [4 x i32] } poison, { [2 x i32], [8 x i32] } %system.data, 0
  %candidate = call [4 x i32] @opaqueCandidate()
  %ahs.system.data = insertvalue { { [2 x i32], [8 x i32] }, [4 x i32] } %ahs.system.data.0, [4 x i32] %candidate, 1
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, { { [2 x i32], [8 x i32] }, [4 x i32] } %ahs.system.data, [8 x i32] %payload)
; CHECK-NEXT: [SDS] 0         1         2
; CHECK-NEXT: [SDS] 0123456789012345678901
; CHECK-NEXT: [SDS] PPPPPPPPPPDDDDDDDDDDDD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, { { [2 x i32], [8 x i32] }, [4 x i32] } %ahs.system.data, [8 x i32] %payload)
  unreachable
rgs.resume:
  %dispatch.system.data = extractvalue { [2 x i32], [8 x i32] } %system.data, 0
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, [2 x i32] %dispatch.system.data, [8 x i32] %payload)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 0123456789
; CHECK-NEXT: [SDS] PPDDDDDDDD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, [2 x i32] %dispatch.system.data, [8 x i32] %payload)
  unreachable
; CHECK-NEXT: [SDS] Finished analysis of function Traversal2
; CHECK-NEXT: [SDS] 0         1         2
; CHECK-NEXT: [SDS] 0123456789012345678901
; CHECK-NEXT: [SDS] PPDDDDDDDDDDDDDDDDDDDD
}

; %args.with.padding requires 6 registers as argument, but 8 dwords in memory
; Test that we correctly map the argument slots into the in-memory type layout,
; by extracting the individual dword values, and passing them as scalars to an outgoing jump.
; This should be detected as preserve.
; CHECK-LABEL: [SDS] Analyzing function JumpWithPaddingInType
define void @JumpWithPaddingInType({}, i32 %ret.addr, i32, %args.with.padding %args) !lgc.rt.shaderstage !2 {
  %scalar.0 = extractvalue %args.with.padding %args, 0
  %scalar.1 = extractvalue %args.with.padding %args, 1
  %scalar.2 = extractvalue %args.with.padding %args, 2, 0
  %scalar.3 = extractvalue %args.with.padding %args, 2, 1
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i32 %scalar.0, i64 %scalar.1, i32 %scalar.2, i64 %scalar.3)
  unreachable
; CHECK-LABEL: [SDS] Finished analysis of function JumpWithPaddingInType
; CHECK-NEXT:  [SDS] 0
; CHECK-NEXT:  [SDS] 012345
; CHECK-NEXT:  [SDS] PPPPPP
}

; Same as above, but for awaits results.
; CHECK-LABEL: [SDS] Analyzing function AwaitWithPaddingInType
define void @AwaitWithPaddingInType({}, i32 %ret.addr, i32, %args.with.padding %args) !lgc.rt.shaderstage !1 {
  ; Intentionally do not wrap %args in a struct -- instead pretend the await function returns
  ; the elements of %args as separate args, so we can test the mapping of arg slots into the returned struct
  ; with multiple struct elements.
  %awaited = call %args.with.padding (...) @lgc.cps.await__p(i32 poison, i32 poison, i32 poison, %args.with.padding %args)
  %scalar.0 = extractvalue %args.with.padding %awaited, 0
  %scalar.1 = extractvalue %args.with.padding %awaited, 1
  %scalar.2 = extractvalue %args.with.padding %awaited, 2, 0
  %scalar.3 = extractvalue %args.with.padding %awaited, 2, 1
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i32 %scalar.0, i64 %scalar.1, i32 %scalar.2, i64 %scalar.3)
  unreachable
; CHECK-LABEL: [SDS] Finished analysis of function AwaitWithPaddingInType
; CHECK-NEXT:  [SDS] 0
; CHECK-NEXT:  [SDS] 012345
; CHECK-NEXT:  [SDS] PPPPPP
}

; Check that we don't treat a single passed-through i16 as preserve. The high outgoing bits are poison,
; so in theory we could treat this as preserve, because only non-poison bits are relevant for the analysis,
; but currently we handle i16s conservatively. Properly supporting i16s is complicated, because incoming poison
; bits that might even be implicit in the in-memory representation of a type need to be accounted for.
; For instance, consider the example that forwards an incoming <2 x i16> argument to a bitcast outgoing i32 argument
; in the JumpWithOverlappingi16s test case.
; CHECK-LABEL: [SDS] Analyzing function JumpWithSinglei16
define void @JumpWithSinglei16({}, i32 %ret.addr, i32, i16 %arg) !lgc.rt.shaderstage !2 {
; Forward arg as-is.
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %arg)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] D
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %arg)
  unreachable
}

; Check that we don't treat a misaligned passed-through dword as preserve. Use a packed struct to force misalignment.
; CHECK-LABEL: [SDS] Analyzing function JumpWithMisalignedDword
define void @JumpWithMisalignedDword({}, i32 %ret.addr, i32, <{ i16, i32 }> %args) !lgc.rt.shaderstage !2 {
  switch i32 %ret.addr, label %conditional.0 [
    i32 0, label %conditional.0
    i32 1, label %conditional.1
  ]
conditional.0:
; Forward args as-is.
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, <{ i16, i32 }> %args)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 01
; CHECK-NEXT: [SDS] DD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, <{ i16, i32 }> %args)
  unreachable
conditional.1:
; Forward extracted scalars.
  %scalar.0 = extractvalue <{ i16, i32 }> %args, 0
  %scalar.1 = extractvalue <{ i16, i32 }> %args, 1
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %scalar.0, i32 %scalar.1)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 01
; CHECK-NEXT: [SDS] DD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %scalar.0, i32 %scalar.1)
  unreachable
  unreachable
}

; All cases involving i16 scalars should not be treated as preserve, as the i16 cannot guarantee to preserve high bits.
; Additionally, there can be issues with alignment.
; CHECK-LABEL: [SDS] Analyzing function JumpWithOverlappingi16s
define void @JumpWithOverlappingi16s({}, i32 %ret.addr, i32, <2 x i16> %args) !lgc.rt.shaderstage !2 {
  switch i32 %ret.addr, label %conditional.2 [
    i32 0, label %conditional.0
    i32 1, label %conditional.1
    i32 2, label %conditional.2
  ]
conditional.0:
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, <2 x i16> %args)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 01
; CHECK-NEXT: [SDS] DD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, <2 x i16> %args)
  unreachable
conditional.1:
; Forward extracted scalars. This preserves arg slots, but we can't detect it.
  %scalar.0 = extractelement <2 x i16> %args, i32 0
  %scalar.1 = extractelement <2 x i16> %args, i32 1
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %scalar.0, i16 %scalar.1)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 01
; CHECK-NEXT: [SDS] DD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %scalar.0, i16 %scalar.1)
  unreachable
conditional.2:
; Forward just the bitcast. This does *not* preserve arg slots, as we merge both i16s into a single i32 arg slot.
; Even when relaxing i16 handling and allowing to treat forwarded i16 arguments as preserve, exploiting that the high bits
; are poison, we may not treat this as preserve. A naive implementation that just compares the value origin of the
; outgoing %bitcast argument with the corresponding incoming argument slot (value %args, offset 0) might come to the conclusion that it is
; preserved. But when allowing i16s, we need to additionally account for the incoming high poison bits that are implicit
; in the in-memory representation of %args.
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i32 %bitcast)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] D
  %bitcast = bitcast <2 x i16> %args to i32
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i32 %bitcast)
  unreachable
}

; Same as above, but with awaits.
; CHECK-LABEL: [SDS] Analyzing function AwaitWithOverlappingi16s
define void @AwaitWithOverlappingi16s({}, i32 %ret.addr, i32, <2 x i16> %args) !lgc.rt.shaderstage !2 {
  switch i32 %ret.addr, label %conditional.2 [
    i32 0, label %conditional.0
    i32 1, label %conditional.1
    i32 2, label %conditional.2
  ]
conditional.0:
; Forward args as-is through an await.
  %awaited.0.struct = call { <2 x i16> } (...) @lgc.cps.await__2xi16(i32 poison, i32 poison, i32 poison, <2 x i16> %args)
  %awaited.0 = extractvalue { <2 x i16> } %awaited.0.struct, 0
; CHECK-LABEL: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, <2 x i16> %awaited.0)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 01
; CHECK-NEXT: [SDS] DD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, <2 x i16> %awaited.0)
  unreachable
conditional.1:
; Forward extracted scalars through an await.
  %scalar.0 = extractelement <2 x i16> %args, i32 0
  %scalar.1 = extractelement <2 x i16> %args, i32 1
  %awaited.1.struct = call { i16, i16 } (...) @lgc.cps.await__i16i16(i32 poison, i32 poison, i32 poison, i16 %scalar.0, i16 %scalar.1)
  %awaited.1.0 = extractvalue { i16, i16 } %awaited.1.struct, 0
  %awaited.1.1 = extractvalue { i16, i16 } %awaited.1.struct, 1
; CHECK:      [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %awaited.1.0, i16 %awaited.1.1)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 01
; CHECK-NEXT: [SDS] DD
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i16 %awaited.1.0, i16 %awaited.1.1)
  unreachable
conditional.2:
; Forward just the bitcast. This does *not* preserve arg slots, as we merge both i16s into a single arg slot.
; CHECK-NEXT: [SDS] Analyzed outgoing call   call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i32 %bitcast)
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] 0
; CHECK-NEXT: [SDS] D
  %bitcast = bitcast <2 x i16> %args to i32
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, i32 %bitcast)
  unreachable
}

; Check that we ignore callable shaders
define void @Callable({}, i32 %ret.addr, i32, %args.type %args) !lgc.rt.shaderstage !5 {
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %args)
  unreachable
; CHECK-NOT: [SDS] Finished analysis of function Callable
}

; Check that we ignore launch kernel shaders
define void @LaunchKernel({}, i32 %ret.addr, i32, %args.type %args) !lgc.rt.shaderstage !7 {
  call void (...) @lgc.cps.jump(i32 poison, i32 poison, {} poison, i32 poison, i32 poison, i32 poison, %args.type %args)
  unreachable
; CHECK-NOT: [SDS] Finished analysis of function LaunchKernel
}

; CHECK: [SDS] Serialized state to MD:
!lgc.cps.module = !{}
!lgc.rt.specialize.driver.shaders.process.in.instruction.order = !{}

!1 = !{i32 1} ; Intersection
!2 = !{i32 2} ; AHS
!5 = !{i32 5} ; Callable
!6 = !{i32 6} ; Traversal
!7 = !{i32 7} ; KernelEntry
