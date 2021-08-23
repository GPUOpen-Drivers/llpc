; Test that,
;   a.the partial pipeline cache works as expected.
;     If the partial pipeline cache is enabled the new pipeline will first check and use the cached data instead of compiling.
;   b.rodata section is merged correctly.
;     If the .rodata section comes from a cached partial pipeline, there will be a .cached string appended to the original section name.
; The test sequence is,
;   a.	Build 3 pipelines: P1(Vs1, Fs1), P2(Vs1, Fs2), P3(Vs2, Fs1).
;   b.	Give all 3 pipelines to amdllpc with shader cache enabled, and the stage access will be,
;           miss, miss, hit, miss, miss, hit
; BEGIN_SHADERTEST
; RUN: amdllpc -spvgen-dir=%spvgendir% -v -shader-cache-mode=1   \
; RUN:      %S/pipelines/PipelineVsFs_ConstantData_Vs1Fs1.pipe   \
; RUN:      %S/pipelines/PipelineVsFs_ConstantData_Vs1Fs2.pipe   \
; RUN:      %S/pipelines/PipelineVsFs_ConstantData_Vs2Fs1.pipe   \
; RUN: | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST:       Non fragment shader cache miss. 
; SHADERTEST-NEXT:  Fragment shader cache miss.
; SHADERTEST:       Non fragment shader cache hit. 
; SHADERTEST-NEXT:  Fragment shader cache miss.
; SHADERTEST:       Non fragment shader cache miss. 
; SHADERTEST-NEXT:  Fragment shader cache hit.
; SHADERTEST-NOT:   shader cache {{miss|hit}}.
; SHADERTEST-LABEL: .rodata.cached
; SHADERTEST:       AMDLLPC SUCCESS
; END_SHADERTEST
