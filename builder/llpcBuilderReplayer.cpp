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
    // Set builder fast math flags from the recorded call.
    if (isa<FPMathOperator>(pCall))
    {
        m_pBuilder->setFastMathFlags(pCall->getFastMathFlags());
    }
    else
    {
        m_pBuilder->clearFastMathFlags();
    }

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

    // Replayer implementation of BuilderImplArith methods
    case BuilderRecorder::CubeFaceCoord:
        {
            return m_pBuilder->CreateCubeFaceCoord(args[0]);
        }

    case BuilderRecorder::CubeFaceIndex:
        {
            return m_pBuilder->CreateCubeFaceIndex(args[0]);
        }

    case BuilderRecorder::QuantizeToFp16:
        {
            return m_pBuilder->CreateQuantizeToFp16(args[0]);
        }

    case BuilderRecorder::SMod:
        {
            return m_pBuilder->CreateSMod(args[0], args[1]);
        }

    case BuilderRecorder::Tan:
        {
            return m_pBuilder->CreateTan(args[0]);
        }

    case BuilderRecorder::ASin:
        {
            return m_pBuilder->CreateASin(args[0]);
        }

    case BuilderRecorder::ACos:
        {
            return m_pBuilder->CreateACos(args[0]);
        }

    case BuilderRecorder::ATan:
        {
            return m_pBuilder->CreateATan(args[0]);
        }

    case BuilderRecorder::ATan2:
        {
            return m_pBuilder->CreateATan2(args[0], args[1]);
        }

    case BuilderRecorder::Sinh:
        {
            return m_pBuilder->CreateSinh(args[0]);
        }

    case BuilderRecorder::Cosh:
        {
            return m_pBuilder->CreateCosh(args[0]);
        }

    case BuilderRecorder::Tanh:
        {
            return m_pBuilder->CreateTanh(args[0]);
        }

    case BuilderRecorder::ASinh:
        {
            return m_pBuilder->CreateASinh(args[0]);
        }

    case BuilderRecorder::ACosh:
        {
            return m_pBuilder->CreateACosh(args[0]);
        }

    case BuilderRecorder::ATanh:
        {
            return m_pBuilder->CreateATanh(args[0]);
        }

    case BuilderRecorder::Power:
        {
            return m_pBuilder->CreatePower(args[0], args[1]);
        }

    case BuilderRecorder::Exp:
        {
            return m_pBuilder->CreateExp(args[0]);
        }

    case BuilderRecorder::Log:
        {
            return m_pBuilder->CreateLog(args[0]);
        }

    case BuilderRecorder::InverseSqrt:
        {
            return m_pBuilder->CreateInverseSqrt(args[0]);
        }

    // Replayer implementations of BuilderImplArith methods
    case BuilderRecorder::Opcode::Derivative:
        {
            return m_pBuilder->CreateDerivative(args[0],                                      // pInputValue
                                                cast<ConstantInt>(args[1])->getZExtValue(),   // isY
                                                cast<ConstantInt>(args[2])->getZExtValue());  // isFine
        }

    case BuilderRecorder::Opcode::FClamp:
        {
            return m_pBuilder->CreateFClamp(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::FMin3:
        {
            return m_pBuilder->CreateFMin3(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::FMax3:
        {
            return m_pBuilder->CreateFMax3(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::FMid3:
        {
            return m_pBuilder->CreateFMid3(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::InsertBitField:
        {
            return m_pBuilder->CreateInsertBitField(args[0], args[1], args[2], args[3]);
        }

    case BuilderRecorder::Opcode::ExtractBitField:
        {
            return m_pBuilder->CreateExtractBitField(args[0],
                                                     args[1],
                                                     args[2],
                                                     cast<ConstantInt>(args[3])->getZExtValue());
        }

    // Replayer implementations of BuilderImplDesc methods
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

    case BuilderRecorder::Opcode::IndexDescPtr:
        {
            return m_pBuilder->CreateIndexDescPtr(args[0],                                      // pDescPtr
                                                  args[1],                                      // pIndex
                                                  cast<ConstantInt>(args[2])->getZExtValue());  // isNonUniform
        }

    case BuilderRecorder::Opcode::LoadDescFromPtr:
        {
            return m_pBuilder->CreateLoadDescFromPtr(args[0]);
        }

    case BuilderRecorder::Opcode::GetSamplerDescPtr:
        {
            return m_pBuilder->CreateGetSamplerDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                       cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::GetImageDescPtr:
        {
            return m_pBuilder->CreateGetImageDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                     cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::GetFmaskDescPtr:
        {
            return m_pBuilder->CreateGetFmaskDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                     cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::GetTexelBufferDescPtr:
        {
            return m_pBuilder->CreateGetTexelBufferDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                           cast<ConstantInt>(args[1])->getZExtValue());  // binding
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

    // Replayer implementations of BuilderImplImage methods
    case BuilderRecorder::Opcode::ImageLoad:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            Value* pCoord = args[3];
            Value* pMipLevel = (args.size() > 4) ? &*args[4] : nullptr;
            return m_pBuilder->CreateImageLoad(pCall->getType(), dim, flags, pImageDesc, pCoord, pMipLevel);
        }

    case BuilderRecorder::Opcode::ImageLoadWithFmask:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            Value* pFmaskDesc = args[3];
            Value* pCoord = args[4];
            Value* pSampleNum = args[5];
            return m_pBuilder->CreateImageLoadWithFmask(pCall->getType(),
                                                        dim,
                                                        flags,
                                                        pImageDesc,
                                                        pFmaskDesc,
                                                        pCoord,
                                                        pSampleNum);
        }

    case BuilderRecorder::Opcode::ImageStore:
        {
            Value* pTexel = args[0];
            uint32_t dim = cast<ConstantInt>(args[1])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[2])->getZExtValue();
            Value* pImageDesc = args[3];
            Value* pCoord = args[4];
            Value* pMipLevel = (args.size() > 5) ? &*args[5] : nullptr;
            return m_pBuilder->CreateImageStore(pTexel, dim, flags, pImageDesc, pCoord, pMipLevel);
        }

    case BuilderRecorder::Opcode::ImageSample:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            Value* pSamplerDesc = args[3];
            uint32_t argsMask = cast<ConstantInt>(args[4])->getZExtValue();
            SmallVector<Value*, Builder::ImageAddressCount> address;
            address.resize(Builder::ImageAddressCount);
            args = args.slice(5);
            for (uint32_t i = 0; i != Builder::ImageAddressCount; ++i)
            {
                if ((argsMask >> i) & 1)
                {
                    address[i] = args[0];
                    args = args.slice(1);
                }
            }
            return m_pBuilder->CreateImageSample(pCall->getType(), dim, flags, pImageDesc, pSamplerDesc, address);
        }

    case BuilderRecorder::Opcode::ImageGather:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            Value* pSamplerDesc = args[3];
            uint32_t argsMask = cast<ConstantInt>(args[4])->getZExtValue();
            SmallVector<Value*, Builder::ImageAddressCount> address;
            address.resize(Builder::ImageAddressCount);
            args = args.slice(5);
            for (uint32_t i = 0; i != Builder::ImageAddressCount; ++i)
            {
                if ((argsMask >> i) & 1)
                {
                    address[i] = args[0];
                    args = args.slice(1);
                }
            }
            return m_pBuilder->CreateImageGather(pCall->getType(),
                                                 dim,
                                                 flags,
                                                 pImageDesc,
                                                 pSamplerDesc,
                                                 address);
        }

    case BuilderRecorder::Opcode::ImageAtomic:
        {
            uint32_t atomicOp = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t dim = cast<ConstantInt>(args[1])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[2])->getZExtValue();
            auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[3])->getZExtValue());
            Value* pImageDesc = args[4];
            Value* pCoord = args[5];
            Value* pInputValue = args[6];
            return m_pBuilder->CreateImageAtomic(atomicOp, dim, flags, ordering, pImageDesc, pCoord, pInputValue);
        }

    case BuilderRecorder::Opcode::ImageAtomicCompareSwap:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[2])->getZExtValue());
            Value* pImageDesc = args[3];
            Value* pCoord = args[4];
            Value* pInputValue = args[5];
            Value* pComparatorValue = args[6];
            return m_pBuilder->CreateImageAtomicCompareSwap(dim,
                                                            flags,
                                                            ordering,
                                                            pImageDesc,
                                                            pCoord,
                                                            pInputValue,
                                                            pComparatorValue);
        }

    case BuilderRecorder::Opcode::ImageQueryLevels:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            return m_pBuilder->CreateImageQueryLevels(dim, flags, pImageDesc);
        }

    case BuilderRecorder::Opcode::ImageQuerySamples:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            return m_pBuilder->CreateImageQuerySamples(dim, flags, pImageDesc);
        }

    case BuilderRecorder::Opcode::ImageQuerySize:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            Value* pLod = args[3];
            return m_pBuilder->CreateImageQuerySize(dim, flags, pImageDesc, pLod);
        }

    case BuilderRecorder::Opcode::ImageGetLod:
        {
            uint32_t dim = cast<ConstantInt>(args[0])->getZExtValue();
            uint32_t flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* pImageDesc = args[2];
            Value* pSamplerDesc = args[3];
            Value* pCoord = args[4];
            return m_pBuilder->CreateImageGetLod(dim, flags, pImageDesc, pSamplerDesc, pCoord);
        }

    // Replayer implementations of BuilderImplInOut methods
    case BuilderRecorder::Opcode::ReadGenericInput:
        {
            Builder::InOutInfo inputInfo(cast<ConstantInt>(args[4])->getZExtValue());
            return m_pBuilder->CreateReadGenericInput(
                                               pCall->getType(),                                // Result type
                                               cast<ConstantInt>(args[0])->getZExtValue(),      // Location
                                               args[1],                                         // Location offset
                                               isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Element index
                                               cast<ConstantInt>(args[3])->getZExtValue(),      // Location count
                                               inputInfo,                                       // Input info
                                               isa<UndefValue>(args[5]) ? nullptr : &*args[5]); // Vertex index
        }

    case BuilderRecorder::Opcode::ReadGenericOutput:
        {
            Builder::InOutInfo outputInfo(cast<ConstantInt>(args[4])->getZExtValue());
            return m_pBuilder->CreateReadGenericOutput(
                                                pCall->getType(),                               // Result type
                                                cast<ConstantInt>(args[0])->getZExtValue(),     // Location
                                                args[1],                                        // Location offset
                                                isa<UndefValue>(args[2]) ? nullptr : &*args[2], // Element index
                                                cast<ConstantInt>(args[3])->getZExtValue(),     // Location count
                                                outputInfo,                                     // Output info
                                                isa<UndefValue>(args[5]) ? nullptr: &*args[5]); // Vertex index
        }

    case BuilderRecorder::Opcode::WriteGenericOutput:
        {
            Builder::InOutInfo outputInfo(cast<ConstantInt>(args[5])->getZExtValue());
            return m_pBuilder->CreateWriteGenericOutput(
                                                 args[0],                                         // Value to write
                                                 cast<ConstantInt>(args[1])->getZExtValue(),      // Location
                                                 args[2],                                         // Location offset
                                                 isa<UndefValue>(args[3]) ? nullptr : &*args[3],  // Element index
                                                 cast<ConstantInt>(args[4])->getZExtValue(),      // Location count
                                                 outputInfo,                                      // Output info
                                                 isa<UndefValue>(args[6]) ? nullptr : &*args[6]); // Vertex index
        }

    case BuilderRecorder::Opcode::WriteXfbOutput:
        {
            Builder::InOutInfo outputInfo(cast<ConstantInt>(args[6])->getZExtValue());
            return m_pBuilder->CreateWriteXfbOutput(args[0],                                    // Value to write
                                                    cast<ConstantInt>(args[1])->getZExtValue(), // IsBuiltIn
                                                    cast<ConstantInt>(args[2])->getZExtValue(), // Location/builtIn
                                                    cast<ConstantInt>(args[3])->getZExtValue(), // XFB buffer number
                                                    cast<ConstantInt>(args[4])->getZExtValue(), // XFB stride
                                                    args[5],                                    // XFB byte offset
                                                    outputInfo);
        }

    case BuilderRecorder::Opcode::ReadBuiltInInput:
        {
            auto builtIn = static_cast<Builder::BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
            Builder::InOutInfo inputInfo(cast<ConstantInt>(args[1])->getZExtValue());
            return m_pBuilder->CreateReadBuiltInInput(builtIn,                                         // BuiltIn
                                                      inputInfo,                                       // Input info
                                                      isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                                      isa<UndefValue>(args[3]) ? nullptr : &*args[3]); // Index
        }

    case BuilderRecorder::Opcode::ReadBuiltInOutput:
        {
            auto builtIn = static_cast<Builder::BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
            Builder::InOutInfo outputInfo(cast<ConstantInt>(args[1])->getZExtValue());
            return m_pBuilder->CreateReadBuiltInOutput(builtIn,                                         // BuiltIn
                                                       outputInfo,                                      // Output info
                                                       isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                                       isa<UndefValue>(args[3]) ? nullptr : &*args[3]); // Index
        }

    case BuilderRecorder::Opcode::WriteBuiltInOutput:
        {
            auto builtIn = static_cast<Builder::BuiltInKind>(cast<ConstantInt>(args[1])->getZExtValue());
            Builder::InOutInfo outputInfo(cast<ConstantInt>(args[2])->getZExtValue());
            return m_pBuilder->CreateWriteBuiltInOutput(
                                                  args[0],                                          // Val to write
                                                  builtIn,                                          // BuiltIn
                                                  outputInfo,                                       // Output info
                                                  isa<UndefValue>(args[3]) ? nullptr : &*args[3],   // Vertex index
                                                  isa<UndefValue>(args[4]) ? nullptr : &*args[4]);  // Index
        }

    // Replayer implementations of BuilderImplMisc methods
    case BuilderRecorder::Opcode::EmitVertex:
        {
            return m_pBuilder->CreateEmitVertex(cast<ConstantInt>(args[0])->getZExtValue());
        }

    case BuilderRecorder::Opcode::EndPrimitive:
        {
            return m_pBuilder->CreateEndPrimitive(cast<ConstantInt>(args[0])->getZExtValue());
        }

    case BuilderRecorder::Opcode::Kill:
        {
            return m_pBuilder->CreateKill();
        }
    case BuilderRecorder::Opcode::ReadClock:
        {
            bool realtime = (cast<ConstantInt>(args[0])->getZExtValue() != 0);
            return m_pBuilder->CreateReadClock(realtime);
        }
    case BuilderRecorder::Opcode::DemoteToHelperInvocation:
        {
            return m_pBuilder->CreateDemoteToHelperInvocation();
        }
    case BuilderRecorder::Opcode::IsHelperInvocation:
        {
            return m_pBuilder->CreateIsHelperInvocation();
        }
    case BuilderRecorder::Opcode::TransposeMatrix:
        {
            return m_pBuilder->CreateTransposeMatrix(args[0]);
        }
    case BuilderRecorder::Opcode::MatrixTimesScalar:
        {
            return m_pBuilder->CreateMatrixTimesScalar(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::VectorTimesMatrix:
        {
            return m_pBuilder->CreateVectorTimesMatrix(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::MatrixTimesVector:
        {
            return m_pBuilder->CreateMatrixTimesVector(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::MatrixTimesMatrix:
        {
            return m_pBuilder->CreateMatrixTimesMatrix(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::OuterProduct:
        {
            return m_pBuilder->CreateOuterProduct(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::DotProduct:
        {
            return m_pBuilder->CreateDotProduct(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::Determinant:
        {
            return m_pBuilder->CreateDeterminant(args[0]);
        }
    case BuilderRecorder::Opcode::MatrixInverse:
        {
            return m_pBuilder->CreateMatrixInverse(args[0]);
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
            return m_pBuilder->CreateSubgroupAll(args[0], cast<ConstantInt>(args[1])->getZExtValue() != 0);
        }
    case BuilderRecorder::Opcode::SubgroupAny:
        {
            return m_pBuilder->CreateSubgroupAny(args[0], cast<ConstantInt>(args[1])->getZExtValue() != 0);
        }
    case BuilderRecorder::Opcode::SubgroupAllEqual:
        {
            return m_pBuilder->CreateSubgroupAllEqual(args[0], cast<ConstantInt>(args[1])->getZExtValue() != 0);
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
    case BuilderRecorder::Opcode::SubgroupSwizzleQuad:
        {
            return m_pBuilder->CreateSubgroupSwizzleQuad(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupSwizzleMask:
        {
            return m_pBuilder->CreateSubgroupSwizzleMask(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupWriteInvocation:
        {
            return m_pBuilder->CreateSubgroupWriteInvocation(args[0], args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupMbcnt:
        {
            return m_pBuilder->CreateSubgroupMbcnt(args[0]);
        }
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(BuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)
