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
#include "llpcContext.h"
#include "llpcInternal.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-builder-replayer"

using namespace Llpc;
using namespace llvm;

namespace
{

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class BuilderReplayer final : public ModulePass, BuilderRecorderMetadataKinds
{
public:
    BuilderReplayer() : ModulePass(ID) {}
    BuilderReplayer(Builder* pBuilder);

    bool runOnModule(Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderReplayer);

    void ReplayCall(uint32_t opcode, CallInst* pCall);
    void CheckCallAndReplay(Value* pValue);

    Value* ProcessCall(uint32_t opcode, CallInst* pCall);

    std::unique_ptr<Builder>                m_pBuilder;                         // The LLPC builder that the builder
                                                                                //  calls are being replayed on.
    Module*                                 m_pModule;                          // Module that the pass is being run on
    std::map<Function*, ShaderStage>        m_shaderStageMap;                   // Map function -> shader stage
    llvm::Function*                         m_pEnclosingFunc = nullptr;         // Last function written with current
                                                                                //  shader stage
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
// Constructor
BuilderReplayer::BuilderReplayer(
    Builder* pBuilder)      // [in] Builder to replay calls into
    :
    ModulePass(ID),
    BuilderRecorderMetadataKinds(static_cast<LLVMContext&>(pBuilder->getContext())),
    m_pBuilder(pBuilder)
{
    initializeBuilderReplayerPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
bool BuilderReplayer::runOnModule(
    Module& module)   // [in] Module to run this pass on
{
    LLVM_DEBUG(dbgs() << "Running the pass of replaying LLPC builder calls\n");

    m_pModule = &module;

    bool changed = false;

    SmallVector<Function*, 8> funcsToRemove;

    for (auto& func : module)
    {
        // Skip non-declarations that are definitely not LLPC intrinsics.
        if (func.isDeclaration() == false)
        {
            continue;
        }

        const MDNode* const pFuncMeta = func.getMetadata(m_opcodeMetaKindId);

        // Skip builder calls that do not have the correct metadata to identify the opcode.
        if (pFuncMeta == nullptr)
        {
            // If the function had the llpc builder call prefix, it means the metadata was not encoded correctly.
            LLPC_ASSERT(func.getName().startswith(BuilderCallPrefix) == false);
            continue;
        }

        const ConstantAsMetadata* const pMetaConst = cast<ConstantAsMetadata>(pFuncMeta->getOperand(0));
        uint32_t opcode = cast<ConstantInt>(pMetaConst->getValue())->getZExtValue();

        // If we got here we are definitely changing the module.
        changed = true;

        SmallVector<CallInst*, 8> callsToRemove;

        while (func.use_empty() == false)
        {
            CallInst* const pCall = dyn_cast<CallInst>(func.use_begin()->getUser());

            // Replay the call into BuilderImpl.
            ReplayCall(opcode, pCall);
        }

        func.clearMetadata();
        LLPC_ASSERT(func.user_empty());
        funcsToRemove.push_back(&func);
    }

    for (Function* const pFunc : funcsToRemove)
    {
        pFunc->eraseFromParent();
    }

    return changed;
}

// =====================================================================================================================
// Replay a recorded builder call.
void BuilderReplayer::ReplayCall(
    uint32_t  opcode,   // The builder call opcode
    CallInst* pCall)    // [in] The builder call to process
{
    // Change shader stage if necessary.
    Function* pEnclosingFunc = pCall->getParent()->getParent();
    if (pEnclosingFunc != m_pEnclosingFunc)
    {
        m_pEnclosingFunc = pEnclosingFunc;

        auto mapIt = m_shaderStageMap.find(pEnclosingFunc);
        ShaderStage stage = ShaderStageInvalid;
        if (mapIt == m_shaderStageMap.end())
        {
            stage = GetShaderStageFromFunction(pEnclosingFunc);
            m_shaderStageMap[pEnclosingFunc] = stage;
        }
        else
        {
            stage = mapIt->second;
        }
        m_pBuilder->SetShaderStage(stage);
    }

    // Set the insert point on the Builder. Also sets debug location to that of pCall.
    m_pBuilder->SetInsertPoint(pCall);

    // Process the builder call.
    LLVM_DEBUG(dbgs() << "Replaying " << *pCall << "\n");
    Value* pNewValue = ProcessCall(opcode, pCall);

    // Replace uses of the call with the new value, take the name, remove the old call.
    if (pNewValue != nullptr)
    {
        LLVM_DEBUG(dbgs() << "  replacing with: " << *pNewValue << "\n");
        pCall->replaceAllUsesWith(pNewValue);
        if (auto pNewInst = dyn_cast<Instruction>(pNewValue))
        {
            if (pCall->getName() != "")
            {
                pNewInst->takeName(pCall);
            }
        }
    }
    pCall->eraseFromParent();
}

// =====================================================================================================================
// If the passed value is a recorded builder call, replay it now.
// This is used in the waterfall loop workaround for not knowing the replay order.
void BuilderReplayer::CheckCallAndReplay(
    Value* pValue)    // [in] Value that might be a recorded call
{
    if (auto pCall = dyn_cast<CallInst>(pValue))
    {
        if (auto pFunc = pCall->getCalledFunction())
        {
            if (pFunc->getName().startswith(BuilderCallPrefix))
            {
                uint32_t opcode = cast<ConstantInt>(cast<ConstantAsMetadata>(
                                      pFunc->getMetadata(m_opcodeMetaKindId)->getOperand(0))
                                    ->getValue())->getZExtValue();

                ReplayCall(opcode, pCall);
            }
        }
    }
}

// =====================================================================================================================
// Process one recorder builder call.
// Returns the replacement value, or nullptr in the case that we do not want the caller to replace uses of
// pCall with the new value.
Value* BuilderReplayer::ProcessCall(
    uint32_t  opcode,   // The builder call opcode
    CallInst* pCall)    // [in] The builder call to process
{
    // Get the args.
    auto args = ArrayRef<Use>(&pCall->getOperandList()[0], pCall->getNumArgOperands());

    switch (opcode)
    {
    case BuilderRecorder::Opcode::Nop:
    default:
        {
            LLPC_NEVER_CALLED();
            return nullptr;
        }

    // Replayer implementations of BuilderImplDesc methods
    case BuilderRecorder::Opcode::WaterfallLoop:
    case BuilderRecorder::Opcode::WaterfallStoreLoop:
        {
            SmallVector<uint32_t, 2> operandIdxs;
            for (Value* pOperand : args)
            {
                if (auto pConstOperand = dyn_cast<ConstantInt>(pOperand))
                {
                    operandIdxs.push_back(pConstOperand->getZExtValue());
                }
            }

            Instruction* pNonUniformInst = nullptr;
            if (opcode == BuilderRecorder::Opcode::WaterfallLoop)
            {
                pNonUniformInst = cast<Instruction>(args[0]);
            }
            else
            {
                // This is the special case that we want to waterfall a store op with no result.
                // The llpc.call.waterfall.store.loop intercepts (one of) the non-uniform descriptor
                // input(s) to the store. Use that interception to find the store, and remove the
                // interception.
                Use& useInNonUniformInst = *pCall->use_begin();
                pNonUniformInst = cast<Instruction>(useInNonUniformInst.getUser());
                useInNonUniformInst = args[0];
            }

            // BuilderImpl::CreateWaterfallLoop looks back at each descriptor input to the op to find
            // the non-uniform index. It does not know about BuilderRecorder/BuilderReplayer, so here
            // we must work around the unknown order of replaying by finding any recorded descriptor
            // load and replay it first.
            for (uint32_t operandIdx : operandIdxs)
            {
                Value* pInput = cast<Instruction>(args[0])->getOperand(operandIdx);
                while (auto pGep = dyn_cast<GetElementPtrInst>(pInput))
                {
                    pInput = pGep->getOperand(0);
                }
                CheckCallAndReplay(pInput);
            }

            // Create the waterfall loop.
            auto pWaterfallLoop = m_pBuilder->CreateWaterfallLoop(pNonUniformInst, operandIdxs);

            if (opcode == BuilderRecorder::Opcode::WaterfallLoop)
            {
                return pWaterfallLoop;
            }

            // For the store op case, avoid using the replaceAllUsesWith in the caller.
            if (pCall->getName() != "")
            {
                pWaterfallLoop->takeName(pCall);
            }
            return nullptr;
        }

    case BuilderRecorder::Opcode::LoadBufferDesc:
        {
            return m_pBuilder->CreateLoadBufferDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue(),  // isNonUniform
                  isa<PointerType>(pCall->getType()) ?
                      pCall->getType()->getPointerElementType() :
                      nullptr);                                // pPointeeTy
        }

    case BuilderRecorder::Opcode::LoadSamplerDesc:
        {
            return m_pBuilder->CreateLoadSamplerDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::LoadResourceDesc:
        {
            return m_pBuilder->CreateLoadResourceDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::LoadTexelBufferDesc:
        {
            return m_pBuilder->CreateLoadTexelBufferDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::LoadFmaskDesc:
        {
            return m_pBuilder->CreateLoadFmaskDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::LoadPushConstantsPtr:
        {
            return m_pBuilder->CreateLoadPushConstantsPtr(
                  pCall->getType()->getPointerElementType());  // pPushConstantsTy
        }

    case BuilderRecorder::Opcode::GetBufferDescLength:
        {
            return m_pBuilder->CreateGetBufferDescLength(args[0]);
        }

    // Replayer implementations of BuilderImplMisc methods
    case BuilderRecorder::Opcode::Kill:
        {
            return m_pBuilder->CreateKill();
        }
    case BuilderRecorder::Opcode::ReadClock:
        {
            bool realtime = (cast<ConstantInt>(args[0])->getZExtValue() != 0);
            return m_pBuilder->CreateReadClock(realtime);
        }
    case BuilderRecorder::Opcode::TransposeMatrix:
        {
            return m_pBuilder->CreateTransposeMatrix(args[0]);
        }

    // Replayer implementations of BuilderImplSubgroup methods
    case BuilderRecorder::Opcode::GetSubgroupSize:
        {
            return m_pBuilder->CreateGetSubgroupSize();
        }
    case BuilderRecorder::Opcode::SubgroupElect:
        {
            return m_pBuilder->CreateSubgroupElect();
        }
    case BuilderRecorder::Opcode::SubgroupAll:
        {
            return m_pBuilder->CreateSubgroupAll(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupAny:
        {
            return m_pBuilder->CreateSubgroupAny(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupAllEqual:
        {
            return m_pBuilder->CreateSubgroupAllEqual(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBroadcast:
        {
            return m_pBuilder->CreateSubgroupBroadcast(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupBroadcastFirst:
        {
            return m_pBuilder->CreateSubgroupBroadcastFirst(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallot:
        {
            return m_pBuilder->CreateSubgroupBallot(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupInverseBallot:
        {
            return m_pBuilder->CreateSubgroupInverseBallot(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotBitExtract:
        {
            return m_pBuilder->CreateSubgroupBallotBitExtract(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotBitCount:
        {
            return m_pBuilder->CreateSubgroupBallotBitCount(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotInclusiveBitCount:
        {
            return m_pBuilder->CreateSubgroupBallotInclusiveBitCount(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotExclusiveBitCount:
        {
            return m_pBuilder->CreateSubgroupBallotExclusiveBitCount(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotFindLsb:
        {
            return m_pBuilder->CreateSubgroupBallotFindLsb(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotFindMsb:
        {
            return m_pBuilder->CreateSubgroupBallotFindMsb(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffle:
        {
            return m_pBuilder->CreateSubgroupShuffle(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffleXor:
        {
            return m_pBuilder->CreateSubgroupShuffleXor(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffleUp:
        {
            return m_pBuilder->CreateSubgroupShuffleUp(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffleDown:
        {
            return m_pBuilder->CreateSubgroupShuffleDown(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupClusteredReduction:
        {
            Builder::GroupArithOp groupArithOp =
                static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
            return m_pBuilder->CreateSubgroupClusteredReduction(groupArithOp, args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupClusteredInclusive:
        {
            Builder::GroupArithOp groupArithOp =
                static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
            return m_pBuilder->CreateSubgroupClusteredInclusive(groupArithOp, args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupClusteredExclusive:
        {
            Builder::GroupArithOp groupArithOp =
                static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
            return m_pBuilder->CreateSubgroupClusteredExclusive(groupArithOp, args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadBroadcast:
        {
            return m_pBuilder->CreateSubgroupQuadBroadcast(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadSwapHorizontal:
        {
            return m_pBuilder->CreateSubgroupQuadSwapHorizontal(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadSwapVertical:
        {
            return m_pBuilder->CreateSubgroupQuadSwapVertical(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadSwapDiagonal:
        {
            return m_pBuilder->CreateSubgroupQuadSwapDiagonal(args[0]);
        }
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(BuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)
