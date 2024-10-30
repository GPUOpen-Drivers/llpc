%struct.MyClass.1 = type { i32 }

define i32 @inline_fun(ptr nocapture readonly %c) {
  %1 = getelementptr inbounds %struct.MyClass.1, ptr %c, i32 0, i32 0
  %result = load i32, ptr %1, align 4
  ret i32 %result
}

define i32 @inline_fun_struct(%struct.MyClass.1 %c) {
  %result = extractvalue %struct.MyClass.1 %c, 0
  ret i32 %result
}
