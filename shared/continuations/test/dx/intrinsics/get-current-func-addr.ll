; RUN: opt --verify-each -passes='dxil-cont-post-process,lint' -S %s 2>%t.stderr | FileCheck %s
; RUN: count 0 < %t.stderr

declare void @Use(i64)
declare i64 @_AmdGetCurrentFuncAddr()

; CHECK-LABEL: define void @MyRayGen
; CHECK: [[FUNC_PTR_AS_I64:%.*]] = {{.*}} @MyRayGen to i64
; CHECK: call void @Use(i64 [[FUNC_PTR_AS_I64]])
define void @MyRayGen() {
AllocaSpillBB:
  %val = call i64 @_AmdGetCurrentFuncAddr()
  call void @Use(i64 %val)
  ret void
}

; CHECK-LABEL: define void @MyRayGen.resume.0
; CHECK: [[FUNC_PTR_AS_I64:%.*]] = {{.*}} @MyRayGen.resume.0 to i64
; CHECK: call void @Use(i64 [[FUNC_PTR_AS_I64]])
define void @MyRayGen.resume.0() {
entryresume.0:
  %val = call i64 @_AmdGetCurrentFuncAddr()
  call void @Use(i64 %val)
  ret void
}

