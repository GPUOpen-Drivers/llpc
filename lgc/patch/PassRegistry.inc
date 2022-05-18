/*
************************************************************************************************************************
*
*  Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
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
#if LLPC_BUILD_NAVI31
LLPC_PASS("lgc-patch-image-op-collect", PatchImageOpCollect())
#endif

#undef LLPC_PASS
