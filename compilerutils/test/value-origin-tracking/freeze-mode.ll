; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-freeze-mode=0 | FileCheck %s --check-prefix=DYNAMIC
; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-freeze-mode=1 | FileCheck %s --check-prefix=FORWARD

declare void @analyze(...)

define void @testSimpleFreeze() {
; CHECK-LABEL: testSimpleFreeze
  %freeze = freeze i32 poison
; DYNAMIC: %freeze = {{.*}}: Dynamic
; FORWARD: %freeze = {{.*}}: UndefOrPoison
  call void @analyze(i32 %freeze)
  ret void
}

define void @testSelectMultipleFreezes(i1 %cond) {
; CHECK-LABEL: testSelectMultipleFreezes
  %freeze.0 = freeze i32 poison
  %freeze.1 = freeze i32 poison
  %merged = select i1 %cond, i32 %freeze.0, i32 %freeze.1
; DYNAMIC: %merged = {{.*}}: Dynamic
; FORWARD: %merged = {{.*}}: UndefOrPoison
  call void @analyze(i32 %merged)
  ret void
}

define void @testSelectFreezeWithConstant(i1 %cond) {
; CHECK-LABEL: testSelectFreezeWithConstant
  %freeze.0 = freeze i32 poison
  %freeze.1 = freeze i32 poison
  %merged.with.0 = select i1 %cond, i32 %freeze.0, i32 0
  %merged.with.1 = select i1 %cond, i32 %freeze.1, i32 1
; DYNAMIC: %merged.with.0 = {{.*}}: (Constant: 0x0 | Dynamic: {{.*}})
; FORWARD: %merged.with.0 = {{.*}}: (UndefOrPoison | Constant: 0x0)
  call void @analyze(i32 %merged.with.0)
; DYNAMIC: %merged.with.1 = {{.*}}: (Constant: 0x1 | Dynamic: {{.*}})
; FORWARD: %merged.with.1 = {{.*}}: (UndefOrPoison | Constant: 0x1)
  call void @analyze(i32 %merged.with.1)
  ret void
}

define void @testFreezeNonPoison(i1 %cond, i32 %arg) {
; CHECK-LABEL: testFreezeNonPoison
  %add = add i32 1, 1
; DYNAMIC: %add = {{.*}}: Constant: 0x2
; FORWARD: %add = {{.*}}: Constant: 0x2
  call void @analyze(i32 %add)
  %frozen.add = freeze i32 %add
; DYNAMIC: %frozen.add = {{.*}}: Constant: 0x2
; FORWARD: %frozen.add = {{.*}}: Constant: 0x2
  call void @analyze(i32 %frozen.add)

  %arg.or.constant = select i1 %cond, i32 15, i32 %arg
; DYNAMIC: %arg.or.constant = {{.*}}: (Constant: 0xf | Dynamic (argument): {{.*}})
; FORWARD: %arg.or.constant = {{.*}}: (Constant: 0xf | Dynamic (argument): {{.*}})
  call void @analyze(i32 %arg.or.constant)
  %arg.or.constant.frozen = freeze i32 %arg.or.constant
; DYNAMIC: %arg.or.constant.frozen = {{.*}}: (Constant: 0xf | Dynamic (argument): {{.*}})
; FORWARD: %arg.or.constant.frozen = {{.*}}: (Constant: 0xf | Dynamic (argument): {{.*}})
  call void @analyze(i32 %arg.or.constant.frozen)

  %arg.or.poison = select i1 %cond, i32 poison, i32 %arg
; DYNAMIC: %arg.or.poison = {{.*}}: (UndefOrPoison | Dynamic (argument): {{.*}})
; FORWARD: %arg.or.poison = {{.*}}: (UndefOrPoison | Dynamic (argument): {{.*}})
  call void @analyze(i32 %arg.or.poison)
  %arg.or.poison.frozen = freeze i32 %arg.or.poison
; DYNAMIC: %arg.or.poison.frozen = {{.*}}: Dynamic
; FORWARD: %arg.or.poison.frozen = {{.*}}: (UndefOrPoison | Dynamic (argument): {{.*}})
  call void @analyze(i32 %arg.or.poison.frozen)
  ret void
}
