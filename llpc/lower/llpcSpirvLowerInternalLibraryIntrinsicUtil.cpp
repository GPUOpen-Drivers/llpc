/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerInternalLibraryIntrinsicUtil.cpp
 * @brief LLPC source file: utilities for lowering common internal library intrinsics.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "llpcContext.h"
#include "lgc/Builder.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace llvm;
using namespace lgc;

namespace Llpc {

// =====================================================================================================================
// Create function to get lane index (subgroup local invocation ID)
//
// @param func : The function to process
// @param builder : The IR builder
static void createLaneIndex(Function *func, Builder *builder) {
  builder->CreateRet(builder->CreateReadBuiltInInput(static_cast<lgc::BuiltInKind>(BuiltInSubgroupLocalInvocationId),
                                                     {}, nullptr, nullptr, ""));
}

// =====================================================================================================================
// Create function to get lane count (wave size)
//
// @param func : The function to process
// @param builder : The IR builder
static void createLaneCount(Function *func, Builder *builder) {
  builder->CreateRet(builder->CreateGetWaveSize());
}

// =====================================================================================================================
// Create function to generate s_sethalt intrinsic
//
// @param func : The function to process
// @param builder : The IR builder
static void createHalt(Function *func, Builder *builder) {
  builder->CreateIntrinsic(Intrinsic::amdgcn_s_sethalt, {}, {builder->getInt32(1)});
  builder->CreateRetVoid();
}

// =====================================================================================================================
// Initialize library function pointer table
InternalLibraryIntrinsicUtil::LibraryFunctionTable::LibraryFunctionTable() {
  m_libFuncPtrs["AmdExtLaneIndex"] = &createLaneIndex;
  m_libFuncPtrs["AmdExtLaneCount"] = &createLaneCount;
  m_libFuncPtrs["AmdExtHalt"] = &createHalt;
}

} // namespace Llpc
