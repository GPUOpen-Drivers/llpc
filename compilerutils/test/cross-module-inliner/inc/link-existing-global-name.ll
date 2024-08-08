@glob = external global i32

define i32 @inline_fun() {
  %result = load i32, ptr @glob
  ret i32 %result
}
