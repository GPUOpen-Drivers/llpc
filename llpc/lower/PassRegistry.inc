/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC header file: used as the registry of LLPC lowering passes
 ***********************************************************************************************************************
 */

LLPC_PASS("llpc-spirv-lower-access-chain", SpirvLowerAccessChain)
LLPC_PASS("llpc-spirv-lower-const-immediate-store", SpirvLowerConstImmediateStore)
LLPC_PASS("llpc-spirv-lower-inst-meta-remove", SpirvLowerInstMetaRemove)
LLPC_PASS("llpc-spirv-lower-terminator", SpirvLowerTerminator)
LLPC_PASS("llpc-spirv-lower-translator", SpirvLowerTranslator)
LLPC_PASS("llpc-spirv-lower-global", SpirvLowerGlobal)
LLPC_PASS("llpc-spirv-lower-math-const-folding", SpirvLowerMathConstFolding)
LLPC_PASS("llpc-spirv-lower-math-float-op", SpirvLowerMathFloatOp)
LLPC_PASS("llpc-spirv-lower-memory-op", SpirvLowerMemoryOp)
#if VKI_RAY_TRACING
LLPC_PASS("llpc-spirv-lower-ray-tracing-intrinsics", SpirvLowerRayTracingIntrinsics)
LLPC_PASS("llpc-spirv-lower-ray-query", SpirvLowerRayQuery)
LLPC_PASS("llpc-spirv-lower-ray-tracing", SpirvLowerRayTracing)
LLPC_PASS("llpc-spirv-lower-ray-query-post-inline", SpirvLowerRayQueryPostInline)
LLPC_PASS("llpc-spirv-lower-ray-tracing-builtin", SpirvLowerRayTracingBuiltIn)
#endif

#undef LLPC_PASS
