; RUN: opt -passes="value-origin-tracking-test" -S %s | FileCheck %s

declare void @analyze(...)

define void @testConstantInt() {
; CHECK-LABEL: testConstantInt

; CHECK: (i1 true): Constant: 0x1
  call void @analyze(i1 true)

; CHECK: (i8 16): Constant: 0x10
  call void @analyze(i8 16)

; CHECK: (i16 17): Constant: 0x11
  call void @analyze(i16 17)

; CHECK: (i32 64): Constant: 0x40
  call void @analyze(i32 64)

; CHECK: (i64 4294967311): Constant: 0xf; Constant: 0x1
  call void @analyze(i64 u0x10000000f)

  ret void
}

define void @testConstantFloat() {
; CHECK-LABEL: testConstantFloat

; CHECK: (half 0xH1234): Constant: 0x1234
  call void @analyze(half 0xH1234)

; CHECK: (float 1.250000e-01): Constant: 0x3e000000
  call void @analyze(float 1.250000e-01)

; CHECK: (double 0x123456789ABCDEF): Constant: 0x89abcdef; Constant: 0x1234567
  call void @analyze(double 0x0123456789abcdef)

; Check that float "zero" is not incorrectly handled as "null"
; CHECK: (float -0.000000e+00): Constant: 0x80000000
  call void @analyze(float -0.0)

; CHECK: (float 1.250000e-01): Constant: 0x3e000000
  call void @analyze(float bitcast (i32 u0x3e000000 to float))
  ret void
}

define void @testConstantVector() {
; CHECK-LABEL: testConstantVector

; CHECK: (<2 x i32> zeroinitializer): Constant: 0x0; Constant: 0x0
  call void @analyze(<2 x i32> zeroinitializer)

; CHECK: (<9 x i8> zeroinitializer): Constant: 0x0; Constant: 0x0; Constant: 0x0
  call void @analyze(<9 x i8> zeroinitializer)

; CHECK: (<1 x i32> {{(splat \(i32 \-559038737\))|(><i32 -559038737>)}}): Constant: 0xdeadbeef
  call void @analyze(<1 x i32> <i32 u0xdeadbeef>)

; CHECK: (<4 x i8> <i8 1, i8 2, i8 3, i8 4>): Constant: 0x4030201
  call void @analyze(<4 x i8> <i8 1, i8 2, i8 3, i8 4>)

; CHECK: (<1 x float> {{(splat \(float 1\.250000e\-01\))|(<float 1\.250000e\-01>)}}): Constant: 0x3e000000
  call void @analyze(<1 x float> <float 1.250000e-01>)

; computeKnownBits only supports integer vectors, and our
; handling doesn't support smaller-than-slice element types.
; CHECK: (<1 x half> {{(splat \(half 0xH1234\))|(<half 0xH1234>)}}): Dynamic
  call void @analyze(<1 x half> <half 0xH1234>)

; CHECK: (<4 x float> <float 0.000000e+00, float 2.560000e+02, float 0.000000e+00, float undef>): Constant: 0x0; Constant: 0x43800000; Constant: 0x0; UndefOrPoison
  call void @analyze(<4 x float> <float 0.000000e+00, float bitcast (i32 u0x43800000 to float), float 0.000000e+00, float undef>)

  ret void
}

define void @testConstantArray() {
; CHECK-LABEL: testConstantArray

; CHECK: ([2 x i32] zeroinitializer): Constant: 0x0; Constant: 0x0
  call void @analyze([2 x i32] zeroinitializer)

; CHECK: ([9 x i8] zeroinitializer): Constant: 0x0; Constant: 0x0; Constant: 0x0
  call void @analyze([9 x i8] zeroinitializer)

; CHECK: ([1 x i32] [i32 -559038737]): Constant: 0xdeadbeef
  call void @analyze([1 x i32] [i32 u0xdeadbeef])

; In contrast to vectors, we can't detect constant arrays of small types.
; This is because llvm computeKnownBits supports vectors but not arrays,
; and our handling of constant arrays/vectors doesn't support element types
; smaller than slices.
; CHECK: ([4 x i8] c"\01\02\03\04"): Dynamic
  call void @analyze([4 x i8] [i8 1, i8 2, i8 3, i8 4])

; CHECK: ([1 x float] [float 1.250000e-01]): Constant: 0x3e000000
  call void @analyze([1 x float] [float 1.250000e-01])

; CHECK: ([4 x float] [float 0.000000e+00, float 2.560000e+02, float 0.000000e+00, float undef]): Constant: 0x0; Constant: 0x43800000; Constant: 0x0; UndefOrPoison
  call void @analyze([4 x float] [float 0.000000e+00, float bitcast (i32 u0x43800000 to float), float 0.000000e+00, float undef])

  ret void
}

%somestruct = type { i32, i8, half }
define void @testConstantStruct() {
; CHECK-LABEL: testConstantStruct
; Only support zeroinitializer for now

; CHECK: (%somestruct zeroinitializer): Constant: 0x0; Constant: 0x0
  call void @analyze(%somestruct zeroinitializer)

; CHECK: (%somestruct { i32 1, i8 1, half 0xH0000 }): Dynamic: {{.*}} (offset 0); Dynamic: {{.*}} (offset 4)
  call void @analyze(%somestruct { i32 1, i8 1, half 0xH0 })

  ret void
}

define void @testDynamic(i32 %arg) {
; CHECK-LABEL: testDynamic
; CHECK: (i32 %arg): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(i32 %arg)
  ret void
}

define void @testPoison() {
; CHECK-LABEL: testPoison
; CHECK: (i1 poison): UndefOrPoison
  call void @analyze(i1 poison)
; CHECK: (i32 poison): UndefOrPoison
  call void @analyze(i32 poison)
; CHECK: (double poison): UndefOrPoison; UndefOrPoison
  call void @analyze(double poison)

; See freeze-mode.ll for detailed freeze tests.
  %freezePoison = freeze i32 poison
; CHECK: (  %freezePoison = {{.*}}): Dynamic
  call void @analyze(i32 %freezePoison)

  %freezeNonPoison = freeze i32 5
; CHECK: (  %freezeNonPoison = {{.*}}): Constant: 0x5
  call void @analyze(i32 %freezeNonPoison)
  ret void
}

define void @testArray(i32 %arg) {
; CHECK-LABEL: testArray
  %arr.1 = insertvalue [3 x i32] poison, i32 100, 0
  %arr.2 = insertvalue [3 x i32] %arr.1, i32 %arg, 1
  %extract.0 = extractvalue [3 x i32] %arr.2, 0
  %extract.1 = extractvalue [3 x i32] %arr.2, 1
  %extract.2 = extractvalue [3 x i32] %arr.2, 2
; CHECK: (  %extract.0 = extractvalue [3 x i32] %arr.2, 0): Constant: 0x64
  call void @analyze(i32 %extract.0)
; CHECK: (  %extract.1 = extractvalue [3 x i32] %arr.2, 1): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(i32 %extract.1)
; CHECK: (  %extract.2 = extractvalue [3 x i32] %arr.2, 2): UndefOrPoison
  call void @analyze(i32 %extract.2)
  ret void
}

define void @testVector(i32 %arg) {
; CHECK-LABEL: testVector
  %vec.1 = insertelement <3 x i32> poison, i32 100, i32 0
  %vec.2 = insertelement <3 x i32> %vec.1, i32 %arg, i32 1
  %extract.0 = extractelement <3 x i32> %vec.2, i32 0
  %extract.1 = extractelement <3 x i32> %vec.2, i32 1
  %extract.2 = extractelement <3 x i32> %vec.2, i32 2
  %extract.dyn = extractelement <3 x i32> %vec.2, i32 %arg
; CHECK: (  %extract.0 = extractelement <3 x i32> %vec.2, i32 0): Constant: 0x64
  call void @analyze(i32 %extract.0)
; CHECK: (  %extract.1 = extractelement <3 x i32> %vec.2, i32 1): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(i32 %extract.1)
; CHECK: (  %extract.2 = extractelement <3 x i32> %vec.2, i32 2): UndefOrPoison
  call void @analyze(i32 %extract.2)
; CHECK: (  %extract.dyn = extractelement <3 x i32> %vec.2, i32 %arg): Dynamic:   %extract.dyn = extractelement <3 x i32> %vec.2, i32 %arg (offset 0)
  call void @analyze(i32 %extract.dyn)

  ; Test that inserting an i1 into the middle of a dword doesn't accidentally overwrite high bits
  %insert.i1 = insertelement <32 x i1> zeroinitializer, i1 1, i32 16
; CHECK: (  %insert.i1 = {{.*}}): Dynamic
  call void @analyze(<32 x i1> %insert.i1)

  ret void
}

define void @testBitcast(i32 %arg) {
; CHECK-LABEL: testBitcast
  %bitcast = bitcast i32 %arg to float
; CHECK: (  %bitcast = bitcast i32 %arg to float): Dynamic (argument): i32 %arg (offset 0)
  call void @analyze(float %bitcast)
  ret void
}

define void @testSelect(i32 %arg1, i1 %arg2, i1 %arg3, i32 %arg4, i1 %arg5, i1 %arg6, i1 %arg7) {
; CHECK-LABEL: testSelect
  %sel.1 = select i1 %arg2, i32 %arg1, i32 -1
; CHECK: (  %sel.1 = select i1 %arg2, i32 %arg1, i32 -1): (Constant: 0xffffffff | Dynamic (argument): i32 %arg1 (offset 0))
  call void @analyze(i32 %sel.1)

; Consistent constant, in different order:
  %sel.2 = select i1 %arg3, i32 -1, i32 %sel.1
; CHECK: (  %sel.2 = select i1 %arg3, i32 -1, i32 %sel.1): (Constant: 0xffffffff | Dynamic (argument): i32 %arg1 (offset 0))
  call void @analyze(i32 %sel.2)

; Inconsistent constants mean we don't know anything:
  %sel.3 = select i1 %arg5, i32 %sel.2, i32 0
; CHECK: (  %sel.3 = select i1 %arg5, i32 %sel.2, i32 0): Dynamic:   %sel.3 = select i1 %arg5, i32 %sel.2, i32 0 (offset 0)
  call void @analyze(i32 %sel.3)

; Consistent dynamic value:
  %arg1.bc.float = bitcast i32 %arg1 to float
  %arg1.bc.i32 = bitcast float %arg1.bc.float to i32
  %sel.4 = select i1 %arg6, i32 %sel.2, i32 %arg1.bc.i32
; CHECK: (  %sel.4 = select i1 %arg6, i32 %sel.2, i32 %arg1.bc.i32): (Constant: 0xffffffff | Dynamic (argument): i32 %arg1 (offset 0))
  call void @analyze(i32 %sel.4)

; Inconsistent dynamic value means we don't know anything
  %sel.5 = select i1 %arg6, i32 %sel.2, i32 %arg4
; CHECK: (  %sel.5 = select i1 %arg6, i32 %sel.2, i32 %arg4): Dynamic:   %sel.5 = select i1 %arg6, i32 %sel.2, i32 %arg4 (offset 0)
  call void @analyze(i32 %sel.5)

; Add in poison:
  %sel.6 = select i1 %arg7, i32 %sel.2, i32 poison
; CHECK: (  %sel.6 = select i1 %arg7, i32 %sel.2, i32 poison): (UndefOrPoison | Constant: 0xffffffff | Dynamic (argument): i32 %arg1 (offset 0))
  call void @analyze(i32 %sel.6)

  ret void
}

define void @testPhi(i32 %arg1, i1 %arg2, i1 %arg3, [5 x i32] %arg4) {
; CHECK-LABEL: testPhi
entry:
  %empty = phi i32
; CHECK: (  %empty = phi i32 ): Dynamic:   %empty = phi i32  (offset 0)
  call void @analyze(i32 %empty)

  br i1 %arg2, label %bb1, label %bb2
bb1:
  %phi.arg = phi i32 [ %arg1, %entry ]
; CHECK: (  %phi.arg = phi i32 [ %arg1, %entry ]): Dynamic (argument): i32 %arg1 (offset 0)
  call void @analyze(i32 %phi.arg)
  br label %bb2
bb2:
  %phi.argOrConst = phi i32 [ %arg1, %entry ], [ 1, %bb1]
; CHECK: (  %phi.argOrConst = phi i32 [ %arg1, %entry ], [ 1, %bb1 ]): (Constant: 0x1 | Dynamic (argument): i32 %arg1 (offset 0))
  call void @analyze(i32 %phi.argOrConst)
  br label %loop.entry
loop.entry:
  %phi.loop.constant = phi i32 [ 1, %bb2 ], [ 1, %loop.entry]
  %phi.loop.propagate = phi i32 [ 1, %bb2 ], [ %phi.loop.propagate, %loop.entry]

; CHECK: (  %phi.loop.constant = phi i32 [ 1, %bb2 ], [ 1, %loop.entry ]): Constant: 0x1
  call void @analyze(i32 %phi.loop.constant)
; %phi.loop.propagate is always constant, but figuring this out requires propagating
; multiple times through the loop until a stable state is reached, which we don't do:
; CHECK: (  %phi.loop.propagate = {{.*}}: Dynamic:   %phi.loop.propagate = phi
  call void @analyze(i32 %phi.loop.propagate)
  br i1 %arg3, label %loop.entry, label %bb.startmany
bb.startmany:
  switch i32 %arg1, label %exit [ i32 0, label %bb.many.0
                                  i32 1, label %bb.many.1
                                  i32 2, label %bb.many.2
                                  i32 3, label %bb.many.3
                                  i32 4, label %bb.many.4 ]
bb.many.0:
  %arr.0 = insertvalue [5 x i32] %arg4, i32 0, 0
  br label %bb.many.exit
bb.many.1:
  %arr.1 = insertvalue [5 x i32] %arg4, i32 1, 1
  br label %bb.many.exit
bb.many.2:
  %arr.2 = insertvalue [5 x i32] %arg4, i32 2, 2
  br label %bb.many.exit
bb.many.3:
  %arr.3 = insertvalue [5 x i32] %arg4, i32 3, 3
  br label %bb.many.exit
bb.many.4:
  %arr.4 = insertvalue [5 x i32] %arg4, i32 4, 4
  br label %bb.many.exit
bb.many.exit:
  %arr.phi = phi [5 x i32] [ %arr.0, %bb.many.0 ], [ %arr.1, %bb.many.1 ], [ %arr.2, %bb.many.2 ], [ %arr.3, %bb.many.3 ], [ %arr.4, %bb.many.4 ]
; CHECK: (  %arr.phi = phi {{.*}}): (Constant: 0x0 | Dynamic (argument): [5 x i32] %arg4 (offset 0)); (Constant: 0x1 | Dynamic (argument): [5 x i32] %arg4 (offset 4)); (Constant: 0x2 | Dynamic (argument): [5 x i32] %arg4 (offset 8)); (Constant: 0x3 | Dynamic (argument): [5 x i32] %arg4 (offset 12)); (Constant: 0x4 | Dynamic (argument): [5 x i32] %arg4 (offset 16))
  call void @analyze([5 x i32] %arr.phi)
  br label %exit
exit:
  ret void
}

; This is a regression test against an earlier problem with the order in which we analyze
; values. We need to processed operands before processing an instruction itself, i.e. in an topological order.
; If there are cycles, we need to give up on some dependencies (supposedly only back dependencies to phi nodes).
define void @testProcessOrder(i32 %arg1, i1 %cond) {
; CHECK-LABEL: testProcessOrder
; This fails with DFS order: We push a and b to the stack when checking c.
; Then we process b, and see that a is already on the stack, so we don't push a to the stack again.
; After having processed arg1, b is on top of the stack, so we pop it and analyze it, but a is still unknown.
  %a = select i1 %cond, i32 %arg1, i32 7
  %b = select i1 %cond, i32 %a, i32 7
  %c = select i1 %cond, i32 %a, i32 %b
; CHECK: (  %c = select {{.*}}): (Constant: 0x7 | Dynamic (argument): i32 %arg1 (offset 0))
  call void @analyze(i32 %c)
  ret void
}

; For unsupported instructions (e.g. add), we try to use computeKnownBits as last fallback.
; This allows to detect some simple cases as well.
define void @testDynamicComputeKnownBits(i32 %arg1, i1 %cond) {
; CHECK-LABEL: testDynamicComputeKnownBits
  %add = add i32 1, 2
; CHECK: (  %add = add {{.*}}): Constant: 0x3
  call void @analyze(i32 %add)

; computeKnownBits only supports integers:
; CHECK: (  %fadd = fadd {{.*}}): Dynamic
  %fadd = fadd float 1.0, 2.0
  call void @analyze(float %fadd)
  ret void
}
