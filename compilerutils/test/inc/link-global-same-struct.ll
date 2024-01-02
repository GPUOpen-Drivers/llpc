%struct.MyStruct = type { i32, i32 }

@glob = external global %struct.MyStruct

define i32 @inline_fun() {
  %ptr = getelementptr %struct.MyStruct, ptr @glob, i32 0, i32 1
  %result = load i32, ptr %ptr
  ret i32 %result
}
