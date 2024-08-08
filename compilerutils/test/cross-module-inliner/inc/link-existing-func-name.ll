; A different main function
declare i32 @main(i32)

define i32 @inline_fun(i32 %arg) {
  %result = call i32 @main(i32 %arg)
  ret i32 %result
}
