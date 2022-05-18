/*
************************************************************************************************************************
*
*  Copyright (C) 2021-2022 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PassRegistry.inc
 * @brief LLPC header file: used as the registry of LLPC lowering passes
 ***********************************************************************************************************************
 */

LLPC_PASS("llpc-spirv-lower-access-chain", SpirvLowerAccessChain())
LLPC_PASS("llpc-spirv-lower-const-immediate-store", SpirvLowerConstImmediateStore())
LLPC_PASS("llpc-spirv-lower-inst-meta-remove", SpirvLowerInstMetaRemove())
LLPC_PASS("llpc-spirv-lower-terminator", SpirvLowerTerminator())
LLPC_PASS("llpc-spirv-lower-translator", SpirvLowerTranslator())
LLPC_PASS("llpc-spirv-lower-global", SpirvLowerGlobal())
LLPC_PASS("llpc-spirv-lower-math-const-folding", SpirvLowerMathConstFolding())
LLPC_PASS("llpc-spirv-lower-math-float-op", SpirvLowerMathFloatOp())
LLPC_PASS("llpc-spirv-lower-memory-op", SpirvLowerMemoryOp())
#if VKI_RAY_TRACING
LLPC_PASS("llpc-spirv-lower-ray-tracing-intrinsics", SpirvLowerRayTracingIntrinsics())
LLPC_PASS("llpc-spirv-lower-ray-query", SpirvLowerRayQuery())
LLPC_PASS("llpc-spirv-lower-ray-tracing", SpirvLowerRayTracing())
LLPC_PASS("llpc-spirv-lower-ray-query-post-inline", SpirvLowerRayQueryPostInline())
LLPC_PASS("llpc-spirv-lower-ray-tracing-builtin", SpirvLowerRayTracingBuiltIn())
#endif

#undef LLPC_PASS
