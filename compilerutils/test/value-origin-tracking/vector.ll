; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-bytes-per-slice=1 | FileCheck %s --check-prefix=CHECK1
; RUN: opt -passes="value-origin-tracking-test" -S %s -value-origin-tracking-test-bytes-per-slice=4 | FileCheck %s --check-prefix=CHECK4
;
; Test vector ops on types that aren't byte-aligned (i1) and overaligned (i16)
target datalayout = "i16:32"

declare void @analyze(...)

define void @testi1(i32 %arg) {
; CHECK-LABEL: testi1
  %vec.0 = insertelement <16 x i1> poison, i1 1, i32 0
  %vec.1 = insertelement <16 x i1> %vec.0, i1 0, i32 1
  %vec.2 = insertelement <16 x i1> %vec.1, i1 1, i32 2
  %vec.3 = insertelement <16 x i1> %vec.2, i1 0, i32 3
  %vec.4 = insertelement <16 x i1> %vec.3, i1 1, i32 4
  %vec.5 = insertelement <16 x i1> %vec.4, i1 0, i32 5
  %vec.6 = insertelement <16 x i1> %vec.5, i1 1, i32 6
  %vec.7 = insertelement <16 x i1> %vec.6, i1 0, i32 7
  %vec.8 = insertelement <16 x i1> %vec.7, i1 1, i32 8
  %vec.9 = insertelement <16 x i1> %vec.8, i1 0, i32 9
  %vec.10 = insertelement <16 x i1> %vec.9, i1 1, i32 10
  call void @analyze(<16 x i1> %vec.10)
; CHECK1: (  %vec.10 = insertelement <16 x i1> %vec.9, i1 true, i32 10): Dynamic:   %vec.10 = insertelement <16 x i1> %vec.9, i1 true, i32 10 (offset 0); Dynamic:   %vec.10 = insertelement <16 x i1> %vec.9, i1 true, i32 10 (offset 1)
; CHECK4: (  %vec.10 = insertelement <16 x i1> %vec.9, i1 true, i32 10): Dynamic:   %vec.10 = insertelement <16 x i1> %vec.9, i1 true, i32 10 (offset 0)
  ret void
}

define void @testi1InsertExtract() {
; CHECK-LABEL: testi1Extract
; We don't support sub-byte inserts/extractions yet, as demonstrated in this test
  %vec.1 = bitcast i32 -1 to <32 x i1>
  %extract.1 = extractelement <32 x i1> %vec.1, i32 0
; CHECK: (  %extract.1 = extractelement <32 x i1> %vec.1, i32 0): Dynamic: {{.*}} (offset 0)
  call void @analyze(i1 %extract.1)
  %vec.2 = bitcast i32 0 to <32 x i1>
  %vec.3 = insertelement <32 x i1> %vec.2, i1 1, i32 8
  call void @analyze(<32 x i1> %vec.3)
; CHECK: (  %vec.3 = insertelement <32 x i1> %vec.2, i1 true, i32 8): Dynamic: {{.*}} (offset 0); Dynamic: {{.*}} (offset 1); Dynamic: {{.*}} (offset 2); Dynamic: {{.*}} (offset 3)
  ret void
}

define void @testi16(i32 %arg) {
; CHECK-LABEL: testi16
  %vec.1 = insertelement <4 x i16> poison, i16 -1, i32 0
  %vec.2 = insertelement <4 x i16> %vec.1, i16 0, i32 1
; CHECK1: %vec.2 = {{.*}}: Constant: 0xff; Constant: 0xff; Constant: 0x0; Constant: 0x0; UndefOrPoison; UndefOrPoison; UndefOrPoison; UndefOrPoison
; Sub-slice extract/insert isn't supported:
; CHECK4: %vec.2 = {{.*}}: Dynamic
  call void @analyze(<4 x i16> %vec.2)
; CHECK1: %extract.1 = {{.*}}): Constant: 0xff; Constant: 0xff
; CHECK4: %extract.1 = {{.*}}: Dynamic
  %extract.1 = extractelement <4 x i16> %vec.2, i32 0
  call void @analyze(i16 %extract.1)
; CHECK1: %extract.2 = {{.*}}): Constant: 0x0; Constant: 0x0
; CHECK4: %extract.2 = {{.*}}: Dynamic
  %extract.2 = extractelement <4 x i16> %vec.2, i32 1
  call void @analyze(i16 %extract.2)
  ret void
}

; Regression test for computeKnownBits handling of vectors
define void @testShuffleVector(i32 %arg) {
; CHECK-LABEL: testShuffleVector
  %vec = shufflevector <2 x i32> zeroinitializer, <2 x i32> zeroinitializer, <2 x i32> <i32 0, i32 0>
; CHECK: %vec = shufflevector {{.*}}: Dynamic
  call void @analyze(<2 x i32> %vec)
  ret void
}
