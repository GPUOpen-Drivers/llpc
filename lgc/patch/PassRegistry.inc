/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PassRegistry.inc
 * @brief LLPC header file: used as the registry of LLPC patching passes
 ***********************************************************************************************************************
 */

LLPC_PASS("lgc-patch-resource-collect", PatchResourceCollect())
LLPC_PASS("lgc-patch-initialize-workgroup-memory", PatchInitializeWorkgroupMemory())
LLPC_PASS("lgc-patch-in-out-import-export", PatchInOutImportExport())
LLPC_PASS("lgc-patch-setup-target-features", PatchSetupTargetFeatures())
LLPC_PASS("lgc-patch-copy-shader", PatchCopyShader())
LLPC_PASS("lgc-patch-prepare-pipeline-abi", PatchPreparePipelineAbi())
LLPC_PASS("lgc-patch-read-first-lane", PatchReadFirstLane())
LLPC_PASS("lgc-patch-llvm-ir-inclusion", PatchLlvmIrInclusion())
LLPC_PASS("lgc-patch-wave-size-adjust", PatchWaveSizeAdjust())
LLPC_PASS("lgc-patch-peephole-opt", PatchPeepholeOpt())
LLPC_PASS("lgc-patch-entry-point-mutate", PatchEntryPointMutate())
LLPC_PASS("lgc-patch-check-shader-cache", PatchCheckShaderCache())
LLPC_PASS("lgc-patch-loop-metadata", PatchLoopMetadata())
LLPC_PASS("lgc-patch-buffer-op", PatchBufferOp())
LLPC_PASS("lgc-patch-workarounds", PatchWorkarounds())
LLPC_PASS("lgc-patch-load-scalarizer", PatchLoadScalarizer())
LLPC_PASS("lgc-patch-null-frag-shader", PatchNullFragShader())

#undef LLPC_PASS
