; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-bytes-per-slice=1 | FileCheck %s --check-prefix=CHECK1
; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-bytes-per-slice=4 | FileCheck %s --check-prefix=CHECK4

declare void @analyze(...)

define void @testConstant() {
; CHECK-LABEL: testConstant
; CHECK1: (i32 -5601263): Constant: 0x11; Constant: 0x88; Constant: 0xaa; Constant: 0xff
; CHECK4: (i32 -5601263): Constant: 0xffaa8811
  call void @analyze(i32 u0xffaa8811)
  ret void
}

define void @testArray(i8 %arg) {
; CHECK-LABEL: testArray
  %arr.1 = insertvalue [3 x i8] poison, i8 u0xff, 0
  %arr.2 = insertvalue [3 x i8] %arr.1, i8 poison, 1
  %arr.3 = insertvalue [3 x i8] %arr.2, i8 %arg, 2
; CHECK1: (  %arr.3 = {{.*}}: Constant: 0xff; UndefOrPoison; Dynamic (argument): i8 %arg (offset 0)
; CHECK4: (  %arr.3 = {{.*}}: Dynamic: {{.*}} (offset 0)
  call void @analyze([3 x i8] %arr.3)
  ret void
}

; Check that inserting a value into a range that is not slice-aligned invalidates
; the affected slices, but preserves the other ones.
; We insert the i16 at index 3 into this packed struct, which covers bytes 7 and 8.
; This touches two dwords, so with dword-sized slices the two middle dwords are dynamic.
; Byte-sized slices however nicely deal with it.
%packed.struct = type <{i32,  i16, i8,  i16, i16,  i8,    i32 }>
;         Indices:      0     1    2    3    4     5      6
;         Byte ranges:  0..3  4..5 6..6 7..8 9..10 11..11 12..15
;                interesting value: ----^^^^
define void @testMisalignedInsertExtract() {
; CHECK-LABEL: testMisalignedInsertExtract
; CHECK1: (  %inserted.3 = {{.*}}): Constant: 0xff; Constant: 0xff; Constant: 0xff; Constant: 0xff; Constant: 0x0; Constant: 0x0; Constant: 0x1; Constant: 0xff; Constant: 0xff
; CHECK1-SAME: Constant: 0x0; Constant: 0x0; Constant: 0x0; Constant: 0x0; Constant: 0x0; Constant: 0x0; Constant: 0x0
; CHECK4: (  %inserted.3 = {{.*}}): Constant: 0xffffffff; Dynamic: {{.*}}; Dynamic: {{.*}}; Constant: 0x0
  %inserted.1 = insertvalue %packed.struct zeroinitializer, i32 -1, 0
  %inserted.2 = insertvalue %packed.struct %inserted.1, i8  1, 2
  %inserted.3 = insertvalue %packed.struct %inserted.2, i16 -1, 3
  call void @analyze(%packed.struct %inserted.3)

; CHECK1: (  %extracted = {{.*}}): Constant: 0x0
; CHECK4: (  %extracted = {{.*}}): Dynamic
  %extracted = extractvalue %packed.struct zeroinitializer, 3
  call void @analyze(i16 %extracted)

  ret void
}

; Test that inserting/extracting a value that is slice-aligned but smaller than a slice works correctly
; We insert/extract the i16 at index 1 in this struct:
%packed.struct.1 = type <{i32, i16, i16, i32 }>
;       interesting value: ----^^^^
define void @testAlignedSubSliceInsertExtract() {
; CHECK-LABEL: testAlignedSubSliceInsertExtract
  %inserted.1 = insertvalue %packed.struct.1 zeroinitializer, i32 -1, 0
  %extracted.1 = extractvalue %packed.struct.1 %inserted.1, 1
  %inserted.2 = insertvalue %packed.struct.1 %inserted.1, i16 1, 1
  %extracted.2 = extractvalue %packed.struct.1 %inserted.2, 1

; CHECK1: (  %extracted.1 = {{.*}}): Constant: 0x0; Constant: 0x0
; CHECK4: (  %extracted.1 = {{.*}}): Constant: 0x0
  call void @analyze(i16 %extracted.1)

; CHECK1: (  %extracted.2 = {{.*}}): Constant: 0x1; Constant: 0x0
; We don't support partial insertions, so treat this conservatively:
; CHECK4: (  %extracted.2 = {{.*}}): Dynamic
  call void @analyze(i16 %extracted.2)

  ret void
}
