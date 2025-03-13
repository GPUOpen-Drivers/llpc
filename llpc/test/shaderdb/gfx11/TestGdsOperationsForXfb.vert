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

// Test to check GDS operations that are required to support GFX11 transform feedback. Also, check
// ds_ordered_count is followed by s_waitcnt lgkmcnt(0), which is required by HW on GFX11.

// RUN: amdllpc %gfxip %s -v | FileCheck -check-prefix=SHADERTEST %s

// SHADERTEST-LABEL: {{^// LLPC}} final pipeline module info
// SHADERTEST: .prepareXfb:
// SHADERTEST: [[orderedWaveId0:%.*]] = inttoptr i32 %orderedWaveId to ptr addrspace(2)
// SHADERTEST-NEXT: call i32 @llvm.amdgcn.ds.ordered.add(ptr addrspace(2) [[orderedWaveId0]], i32 0, i32 0, i32 0, i1 false, i32 16777216, i1 false, i1 false)
// SHADERTEST-NEXT: fence syncscope("workgroup") release
// SHADERTEST: call i32 @llvm.amdgcn.ds.add.gs.reg.rtn.i32(i32 %{{.*}}, i32 0)
// SHADERTEST-NEXT: fence syncscope("workgroup") release
// SHADERTEST-NEXT: call i32 @llvm.amdgcn.ds.add.gs.reg.rtn.i32(i32 0, i32 4)
// SHADERTEST-NEXT: fence syncscope("workgroup") release
// SHADERTEST: [[orderedWaveId1:%.*]] = inttoptr i32 %orderedWaveId to ptr addrspace(2)
// SHADERTEST-NEXT: call i32 @llvm.amdgcn.ds.ordered.add(ptr addrspace(2) [[orderedWaveId1]], i32 %{{.*}}, i32 0, i32 0, i1 false, i32 16777217, i1 true, i1 true)
// SHADERTEST-NEXT: fence syncscope("workgroup") release
// SHADERTEST: call i32 @llvm.amdgcn.ds.add.gs.reg.rtn.i32(i32 %primCountInSubgroup, i32 32)
// SHADERTEST-NEXT: fence syncscope("workgroup") release
// SHADERTEST-NEXT: call i32 @llvm.amdgcn.ds.add.gs.reg.rtn.i32(i32 %77, i32 36)
// SHADERTEST-NEXT: fence syncscope("workgroup") release

// SHADERTEST-LABEL: {{^// LLPC}} final ELF info
// SHADERTEST: ds_ordered_count {{v[0-9]*}}, {{v[0-9]*}} gds
// SHADERTEST: s_waitcnt lgkmcnt(0)
// SHADERTEST: ds_add_gs_reg_rtn v[{{[0-9]*}}:{{[0-9]*}}], {{v[0-9]*}} gds
// SHADERTEST: s_waitcnt lgkmcnt(0)
// SHADERTEST: ds_add_gs_reg_rtn v[{{[0-9]*}}:{{[0-9]*}}], {{v[0-9]*}} offset:4 gds
// SHADERTEST: s_waitcnt lgkmcnt(0)
// SHADERTEST: ds_ordered_count {{v[0-9]*}}, {{v[0-9]*}} offset:772 gds
// SHADERTEST:  s_waitcnt expcnt(0) lgkmcnt(0)
// SHADERTEST: ds_add_gs_reg_rtn v[{{[0-9]*}}:{{[0-9]*}}], {{v[0-9]*}} offset:32 gds
// SHADERTEST: s_waitcnt lgkmcnt(0)
// SHADERTEST: ds_add_gs_reg_rtn v[{{[0-9]*}}:{{[0-9]*}}], {{v[0-9]*}} offset:36 gds
// SHADERTEST: s_waitcnt lgkmcnt(0)

#version 450 core

layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 data0;
layout(location = 1, xfb_buffer = 1, xfb_offset = 0, xfb_stride = 16) out vec4 data1;

void main() {
  data0 = vec4(0.0);
  data1 = vec4(1.0);
}
