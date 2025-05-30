/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  PassRegistry.inc
 * @brief LLPC header file: used as the registry of LGC lowering passes
 ***********************************************************************************************************************
 */

#ifndef LLPC_PASS
#define LLPC_PASS(name, class)
#endif
#ifndef LLPC_LOOP_PASS
#define LLPC_LOOP_PASS LLPC_PASS
#endif
#ifndef LLPC_FUNCTION_PASS
#define LLPC_FUNCTION_PASS LLPC_PASS
#endif
#ifndef LLPC_MODULE_PASS
#define LLPC_MODULE_PASS LLPC_PASS
#endif
#ifndef LLPC_MODULE_PASS_WITH_PARSER
#define LLPC_MODULE_PASS_WITH_PARSER LLPC_MODULE_PASS
#endif

#ifndef LLPC_MODULE_ANALYSIS
#define LLPC_MODULE_ANALYSIS(name, class)
#endif

LLPC_MODULE_ANALYSIS("lgc-pipeline-state", PipelineStateWrapper)
LLPC_MODULE_PASS("print<lgc-pipeline-state>", PipelineStatePrinter)
LLPC_MODULE_PASS("lgc-pipeline-state-recorder", PipelineStateRecorder)

LLPC_MODULE_PASS("lgc-builder-replayer", BuilderReplayer)
LLPC_MODULE_PASS("lgc-continufy", Continufy)
LLPC_MODULE_PASS("lgc-initialize-undef-inputs", InitializeUndefInputs)
LLPC_MODULE_PASS("lgc-collect-resource-usage", CollectResourceUsage)
LLPC_MODULE_PASS("lgc-initialize-workgroup-memory", InitializeWorkgroupMemory)
LLPC_MODULE_PASS("lgc-lower-image-derivatives", LowerImageDerivatives)
LLPC_MODULE_PASS("lgc-lower-in-out", LowerInOut)
LLPC_FUNCTION_PASS("lgc-lower-invariant-loads", LowerInvariantLoads)
LLPC_MODULE_PASS("lgc-set-up-target-features", SetUpTargetFeatures)
LLPC_MODULE_PASS("lgc-generate-copy-shader", GenerateCopyShader)
LLPC_MODULE_PASS("lgc-prepare-pipeline-abi", PreparePipelineAbi)
LLPC_FUNCTION_PASS("lgc-lower-read-first-lane", LowerReadFirstLane)
LLPC_MODULE_PASS("lgc-include-llvm-ir", IncludeLlvmIr)
LLPC_FUNCTION_PASS("lgc-peephole-optimization", PeepholeOptimization)
LLPC_MODULE_PASS("lgc-lower-subgroup-ops", LowerSubgroupOps)
LLPC_MODULE_PASS("lgc-mutate-entry-point", MutateEntryPoint)
LLPC_MODULE_PASS("lgc-check-shader-cache", CheckShaderCache)
LLPC_FUNCTION_PASS("lgc-add-loop-metadata", AddLoopMetadata)
LLPC_FUNCTION_PASS("lgc-structurize-buffers", StructurizeBuffers)
LLPC_FUNCTION_PASS("lgc-lower-buffer-operations", LowerBufferOperations)
LLPC_MODULE_PASS("lgc-apply-workarounds", ApplyWorkarounds)
LLPC_FUNCTION_PASS("lgc-scalarizer-loads", ScalarizeLoads)
LLPC_FUNCTION_PASS("lgc-lower-mul-dx9-zero", LowerMulDx9Zero)
LLPC_MODULE_PASS("lgc-generate-null-frag-shader", GenerateNullFragmentShader)
LLPC_MODULE_PASS("lgc-passthrough-hull-shader", PassthroughHullShader)
LLPC_MODULE_PASS("lgc-collect-image-operations", CollectImageOperations)
LLPC_FUNCTION_PASS("lgc-add-buffer-operations-metadata", AddBufferOperationMetadata)
LLPC_MODULE_PASS("lgc-vertex-fetch", LowerVertexFetch)
LLPC_MODULE_PASS("lgc-frag-color-export", LowerFragmentColorExport)
LLPC_MODULE_PASS("lgc-lower-debug-printf", LowerDebugPrintf)
LLPC_MODULE_PASS("lgc-lower-desc", LowerDesc)

LLPC_MODULE_PASS("lgc-workaround-ds-subdword-write", WorkaroundDsSubdwordWrite)

LLPC_FUNCTION_PASS("lgc-combine-cooperative-matrix", CombineCooperativeMatrix)
LLPC_MODULE_PASS("lgc-lower-cooperative-matrix", LowerCooperativeMatrix)
LLPC_MODULE_PASS("lgc-lower-gpurt", LowerGpuRt)
LLPC_MODULE_PASS("lgc-lower-rayquery", LowerRayQueryWrapper)
LLPC_MODULE_PASS("lgc-emit-shader-hash-token", EmitShaderHashToken)

#undef LLPC_PASS
#undef LLPC_LOOP_PASS
#undef LLPC_FUNCTION_PASS
#undef LLPC_MODULE_PASS
#undef LLPC_MODULE_PASS_WITH_PARSER
#undef LLPC_MODULE_ANALYSIS
