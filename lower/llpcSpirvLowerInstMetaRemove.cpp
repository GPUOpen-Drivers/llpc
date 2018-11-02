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
 * @file  llpcSpirvLowerInstMetaRemove.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerInstMetaRemove.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-inst-meta-remove"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcSpirvLowerInstMetaRemove.h"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerInstMetaRemove::ID = 0;

// =====================================================================================================================
SpirvLowerInstMetaRemove::SpirvLowerInstMetaRemove()
    :
    SpirvLower(ID),
    m_changed(false)
{
    initializeSpirvLowerInstMetaRemovePass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerInstMetaRemove::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Inst-Meta-Remove\n");

    SpirvLower::Init(&module);
    m_changed = false;

    visit(m_pModule);

    LLPC_VERIFY_MODULE_FOR_PASS(module);

    return m_changed;
}

// =====================================================================================================================
// Visits "call" instruction.
void SpirvLowerInstMetaRemove::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto mangledName = pCallee->getName();
    LLPC_ASSERT(strlen(gSPIRVMD::NonUniform) == 16);
    const std::string NonUniformPrefix = std::string("_Z16") + std::string(gSPIRVMD::NonUniform);
    if (mangledName.startswith(NonUniformPrefix))
    {
        callInst.dropAllReferences();
        callInst.eraseFromParent();
        m_changed = true;
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for removing instruction metadata.
INITIALIZE_PASS(SpirvLowerInstMetaRemove, "Spirv-lower-inst-meta-remove",
                "Lower SPIR-V instruction metadata by removing those targeted", false, false)
