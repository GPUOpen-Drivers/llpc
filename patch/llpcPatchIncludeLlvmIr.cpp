/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchIncludeLlvmIr.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchIncludeLlvmIr.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-include-llvm-ir"

#include "llvm/IR/Constants.h"

#include "llpcContext.h"
#include "llpcPatchIncludeLlvmIr.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchIncludeLlvmIr::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations of including llvm-ir as a separate section in the ELF.
ModulePass* CreatePatchIncludeLlvmIr()
{
    return new PatchIncludeLlvmIr();
}

// =====================================================================================================================
PatchIncludeLlvmIr::PatchIncludeLlvmIr()
    :
    Patch(ID)
{
    initializePatchIncludeLlvmIrPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this patching pass on the specified LLVM module.
//
// This pass includes llvm-ir as a separate section in the ELF binary by inserting a new global variable with explicit
// section.
bool PatchIncludeLlvmIr::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    Patch::Init(&module);

    std::string moduleStr;
    raw_string_ostream llvmIr(moduleStr);
    llvmIr << *m_pModule;
    llvmIr.flush();

    auto pGlobalTy = ArrayType::get(m_pContext->Int8Ty(), moduleStr.size());
    auto pInitializer = ConstantDataArray::getString(m_pModule->getContext(), moduleStr, false);
    auto pGlobal = new GlobalVariable(*m_pModule,
                                      pGlobalTy,
                                      true,
                                      GlobalValue::ExternalLinkage,
                                      pInitializer,
                                      "llvm_ir",
                                      nullptr,
                                      GlobalValue::NotThreadLocal,
                                      false);
    LLPC_ASSERT(pGlobal != nullptr);
    pGlobal->setSection(".AMDGPU.metadata.llvm_ir");

    return true;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations of including llvm-ir as a separate section in the ELF binary.
INITIALIZE_PASS(PatchIncludeLlvmIr, DEBUG_TYPE,
                "Include llvm-ir as a separate section in the ELF binary", false, false)
