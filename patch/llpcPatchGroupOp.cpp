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
* @file  llpcPatchGroupOp.cpp
* @brief LLPC source file: contains implementation of class Llpc::PatchGroupOp.
***********************************************************************************************************************
*/
#define DEBUG_TYPE "llpc-patch-group-op"

#include <vector>
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcInternal.h"
#include "llpcPatchGroupOp.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchGroupOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for group operations.
ModulePass* CreatePatchGroupOp()
{
    return new PatchGroupOp();
}

// =====================================================================================================================
PatchGroupOp::PatchGroupOp()
    :
    Patch(ID)
{
    initializePatchGroupOpPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchGroupOp::runOnModule(
    llvm::Module & module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Group-Op\n");

    Patch::Init(&module);
    m_changed = false;

    // Invoke handling of "callInst" instruction
    visit(m_pModule);

    // Remove replaced "call" instructions
    for (auto pGroupCall : m_groupCalls)
    {
        pGroupCall->dropAllReferences();
        pGroupCall->eraseFromParent();
    }
    m_groupCalls.clear();

    return m_changed;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchGroupOp::visitCallInst(
    CallInst & callInst) // [in] "Call" instruction
{
    const auto waveSize = m_pContext->GetGpuProperty()->waveSize;

    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    // Mutate group function with correct wave name
    auto mangledName = pCallee->getName();
    if (mangledName.find(kSPIRVName::GroupPrefix) != std::string::npos)
    {
        auto pos = mangledName.find("wave");
        if (pos != std::string::npos)
        {
            m_changed = true;
            std::string preStr = mangledName.substr(0, pos);
            pos += strlen("waveSz");
            std::string postStr = mangledName.substr(pos);
            std::string callName = preStr + "wave" + std::to_string(waveSize) + postStr;

            std::vector<Value*> args(callInst.op_begin(),
                callInst.op_begin() + callInst.getNumArgOperands());

            Value* pGroupCall = EmitCall(m_pModule,
                                         callName,
                                         callInst.getType(),
                                         args,
                                         NoAttrib,
                                         &callInst);
            callInst.replaceAllUsesWith(pGroupCall);
            m_groupCalls.insert(&callInst);
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM path operations for group operations.
INITIALIZE_PASS(PatchGroupOp, DEBUG_TYPE, "Patch LLVM for group operations", false, false)

