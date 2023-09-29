/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- PassRegistry.inc - Pass definitions --------------------------------===//

#ifndef CONT_PASS
#define CONT_PASS(NAME, CREATE_PASS)
#endif
#ifndef CONT_LOOP_PASS
#define CONT_LOOP_PASS CONT_PASS
#endif
#ifndef CONT_FUNCTION_PASS
#define CONT_FUNCTION_PASS CONT_PASS
#endif
#ifndef CONT_MODULE_PASS
#define CONT_MODULE_PASS CONT_PASS
#endif
#ifndef CONT_CGSCC_PASS
#define CONT_CGSCC_PASS CONT_PASS
#endif
#ifndef CONT_MODULE_ANALYSIS
#define CONT_MODULE_ANALYSIS(NAME, CREATE_PASS)
#endif

CONT_MODULE_ANALYSIS("dialect-context-analysis",
                     DialectContextAnalysis(NeedDialectContext))

CONT_MODULE_PASS("legacy-cleanup-continuations", LegacyCleanupContinuationsPass())
CONT_MODULE_PASS("cleanup-continuations", CleanupContinuationsPass())
CONT_MODULE_PASS("lower-raytracing-pipeline", LowerRaytracingPipelinePass())
CONT_MODULE_PASS("dxil-cont-intrinsic-prepare", DXILContIntrinsicPreparePass())
CONT_MODULE_PASS("dxil-cont-lgc-rt-op-converter", DXILContLgcRtOpConverterPass())
CONT_MODULE_PASS("dxil-cont-post-process", DXILContPostProcessPass())
CONT_MODULE_PASS("dxil-cont-post-hook", DXILContPostHookPass())
CONT_MODULE_PASS("dxil-cont-pre-coroutine", DXILContPreCoroutinePass())
CONT_MODULE_PASS("dxil-cont-pre-hook", DXILContPreHookPass())
CONT_MODULE_PASS("lower-await", LowerAwaitPass())
CONT_MODULE_PASS("register-buffer", RegisterBufferPass())
CONT_MODULE_PASS("remove-types-metadata", RemoveTypesMetadataPass())
CONT_MODULE_PASS("save-continuation-state", SaveContinuationStatePass())

CONT_CGSCC_PASS("dxil-coro-split", DXILCoroSplitPass())
CONT_CGSCC_PASS("lgc-coro-split", LgcCoroSplitPass())

#undef CONT_PASS
#undef CONT_LOOP_PASS
#undef CONT_FUNCTION_PASS
#undef CONT_MODULE_PASS
#undef CONT_MODULE_ANALYSIS
#undef CONT_CGSCC_PASS
