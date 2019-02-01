/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderReplayer.cpp
 * @brief LLPC source file: BuilderReplayer pass
 ***********************************************************************************************************************
 */
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-builder-replayer"

using namespace Llpc;
using namespace llvm;

namespace
{

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class BuilderReplayer : public ModulePass
{
public:
    BuilderReplayer() : ModulePass(ID) {}
    BuilderReplayer(Builder* pBuilder) :
        ModulePass(ID),
        m_pBuilder(pBuilder)
    {
        initializeBuilderReplayerPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------
    static char ID;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderReplayer);

    // Represents a Builder call.
    struct BuilderCall
    {
        uint32_t  seqNum;   // Sequence number of this llpc.call.* call.
        CallInst* pCall;    // The call instruction

        // Comparison, used for sorting into sequence number order.
        bool operator<(const BuilderCall& other) const { return seqNum < other.seqNum; }
    };

    // The LLPC builder that the builder calls are being replayed on.
    std::unique_ptr<Builder> m_pBuilder;
};

} // anonymous

char BuilderReplayer::ID = 0;

// =====================================================================================================================
// Create BuilderReplayer pass
ModulePass* Llpc::CreateBuilderReplayer(
    Builder* pBuilder)    // [in] Builder to replay Builder calls on. The BuilderReplayer takes ownership of this.
{
    return new BuilderReplayer(pBuilder);
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
bool BuilderReplayer::runOnModule(
    Module& module)   // [in] Module to run this pass on
{
    LLVM_DEBUG(dbgs() << "Running the pass of replaying LLPC builder calls\n");

    // Gather the builder calls that were recorded.
    std::vector<BuilderCall> builderCalls;
    for (auto &func : module)
    {
        StringRef name = func.getName();
        if ((func.empty() != false) && name.startswith(BuilderCallPrefix))
        {
            // This is an llpc.call.* declaration. Add the calls that use it to the builderCalls vector.
            for (auto pUser : func.users())
            {
                auto pCall = cast<CallInst>(pUser);
                uint32_t seqNum = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
                builderCalls.push_back(BuilderCall({ seqNum, pCall }));
            }
        }
    }

    // Sort into seqNum order.
    std::sort(builderCalls.begin(), builderCalls.end());

    // Action each one by calling a method on m_pBuilder to create the IR.
    for (const auto& builderCall : builderCalls)
    {
        CallInst* pCall = builderCall.pCall;
        StringRef callName = pCall->getCalledFunction()->getName();
        auto opcode = BuilderRecorder::Opcode(cast<ConstantInt>(pCall->getOperand(0))->getZExtValue());
        LLPC_ASSERT(callName.startswith(BuilderCallPrefix));
        LLPC_ASSERT(callName.substr(strlen(BuilderCallPrefix)).startswith(BuilderRecorder::GetCallName(opcode)));

        StringRef name = builderCall.pCall->getName();
        Value* pNewValue = nullptr;
        m_pBuilder->SetInsertPoint(builderCall.pCall);

        switch (opcode)
        {
        case BuilderRecorder::Opcode::Nop:
            LLPC_NEVER_CALLED();
            break;
        case BuilderRecorder::Opcode::MiscKill:
            pNewValue = m_pBuilder->CreateKill(name);
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        // Replace the original one and erase it.
        LLVM_DEBUG(dbgs() << "BuilderReplayer replacing " << builderCall.pCall
                          << "\n   with " << *pNewValue << "\n");
        builderCall.pCall->replaceAllUsesWith(pNewValue);
        builderCall.pCall->eraseFromParent();
    }
    return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(BuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)

