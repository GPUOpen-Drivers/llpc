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
class BuilderReplayer final : public ModulePass
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

    bool changed = false;

    SmallVector<Function*, 8> funcsToRemove;

    for (auto& func : module)
    {
        // Skip non-declarations that are definitely not LLPC intrinsics.
        if (func.isDeclaration() == false)
        {
            continue;
        }

        const MDNode* const pFuncMeta = func.getMetadata(BuilderCallMetadataName);

        // Skip builder calls that do not have the correct metadata to identify the opcode.
        if (pFuncMeta == nullptr)
        {
            // If the function had the llpc builder call prefix, it means the metadata was not encoded correctly.
            LLPC_ASSERT(func.getName().startswith(BuilderCallPrefix) == false);
            continue;
        }

        const ConstantAsMetadata* const pMetaConst = dyn_cast<ConstantAsMetadata>(pFuncMeta->getOperand(0));
        LLPC_ASSERT(pMetaConst != nullptr);

        const ConstantInt* const pConst = dyn_cast<ConstantInt>(pMetaConst->getValue());
        LLPC_ASSERT(pConst != nullptr);

        // If we got here we are definitely changing the module.
        changed = true;

        SmallVector<CallInst*, 8> callsToRemove;

        for (User* const pUser : func.users())
        {
            CallInst* const pCall = dyn_cast<CallInst>(pUser);

            // Skip users that are not calls.
            if (pCall == nullptr)
            {
                continue;
            }

            m_pBuilder->SetInsertPoint(pCall);

            Value* pNewValue = nullptr;

            switch (BuilderRecorder::Opcode(pConst->getZExtValue()))
            {
            // NOP
            case BuilderRecorder::Opcode::Nop:
            default:
                {
                    LLPC_NEVER_CALLED();
                    continue;
                }

            // Replayer implementations of BuilderImplDesc methods
            case BuilderRecorder::Opcode::DescWaterfallLoop:
                {
                    SmallVector<uint32_t, 2> operandIdxs;
                    for (Value* pOperand : pCall->arg_operands())
                    {
                        if (auto pConstOperand = dyn_cast<ConstantInt>(pOperand))
                        {
                            operandIdxs.push_back(pConstOperand->getZExtValue());
                        }
                    }
                    pNewValue = m_pBuilder->CreateWaterfallLoop(cast<Instruction>(pCall->getArgOperand(0)),
                                                                operandIdxs);
                    break;
                }

            case BuilderRecorder::Opcode::DescWaterfallStoreLoop:
                {
                    SmallVector<uint32_t, 2> operandIdxs;
                    for (Value* pOperand : pCall->arg_operands())
                    {
                        if (auto pConstOperand = dyn_cast<ConstantInt>(pOperand))
                        {
                            operandIdxs.push_back(pConstOperand->getZExtValue());
                        }
                    }

                    // This is the special case that we want to waterfall a store op with no result.
                    // The llpc.call.waterfall.store.loop intercepts (one of) the non-uniform descriptor
                    // input(s) to the store. Use that interception to find the store, and remove the
                    // interception.
                    Use& useInNonUniformInst = *pCall->use_begin();
                    auto pNonUniformInst = cast<Instruction>(useInNonUniformInst.getUser());
                    useInNonUniformInst = pCall->getArgOperand(0);
                    auto pWaterfallLoop = m_pBuilder->CreateWaterfallLoop(pNonUniformInst, operandIdxs);
                    // Avoid using the replaceAllUsesWith after the end of this switch.
                    pWaterfallLoop->takeName(pCall);
                    callsToRemove.push_back(pCall);
                    continue;
                }

            case BuilderRecorder::Opcode::DescLoadBuffer:
                {
                    pNewValue = m_pBuilder->CreateLoadBufferDesc(
                          cast<ConstantInt>(pCall->getArgOperand(0))->getZExtValue(),  // descSet
                          cast<ConstantInt>(pCall->getArgOperand(1))->getZExtValue(),  // binding
                          pCall->getArgOperand(2),                                     // pBlockOffset
                          cast<ConstantInt>(pCall->getArgOperand(3))->getZExtValue(),  // isNonUniform
                          isa<PointerType>(pCall->getType()) ?
                              pCall->getType()->getPointerElementType() :
                              nullptr);                                                // pPointeeTy
                    break;
                }

            case BuilderRecorder::Opcode::DescLoadSpillTablePtr:
                {
                    pNewValue = m_pBuilder->CreateLoadSpillTablePtr(
                          pCall->getType()->getPointerElementType());                  // pSpillTableTy
                    break;
                }

            // Replayer implementations of BuilderImplMisc methods
            case BuilderRecorder::Opcode::MiscKill:
                {
                    pNewValue = m_pBuilder->CreateKill();
                    break;
                }
            case BuilderRecorder::Opcode::MiscReadClock:
                {
                    LLPC_ASSERT(isa<ConstantInt>(pCall->getArgOperand(0)));
                    bool realtime = (cast<ConstantInt>(pCall->getArgOperand(0))->getZExtValue() != 0);
                    pNewValue = m_pBuilder->CreateReadClock(realtime);
                    break;
                }

            }

            if (auto pNewInst = dyn_cast<Instruction>(pNewValue))
            {
                pNewInst->takeName(pCall);
            }
            pCall->replaceAllUsesWith(pNewValue);
            callsToRemove.push_back(pCall);
        }

        for (CallInst* const pCall : callsToRemove)
        {
            pCall->eraseFromParent();
        }

        if (func.user_empty())
        {
            funcsToRemove.push_back(&func);
        }
    }

    for (Function* const pFunc : funcsToRemove)
    {
        pFunc->eraseFromParent();
    }

    return changed;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(BuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)
