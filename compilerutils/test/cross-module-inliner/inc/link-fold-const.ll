@glob = global ptr @glob.1
@glob.1 = global i32 5

define i32 @inline_fun() {
  %result = ptrtoint ptr @glob to i32
  ret i32 %result
}
