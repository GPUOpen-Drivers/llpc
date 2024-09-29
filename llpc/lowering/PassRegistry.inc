/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC header file: used as the registry of LLPC lowering passes
 ***********************************************************************************************************************
 */

#ifndef LLPC_MODULE_PASS
#define LLPC_MODULE_PASS LLPC_PASS
#endif

LLPC_MODULE_PASS("inline", AlwaysInlinerPass)
LLPC_MODULE_PASS("globaldce", GlobalDCEPass)
LLPC_MODULE_PASS("sroa", SROAPass)
LLPC_MODULE_PASS("globalopt", GlobalOptPass)
LLPC_MODULE_PASS("adce", ADCEPass)
LLPC_MODULE_PASS("instcombine", InstCombinePass)
LLPC_MODULE_PASS("simplifycfg", SimplifyCFGPass)
LLPC_MODULE_PASS("early-cse", EarlyCSEPass)
LLPC_MODULE_PASS("scalar-replacement-of-builtins", ScalarReplacementOfBuiltins)
LLPC_MODULE_PASS("lower-gl-compatibility", LowerGlCompatibility)
LLPC_MODULE_PASS("lower-access-chain", LowerAccessChain)
LLPC_MODULE_PASS("lower-cfg-merges", LowerCfgMerges)
LLPC_MODULE_PASS("lower-const-immediate-store", LowerConstImmediateStore)
LLPC_MODULE_PASS("lower-cooperative-matrix", LowerCooperativeMatrixProxy)
LLPC_MODULE_PASS("lower-inst-meta-remove", LowerInstMetaRemove)
LLPC_MODULE_PASS("lower-terminator", LowerTerminator)
LLPC_MODULE_PASS("lower-globals", LowerGlobals)
LLPC_MODULE_PASS("lower-math-const-folding", LowerMathConstFolding)
LLPC_MODULE_PASS("lower-math-precision", LowerMathPrecision)
LLPC_MODULE_PASS("lower-math-float-op", LowerMathFloatOp)
LLPC_MODULE_PASS("lower-memory-op", LowerMemoryOp)
LLPC_MODULE_PASS("llpc-spirv-lower-ray-tracing", SpirvLowerRayTracing)
LLPC_MODULE_PASS("lower-post-inline", LowerPostInline)

#undef LLPC_PASS
#undef LLPC_MODULE_PASS
