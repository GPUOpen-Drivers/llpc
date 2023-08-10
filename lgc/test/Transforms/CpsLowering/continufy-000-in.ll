; This is an example test case for the continufy pass.
; For a possible example output, see the -out file.

; Note:
;  - !continufy.stage metadata added in llpcSpirvLowerRaytracing to functions and calls
;  - use lgc::rt::RayTracingShaderStage enum values
;  - use -1 to indicate traversal
; 

define spir_func void @raygen() !lgc.shaderstage !{i32 7} !continufy.stage !{i32 0} {
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 0)
  %fn = load ptr, ptr addrspace(4) %pushconst
  %p8 = getelementptr i8, ptr addrspace(4) %pushconst, i32 8
  %x = load i32, ptr addrspace(4) %p8
  %p16 = getelementptr i8, ptr addrspace(4) %pushconst, i32 16
  %dst = load ptr addrspace(1), ptr addrspace(4) %p16
  %r = call spir_func [2 x i32] %fn(i32 %x, ptr addrspace(1) %dst) !continufy.stage !{i32 -1}
  store [2 x i32] %r, ptr addrspace(1) %dst
  ret void
}

define spir_func i32 @chs(i32 %x) !lgc.shaderstage !{i32 7} !continufy.stage !{i32 3} {
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 24)
  %fn = load ptr, ptr addrspace(4) %pushconst
  %y = call spir_func i32 %fn(i32 %x) !continufy.stage !{i32 5}
  ret i32 %y
}

; Note: No !continufy.stage metadata here
define dllexport void @lgc.shader.CS.main() !lgc.shaderstage !{i32 7} {
entry:
  %id = call i32 @lgc.shader.input.LocalInvocationId(i32 49)
  %live = icmp ult i32 %id, 29
  br i1 %live, label %main, label %exit

main:
  %pushconst = call ptr addrspace(4) @lgc.user.data(i32 32)
  %fn = load ptr, ptr addrspace(4) %pushconst
  call spir_func void %fn() !continufy.stage !{i32 0}
  br label %exit

exit:
  ret void
}

declare ptr addrspace(4) @lgc.user.data(i32)
declare i32 @lgc.shader.input.LocalInvocationId(i32)
