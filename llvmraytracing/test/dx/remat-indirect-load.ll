; NOTE: Do not autogenerate
; RUN: opt -debug-only=dxil-coro-split -passes='dxil-coro-split' -S %s 2>&1 | FileCheck %s
;
; Test that an indirect handle load pattern does not produce a rematerialization
; warning. We know that remat in this case is not profitable.
;
; REQUIRES: assertions

; CHECK-NOT: Warning: isRematerializableDxilLoad unhandled pattern: {{.*}} = extractvalue %dx.types.ResRet.i32

target datalayout = "e-m:e-p:64:32-p20:32:32-p21:32:32-p32:32:32-i1:32-i8:8-i16:16-i32:32-i64:32-f16:16-f32:32-f64:32-v8:8-v16:16-v32:32-v48:32-v64:32-v80:32-v96:32-v112:32-v128:32-v144:32-v160:32-v176:32-v192:32-v208:32-v224:32-v240:32-v256:32-n8:16:32"
target triple = "dxil-ms-dx"

%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }
%dx.types.Handle = type { ptr }

; Function Attrs: presplitcoroutine
define { ptr, ptr } @"indirect_handle_load"() #0 {
_cont_RayTCurrent.exit:
  %0 = call token @llvm.coro.id.retcon(i32 0, i32 0, ptr null, ptr @"continuation.prototype.indirect_handle_load", ptr @continuation.malloc, ptr @continuation.free)
  %1 = call ptr @llvm.coro.begin(token %0, ptr null)
  %2 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 0, %dx.types.Handle zeroinitializer)
  %3 = extractvalue %dx.types.ResRet.i32 %2, 0
  %4 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 0, i32 %3)
  %5 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 0, %dx.types.Handle %4)
  ret { ptr, ptr } zeroinitializer
}

declare %dx.types.Handle @dx.op.createHandleFromHeap(i32, i32)

declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle)

declare ptr @continuation.malloc(i32)

declare void @continuation.free(ptr)

; Function Attrs: nounwind
declare token @llvm.coro.id.retcon(i32, i32, ptr, ptr, ptr, ptr) #1

; Function Attrs: nounwind
declare ptr @llvm.coro.begin(token, ptr writeonly) #1

declare { ptr, ptr } @"continuation.prototype.indirect_handle_load"(ptr)

attributes #0 = { presplitcoroutine }
attributes #1 = { nounwind }
