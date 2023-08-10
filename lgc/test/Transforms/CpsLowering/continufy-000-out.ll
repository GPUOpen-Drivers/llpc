; This is example output for running continufy on the -in file.
;
; Details of the output are likely to differ from the final production pass,
; especially instruction order and value names.

define spir_func void @raygen({} %state, i32 %rcr) !lgc.shaderstage !{i32 7} !lgc.cps !{i32 0} {
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 0)
  %fn = load ptr, ptr addrspace(4) %pushconst
  %p8 = getelementptr i8, ptr addrspace(4) %pushconst, i32 8
  %x = load i32, ptr addrspace(4) %p8
  %p16 = getelementptr i8, ptr addrspace(4) %pushconst, i32 16
  %dst = load ptr addrspace(1), ptr addrspace(4) %p16

  %cr.0 = ptrtoint ptr %fn to i32
  %cr.1 = or i32 %cr.0, 2
  %r = call [2 x i32] @lgc.cps.await.a2i32(i32 %cr.1, i32 4, i32 %x, ptr addrspace(1) %dst)

  store [2 x i32] %r, ptr addrspace(1) %dst

  ; Note: RGS returns, meaning end of thread.
  ret void
}

define spir_func i32 @chs({} %state, i32 %rcr, i32 %x) !lgc.shaderstage !{i32 7} !lgc.cps !{i32 1} {
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 24)
  %fn = load ptr, ptr addrspace(4) %pushconst

  %cr.0 = ptrtoint ptr %fn to i32
  %cr.1 = or i32 %cr.0, 1
  %y = call i32 @lgc.cps.await.i32(i32 %cr.1, i32 2, i32 %)

  call void @lgc.cps.jump(i32 %rcr, i32 5, i32 %y)
  ret i32 %y
}

define dllexport void @lgc.shader.CS.main() !lgc.shaderstage !{i32 7} {
entry:
  %id = call i32 @lgc.shader.input.LocalInvocationId(i32 49)
  %live = icmp ult i32 %id, 29
  br i1 %live, label %main, label %exit

main:
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 32)
  %fn = load ptr, ptr addrspace(4) %pushconst

  %cr.0 = ptrtoint ptr %fn to i32
  call void @lgc.cps.await.void(i32 %cr.0, i32 1)

  br label %exit

exit:
  ; Note: Entry kernel also returns
  ret void
}

declare ptr addrspace(4) @lgc.user.data(i32)
declare i32 @lgc.shader.input.LocalInvocationId(i32)
declare void @lgc.cps.await.void(...)
declare i32 @lgc.cps.await.i32(...)
declare [2 x i32] @lgc.cps.await.a2i32(...)
