@glob = global ptr @glob.1
@glob.1 = global i32 5

define i32 @inline_fun(i32 %arg) {
  %result = add i32 %arg, 1
  %cond = icmp uge i32 %result, 5
  br i1 %cond, label %ret, label %ret2

ret:
  ret i32 %result

ret2:
  %res2 = ptrtoint ptr @glob to i32
  ret i32 %res2
}
