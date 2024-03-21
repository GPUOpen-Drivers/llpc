; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --include-generated-funcs --version 3
; RUN: opt --verify-each -S  -o - -passes='lower-await' %s | FileCheck --check-prefixes=LOWER-AWAIT %s

define void @_cont_Traversal() !lgc.cps !{i32 2} !continuation !{ptr @_cont_Traversal} {
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 32)
  %fn = load ptr, ptr addrspace(4) %pushconst
  %cr = ptrtoint ptr %fn to i32
  call void (...) @lgc.cps.jump(i32 %cr, i32 2, {} poison, i32 poison)
  unreachable
}

!lgc.cps.module = !{}

declare ptr addrspace(4) @lgc.user.data(i32)
declare void @lgc.cps.jump(...)
; LOWER-AWAIT-LABEL: define { ptr, ptr } @_cont_Traversal(
; LOWER-AWAIT-SAME: ptr [[TMP0:%.*]]) !lgc.cps [[META0:![0-9]+]] !continuation [[META1:![0-9]+]] {
; LOWER-AWAIT-NEXT:    [[TMP2:%.*]] = call token @llvm.coro.id.retcon(i32 8, i32 4, ptr [[TMP0]], ptr @continuation.prototype._cont_Traversal, ptr @continuation.malloc, ptr @continuation.free)
; LOWER-AWAIT-NEXT:    [[TMP3:%.*]] = call ptr @llvm.coro.begin(token [[TMP2]], ptr null)
; LOWER-AWAIT-NEXT:    [[PUSHCONST:%.*]] = call ptr addrspace(4) @lgc.user.data(i32 32)
; LOWER-AWAIT-NEXT:    [[FN:%.*]] = load ptr, ptr addrspace(4) [[PUSHCONST]], align 8
; LOWER-AWAIT-NEXT:    [[CR:%.*]] = ptrtoint ptr [[FN]] to i32
; LOWER-AWAIT-NEXT:    call void (...) @lgc.cps.jump(i32 [[CR]], i32 2, {} poison, i32 poison)
; LOWER-AWAIT-NEXT:    unreachable
;
