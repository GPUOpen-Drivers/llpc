/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchLlvmIrInclusion.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchLlvmIrInclusion.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-llvm-ir-inclusion"

#include "llvm/IR/Constants.h"

#include "llpcPatchLlvmIrInclusion.h"
#include "palPipelineAbi.h"
#include "g_palPipelineAbiMetadata.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchLlvmIrInclusion::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations of including LLVM IR as a separate section in the ELF.
ModulePass* CreatePatchLlvmIrInclusion()
{
    return new PatchLlvmIrInclusion();
}

// =====================================================================================================================
PatchLlvmIrInclusion::PatchLlvmIrInclusion()
    :
    Patch(ID)
{
    initializePatchLlvmIrInclusionPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this patching pass on the specified LLVM module.
//
// This pass includes LLVM IR as a separate section in the ELF binary by inserting a new global variable with explicit
// section.
bool PatchLlvmIrInclusion::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    Patch::Init(&module);

    std::string moduleStr;
    raw_string_ostream llvmIr(moduleStr);
    llvmIr << *m_pModule;
    llvmIr.flush();

    auto pGlobalTy = ArrayType::get(Type::getInt8Ty(*m_pContext), moduleStr.size());
    auto pInitializer = ConstantDataArray::getString(m_pModule->getContext(), moduleStr, false);
    auto pGlobal = new GlobalVariable(*m_pModule,
                                      pGlobalTy,
                                      true,
                                      GlobalValue::ExternalLinkage,
                                      pInitializer,
                                      "llvmir",
                                      nullptr,
                                      GlobalValue::NotThreadLocal,
                                      false);
    LLPC_ASSERT(pGlobal != nullptr);

    std::string namePrefix = Util::Abi::AmdGpuCommentName;
    pGlobal->setSection(namePrefix + "llvmir");

    return true;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations of including LLVM IR as a separate section in the ELF binary.
INITIALIZE_PASS(PatchLlvmIrInclusion, DEBUG_TYPE,
                "Include LLVM IR as a separate section in the ELF binary", false, false)
