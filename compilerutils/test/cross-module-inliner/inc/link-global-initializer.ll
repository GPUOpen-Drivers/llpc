@glob = external global i32
@glob.1 = global ptr @glob

define i32 @inline_fun() {
  %result = ptrtoint ptr @glob.1 to i32
  ret i32 %result
}
