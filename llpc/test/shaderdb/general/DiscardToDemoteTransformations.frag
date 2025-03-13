/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

// Check that amdllpc enables discard-to-demote transforms automatically and that they
// can be disabled on demand. This should affect the generated code and cache hash.

// RUN: amdllpc %gfxip --v %s \
// RUN:   --amdgpu-conditional-discard-transformations=0 \
// RUN:   | tee %t.disabled | FileCheck %s --check-prefix=DISABLED
//
// DISABLED-LABEL: {{^}}SPIR-V disassembly
// DISABLED:       {{^}} {{OpKill|OpTerminateInvocation}}
// DISABLED:             OpImageSampleImplicitLod
// DISABLED-LABEL: {{^}}// LLPC FE lowering results
// DISABLED:       call void (...) @lgc.create.kill()
// DISABLED-LABEL: {{^}}// LLPC LGC lowering results
// DISABLED:       call void @llvm.amdgcn.kill(i1 false)
// DISABLED-LABEL: {{^}}// LLPC final ELF info
// DISABLED:       _amdgpu_ps_main:
// DISABLED:       s_wqm_b64 exec, exec
// DISABLED-NOT:   s_wqm_b64
// DISABLED-LABEL: {{^}}===== AMDLLPC SUCCESS =====

// RUN: amdllpc %gfxip --v %s \
// RUN:   | tee %t.enabled | FileCheck %s --check-prefix=ENABLED
//
// ENABLED-LABEL: {{^}}SPIR-V disassembly
// ENABLED:       {{^}} {{OpKill|OpTerminateInvocation}}
// ENABLED:             OpImageSampleImplicitLod
// ENABLED-LABEL: {{^}}// LLPC FE lowering results
// ENABLED:       call void (...) @lgc.create.kill()
// ENABLED-LABEL: {{^}}// LLPC LGC lowering results
// ENABLED:       call void @llvm.amdgcn.kill(i1 false)
// ENABLED-LABEL: {{^}}// LLPC final ELF info
// ENABLED:       _amdgpu_ps_main:
// ENABLED:       s_wqm_b64 exec, exec
// ENABLED:       s_wqm_b64 [[SGPRS:s\[[0-9]+:[0-9]+\]]], s[{{.*}}]
// ENABLED:       s_and_b64 exec, exec, [[SGPRS]]
// ENABLED-LABEL: {{^}}===== AMDLLPC SUCCESS =====

// Check that both compilations produced in different PIPE and FS hashes.
// The hashes in the Elf note are currently expected to be the same.
// RUN: cat %t.disabled %t.enabled | FileCheck %s --match-full-lines --check-prefix=HASH
// HASH:      // LLPC calculated hash results (graphics pipeline)
// HASH:      PIPE : 0x[[#%.16X,DIS_PIPE:]]
// HASH-NEXT: FS   : 0x[[#%.16X,DIS_FS:]]
// HASH:      .xgl_cache_info: {
// HASH-NEXT: .128_bit_cache_hash: [ 0x[[#%.16X,DIS_CACHE_1:]] 0x[[#%.16X,DIS_CACHE_2:]] ]
// HASH:      ===== AMDLLPC SUCCESS =====
//
// HASH:      // LLPC calculated hash results (graphics pipeline)
// HASH-NOT:  PIPE : 0x[[#DIS_PIPE]]
// HASH:      PIPE : 0x[[#%.16X,EN_PIPE:]]
// HASH-NOT:  FS   : 0x[[#DIS_FS]]
// HASH-NEXT: FS   : 0x[[#%.16X,EN_FS:]]
// HASH:      .xgl_cache_info: {
// HASH-NEXT: .128_bit_cache_hash: [ 0x[[#DIS_CACHE_1]] 0x[[#DIS_CACHE_2]] ]
// HASH:      ===== AMDLLPC SUCCESS =====

#version 450

layout (location = 0) in vec2 texCoordIn;
layout (location = 1) in flat int discardPixel;

layout (binding = 0) uniform sampler2D image;

layout (location = 0) out vec4 fragColor;

void main() {
  if (discardPixel != 0)
    discard;
  fragColor = texture(image, texCoordIn);
}
