%struct.MyStruct = type { i32, i32 }

@glob = external global %struct.MyStruct

define i32 @inline_fun() {
  %result = load i32, ptr getelementptr inbounds (%struct.MyStruct, ptr @glob, i64 0, i32 1)
  ret i32 %result
}
