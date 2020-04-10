/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/llpcBuilderContext.h"
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"
#include "llpcPipelineState.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-builder-replayer"

using namespace lgc;
using namespace llvm;

namespace
{

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class BuilderReplayer final : public ModulePass, BuilderRecorderMetadataKinds
{
public:
    BuilderReplayer() : ModulePass(ID) {}
    BuilderReplayer(Pipeline* pipeline);

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
    }

    bool runOnModule(Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;

private:
    BuilderReplayer(const BuilderReplayer&) = delete;
    BuilderReplayer& operator=(const BuilderReplayer&) = delete;

    void replayCall(unsigned opcode, CallInst* call);

    Value* processCall(unsigned opcode, CallInst* call);

    std::unique_ptr<Builder>                m_builder;                         // The LLPC builder that the builder
                                                                                //  calls are being replayed on.
    std::map<Function*, ShaderStage>        m_shaderStageMap;                   // Map function -> shader stage
    llvm::Function*                         m_enclosingFunc = nullptr;         // Last function written with current
                                                                                //  shader stage
};

} // anonymous

char BuilderReplayer::ID = 0;

// =====================================================================================================================
// Create BuilderReplayer pass
//
// @param pipeline : Pipeline object
ModulePass* lgc::createBuilderReplayer(
    Pipeline*  pipeline)
{
    return new BuilderReplayer(pipeline);
}

// =====================================================================================================================
// Constructor
//
// @param pipeline : Pipeline object
BuilderReplayer::BuilderReplayer(
    Pipeline*  pipeline)
    :
    ModulePass(ID),
    BuilderRecorderMetadataKinds(static_cast<LLVMContext&>(pipeline->getContext()))
{
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
//
// @param module : Module to run this pass on
bool BuilderReplayer::runOnModule(
    Module& module)
{
    LLVM_DEBUG(dbgs() << "Running the pass of replaying LLPC builder calls\n");

    // Set up the pipeline state from the specified linked IR module.
    PipelineState* pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
    pipelineState->readState(&module);

    // Create the BuilderImpl to replay into, passing it the PipelineState
    BuilderContext* builderContext = pipelineState->getBuilderContext();
    m_builder.reset(builderContext->createBuilder(pipelineState, /*useBuilderRecorder=*/false));

    SmallVector<Function*, 8> funcsToRemove;

    for (auto& func : module)
    {
        // Skip non-declarations that are definitely not LLPC intrinsics.
        if (!func.isDeclaration())
            continue;

        const MDNode* const funcMeta = func.getMetadata(opcodeMetaKindId);

        // Skip builder calls that do not have the correct metadata to identify the opcode.
        if (!funcMeta )
        {
            // If the function had the llpc builder call prefix, it means the metadata was not encoded correctly.
            assert(func.getName().startswith(BuilderCallPrefix) == false);
            continue;
        }

        const ConstantAsMetadata* const metaConst = cast<ConstantAsMetadata>(funcMeta->getOperand(0));
        unsigned opcode = cast<ConstantInt>(metaConst->getValue())->getZExtValue();

        SmallVector<CallInst*, 8> callsToRemove;

        while (!func.use_empty())
        {
            CallInst* const call = dyn_cast<CallInst>(func.use_begin()->getUser());

            // Replay the call into BuilderImpl.
            replayCall(opcode, call);
        }

        func.clearMetadata();
        assert(func.user_empty());
        funcsToRemove.push_back(&func);
    }

    for (Function* const func : funcsToRemove)
        func->eraseFromParent();

    return true;
}

// =====================================================================================================================
// Replay a recorded builder call.
//
// @param opcode : The builder call opcode
// @param call : The builder call to process
void BuilderReplayer::replayCall(
    unsigned  opcode,
    CallInst* call)
{
    // Change shader stage if necessary.
    Function* enclosingFunc = call->getParent()->getParent();
    if (enclosingFunc != m_enclosingFunc)
    {
        m_enclosingFunc = enclosingFunc;

        auto mapIt = m_shaderStageMap.find(enclosingFunc);
        ShaderStage stage = ShaderStageInvalid;
        if (mapIt == m_shaderStageMap.end())
        {
            stage = getShaderStageFromFunction(enclosingFunc);
            m_shaderStageMap[enclosingFunc] = stage;
        }
        else
            stage = mapIt->second;
        m_builder->setShaderStage(stage);
    }

    // Set the insert point on the Builder. Also sets debug location to that of pCall.
    m_builder->SetInsertPoint(call);

    // Process the builder call.
    LLVM_DEBUG(dbgs() << "Replaying " << *call << "\n");
    Value* newValue = processCall(opcode, call);

    // Replace uses of the call with the new value, take the name, remove the old call.
    if (newValue )
    {
        LLVM_DEBUG(dbgs() << "  replacing with: " << *newValue << "\n");
        call->replaceAllUsesWith(newValue);
        if (auto newInst = dyn_cast<Instruction>(newValue))
        {
            if (call->getName() != "")
                newInst->takeName(call);
        }
    }
    call->eraseFromParent();
}

// =====================================================================================================================
// Process one recorder builder call.
// Returns the replacement value, or nullptr in the case that we do not want the caller to replace uses of
// pCall with the new value.
//
// @param opcode : The builder call opcode
// @param call : The builder call to process
Value* BuilderReplayer::processCall(
    unsigned  opcode,
    CallInst* call)
{
    // Set builder fast math flags from the recorded call.
    if (isa<FPMathOperator>(call))
        m_builder->setFastMathFlags(call->getFastMathFlags());
    else
        m_builder->clearFastMathFlags();

    // Get the args.
    auto args = ArrayRef<Use>(&call->getOperandList()[0], call->getNumArgOperands());

    switch (opcode)
    {
    case BuilderRecorder::Opcode::Nop:
    default:
        {
            llvm_unreachable("Should never be called!");
            return nullptr;
        }

    // Replayer implementation of BuilderImplArith methods
    case BuilderRecorder::CubeFaceCoord:
        {
            return m_builder->CreateCubeFaceCoord(args[0]);
        }

    case BuilderRecorder::CubeFaceIndex:
        {
            return m_builder->CreateCubeFaceIndex(args[0]);
        }

    case BuilderRecorder::FpTruncWithRounding:
        {
            auto roundingMode = static_cast<unsigned>(
                                  cast<ConstantInt>(args[1])->getZExtValue());
            return m_builder->CreateFpTruncWithRounding(args[0], call->getType(), roundingMode);
        }

    case BuilderRecorder::QuantizeToFp16:
        {
            return m_builder->CreateQuantizeToFp16(args[0]);
        }

    case BuilderRecorder::SMod:
        {
            return m_builder->CreateSMod(args[0], args[1]);
        }

    case BuilderRecorder::FMod:
        {
            return m_builder->CreateFMod(args[0], args[1]);
        }

    case BuilderRecorder::Fma:
        {
            return m_builder->CreateFma(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Tan:
        {
            return m_builder->CreateTan(args[0]);
        }

    case BuilderRecorder::ASin:
        {
            return m_builder->CreateASin(args[0]);
        }

    case BuilderRecorder::ACos:
        {
            return m_builder->CreateACos(args[0]);
        }

    case BuilderRecorder::ATan:
        {
            return m_builder->CreateATan(args[0]);
        }

    case BuilderRecorder::ATan2:
        {
            return m_builder->CreateATan2(args[0], args[1]);
        }

    case BuilderRecorder::Sinh:
        {
            return m_builder->CreateSinh(args[0]);
        }

    case BuilderRecorder::Cosh:
        {
            return m_builder->CreateCosh(args[0]);
        }

    case BuilderRecorder::Tanh:
        {
            return m_builder->CreateTanh(args[0]);
        }

    case BuilderRecorder::ASinh:
        {
            return m_builder->CreateASinh(args[0]);
        }

    case BuilderRecorder::ACosh:
        {
            return m_builder->CreateACosh(args[0]);
        }

    case BuilderRecorder::ATanh:
        {
            return m_builder->CreateATanh(args[0]);
        }

    case BuilderRecorder::Power:
        {
            return m_builder->CreatePower(args[0], args[1]);
        }

    case BuilderRecorder::Exp:
        {
            return m_builder->CreateExp(args[0]);
        }

    case BuilderRecorder::Log:
        {
            return m_builder->CreateLog(args[0]);
        }

    case BuilderRecorder::InverseSqrt:
        {
            return m_builder->CreateInverseSqrt(args[0]);
        }

    case BuilderRecorder::SAbs:
        {
            return m_builder->CreateSAbs(args[0]);
        }

    case BuilderRecorder::FSign:
        {
            return m_builder->CreateFSign(args[0]);
        }

    case BuilderRecorder::SSign:
        {
            return m_builder->CreateSSign(args[0]);
        }

    case BuilderRecorder::Fract:
        {
            return m_builder->CreateFract(args[0]);
        }

    case BuilderRecorder::SmoothStep:
        {
            return m_builder->CreateSmoothStep(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Ldexp:
        {
            return m_builder->CreateLdexp(args[0], args[1]);
        }

    case BuilderRecorder::ExtractSignificand:
        {
            return m_builder->CreateExtractSignificand(args[0]);
        }

    case BuilderRecorder::ExtractExponent:
        {
            return m_builder->CreateExtractExponent(args[0]);
        }

    case BuilderRecorder::CrossProduct:
        {
            return m_builder->CreateCrossProduct(args[0], args[1]);
        }

    case BuilderRecorder::NormalizeVector:
        {
            return m_builder->CreateNormalizeVector(args[0]);
        }

    case BuilderRecorder::FaceForward:
        {
            return m_builder->CreateFaceForward(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Reflect:
        {
            return m_builder->CreateReflect(args[0], args[1]);
        }

    case BuilderRecorder::Refract:
        {
            return m_builder->CreateRefract(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::Derivative:
        {
            return m_builder->CreateDerivative(args[0],                                      // pInputValue
                                                cast<ConstantInt>(args[1])->getZExtValue(),   // isY
                                                cast<ConstantInt>(args[2])->getZExtValue());  // isFine
        }

    case BuilderRecorder::Opcode::FClamp:
        {
            return m_builder->CreateFClamp(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::FMin:
        {
            return m_builder->CreateFMin(args[0], args[1]);
        }

    case BuilderRecorder::Opcode::FMax:
        {
            return m_builder->CreateFMax(args[0], args[1]);
        }

    case BuilderRecorder::Opcode::FMin3:
        {
            return m_builder->CreateFMin3(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::FMax3:
        {
            return m_builder->CreateFMax3(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::FMid3:
        {
            return m_builder->CreateFMid3(args[0], args[1], args[2]);
        }

    case BuilderRecorder::Opcode::IsInf:
        {
            return m_builder->CreateIsInf(args[0]);
        }

    case BuilderRecorder::Opcode::IsNaN:
        {
            return m_builder->CreateIsNaN(args[0]);
        }

    case BuilderRecorder::Opcode::InsertBitField:
        {
            return m_builder->CreateInsertBitField(args[0], args[1], args[2], args[3]);
        }

    case BuilderRecorder::Opcode::ExtractBitField:
        {
            return m_builder->CreateExtractBitField(args[0],
                                                     args[1],
                                                     args[2],
                                                     cast<ConstantInt>(args[3])->getZExtValue());
        }

    case BuilderRecorder::Opcode::FindSMsb:
        {
            return m_builder->CreateFindSMsb(args[0]);
        }

    case BuilderRecorder::Opcode::FMix:
        {
            return m_builder->createFMix(args[0], args[1], args[2]);
        }

    // Replayer implementations of BuilderImplDesc methods
    case BuilderRecorder::Opcode::LoadBufferDesc:
        {
            return m_builder->CreateLoadBufferDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue(),  // isNonUniform
                  cast<ConstantInt>(args[4])->getZExtValue(),  // isWritten
                  isa<PointerType>(call->getType()) ?
                      call->getType()->getPointerElementType() :
                      nullptr);                                // pPointeeTy
        }

    case BuilderRecorder::Opcode::IndexDescPtr:
        {
            return m_builder->CreateIndexDescPtr(args[0],                                      // pDescPtr
                                                  args[1],                                      // pIndex
                                                  cast<ConstantInt>(args[2])->getZExtValue());  // isNonUniform
        }

    case BuilderRecorder::Opcode::LoadDescFromPtr:
        {
            return m_builder->CreateLoadDescFromPtr(args[0]);
        }

    case BuilderRecorder::Opcode::GetSamplerDescPtr:
        {
            return m_builder->CreateGetSamplerDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                       cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::GetImageDescPtr:
        {
            return m_builder->CreateGetImageDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                     cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::GetFmaskDescPtr:
        {
            return m_builder->CreateGetFmaskDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                     cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::GetTexelBufferDescPtr:
        {
            return m_builder->CreateGetTexelBufferDescPtr(cast<ConstantInt>(args[0])->getZExtValue(),   // descSet
                                                           cast<ConstantInt>(args[1])->getZExtValue());  // binding
        }

    case BuilderRecorder::Opcode::LoadPushConstantsPtr:
        {
            return m_builder->CreateLoadPushConstantsPtr(
                  call->getType()->getPointerElementType());  // pPushConstantsTy
        }

    case BuilderRecorder::Opcode::GetBufferDescLength:
        {
            return m_builder->CreateGetBufferDescLength(args[0]);
        }

    // Replayer implementations of BuilderImplImage methods
    case BuilderRecorder::Opcode::ImageLoad:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            Value* coord = args[3];
            Value* mipLevel = args.size() > 4 ? &*args[4] : nullptr;
            return m_builder->CreateImageLoad(call->getType(), dim, flags, imageDesc, coord, mipLevel);
        }

    case BuilderRecorder::Opcode::ImageLoadWithFmask:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            Value* fmaskDesc = args[3];
            Value* coord = args[4];
            Value* sampleNum = args[5];
            return m_builder->CreateImageLoadWithFmask(call->getType(),
                                                        dim,
                                                        flags,
                                                        imageDesc,
                                                        fmaskDesc,
                                                        coord,
                                                        sampleNum);
        }

    case BuilderRecorder::Opcode::ImageStore:
        {
            Value* texel = args[0];
            unsigned dim = cast<ConstantInt>(args[1])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[2])->getZExtValue();
            Value* imageDesc = args[3];
            Value* coord = args[4];
            Value* mipLevel = args.size() > 5 ? &*args[5] : nullptr;
            return m_builder->CreateImageStore(texel, dim, flags, imageDesc, coord, mipLevel);
        }

    case BuilderRecorder::Opcode::ImageSample:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            Value* samplerDesc = args[3];
            unsigned argsMask = cast<ConstantInt>(args[4])->getZExtValue();
            SmallVector<Value*, Builder::ImageAddressCount> address;
            address.resize(Builder::ImageAddressCount);
            args = args.slice(5);
            for (unsigned i = 0; i != Builder::ImageAddressCount; ++i)
            {
                if ((argsMask >> i) & 1)
                {
                    address[i] = args[0];
                    args = args.slice(1);
                }
            }
            return m_builder->CreateImageSample(call->getType(), dim, flags, imageDesc, samplerDesc, address);
        }

    case BuilderRecorder::Opcode::ImageGather:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            Value* samplerDesc = args[3];
            unsigned argsMask = cast<ConstantInt>(args[4])->getZExtValue();
            SmallVector<Value*, Builder::ImageAddressCount> address;
            address.resize(Builder::ImageAddressCount);
            args = args.slice(5);
            for (unsigned i = 0; i != Builder::ImageAddressCount; ++i)
            {
                if ((argsMask >> i) & 1)
                {
                    address[i] = args[0];
                    args = args.slice(1);
                }
            }
            return m_builder->CreateImageGather(call->getType(),
                                                 dim,
                                                 flags,
                                                 imageDesc,
                                                 samplerDesc,
                                                 address);
        }

    case BuilderRecorder::Opcode::ImageAtomic:
        {
            unsigned atomicOp = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned dim = cast<ConstantInt>(args[1])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[2])->getZExtValue();
            auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[3])->getZExtValue());
            Value* imageDesc = args[4];
            Value* coord = args[5];
            Value* inputValue = args[6];
            return m_builder->CreateImageAtomic(atomicOp, dim, flags, ordering, imageDesc, coord, inputValue);
        }

    case BuilderRecorder::Opcode::ImageAtomicCompareSwap:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[2])->getZExtValue());
            Value* imageDesc = args[3];
            Value* coord = args[4];
            Value* inputValue = args[5];
            Value* comparatorValue = args[6];
            return m_builder->CreateImageAtomicCompareSwap(dim,
                                                            flags,
                                                            ordering,
                                                            imageDesc,
                                                            coord,
                                                            inputValue,
                                                            comparatorValue);
        }

    case BuilderRecorder::Opcode::ImageQueryLevels:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            return m_builder->CreateImageQueryLevels(dim, flags, imageDesc);
        }

    case BuilderRecorder::Opcode::ImageQuerySamples:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            return m_builder->CreateImageQuerySamples(dim, flags, imageDesc);
        }

    case BuilderRecorder::Opcode::ImageQuerySize:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            Value* lod = args[3];
            return m_builder->CreateImageQuerySize(dim, flags, imageDesc, lod);
        }

    case BuilderRecorder::Opcode::ImageGetLod:
        {
            unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
            unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
            Value* imageDesc = args[2];
            Value* samplerDesc = args[3];
            Value* coord = args[4];
            return m_builder->CreateImageGetLod(dim, flags, imageDesc, samplerDesc, coord);
        }

    // Replayer implementations of BuilderImplInOut methods
    case BuilderRecorder::Opcode::ReadGenericInput:
        {
            InOutInfo inputInfo(cast<ConstantInt>(args[4])->getZExtValue());
            return m_builder->CreateReadGenericInput(
                                               call->getType(),                                // Result type
                                               cast<ConstantInt>(args[0])->getZExtValue(),      // Location
                                               args[1],                                         // Location offset
                                               isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Element index
                                               cast<ConstantInt>(args[3])->getZExtValue(),      // Location count
                                               inputInfo,                                       // Input info
                                               isa<UndefValue>(args[5]) ? nullptr : &*args[5]); // Vertex index
        }

    case BuilderRecorder::Opcode::ReadGenericOutput:
        {
            InOutInfo outputInfo(cast<ConstantInt>(args[4])->getZExtValue());
            return m_builder->CreateReadGenericOutput(
                                                call->getType(),                               // Result type
                                                cast<ConstantInt>(args[0])->getZExtValue(),     // Location
                                                args[1],                                        // Location offset
                                                isa<UndefValue>(args[2]) ? nullptr : &*args[2], // Element index
                                                cast<ConstantInt>(args[3])->getZExtValue(),     // Location count
                                                outputInfo,                                     // Output info
                                                isa<UndefValue>(args[5]) ? nullptr: &*args[5]); // Vertex index
        }

    case BuilderRecorder::Opcode::WriteGenericOutput:
        {
            InOutInfo outputInfo(cast<ConstantInt>(args[5])->getZExtValue());
            return m_builder->CreateWriteGenericOutput(
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
            InOutInfo outputInfo(cast<ConstantInt>(args[6])->getZExtValue());
            return m_builder->CreateWriteXfbOutput(args[0],                                    // Value to write
                                                    cast<ConstantInt>(args[1])->getZExtValue(), // IsBuiltIn
                                                    cast<ConstantInt>(args[2])->getZExtValue(), // Location/builtIn
                                                    cast<ConstantInt>(args[3])->getZExtValue(), // XFB buffer ID
                                                    cast<ConstantInt>(args[4])->getZExtValue(), // XFB stride
                                                    args[5],                                    // XFB byte offset
                                                    outputInfo);
        }

    case BuilderRecorder::Opcode::ReadBuiltInInput:
        {
            auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
            InOutInfo inputInfo(cast<ConstantInt>(args[1])->getZExtValue());
            return m_builder->CreateReadBuiltInInput(builtIn,                                         // BuiltIn
                                                      inputInfo,                                       // Input info
                                                      isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                                      isa<UndefValue>(args[3]) ? nullptr : &*args[3]); // Index
        }

    case BuilderRecorder::Opcode::ReadBuiltInOutput:
        {
            auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
            InOutInfo outputInfo(cast<ConstantInt>(args[1])->getZExtValue());
            return m_builder->CreateReadBuiltInOutput(builtIn,                                         // BuiltIn
                                                       outputInfo,                                      // Output info
                                                       isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                                       isa<UndefValue>(args[3]) ? nullptr : &*args[3]); // Index
        }

    case BuilderRecorder::Opcode::WriteBuiltInOutput:
        {
            auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[1])->getZExtValue());
            InOutInfo outputInfo(cast<ConstantInt>(args[2])->getZExtValue());
            return m_builder->CreateWriteBuiltInOutput(
                                                  args[0],                                          // Val to write
                                                  builtIn,                                          // BuiltIn
                                                  outputInfo,                                       // Output info
                                                  isa<UndefValue>(args[3]) ? nullptr : &*args[3],   // Vertex index
                                                  isa<UndefValue>(args[4]) ? nullptr : &*args[4]);  // Index
        }

    // Replayer implementations of BuilderImplMisc methods
    case BuilderRecorder::Opcode::EmitVertex:
        {
            return m_builder->CreateEmitVertex(cast<ConstantInt>(args[0])->getZExtValue());
        }

    case BuilderRecorder::Opcode::EndPrimitive:
        {
            return m_builder->CreateEndPrimitive(cast<ConstantInt>(args[0])->getZExtValue());
        }

    case BuilderRecorder::Opcode::Barrier:
        {
            return m_builder->CreateBarrier();
        }

    case BuilderRecorder::Opcode::Kill:
        {
            return m_builder->CreateKill();
        }
    case BuilderRecorder::Opcode::ReadClock:
        {
            bool realtime = (cast<ConstantInt>(args[0])->getZExtValue() != 0);
            return m_builder->CreateReadClock(realtime);
        }
    case BuilderRecorder::Opcode::DemoteToHelperInvocation:
        {
            return m_builder->CreateDemoteToHelperInvocation();
        }
    case BuilderRecorder::Opcode::IsHelperInvocation:
        {
            return m_builder->CreateIsHelperInvocation();
        }
    case BuilderRecorder::Opcode::TransposeMatrix:
        {
            return m_builder->CreateTransposeMatrix(args[0]);
        }
    case BuilderRecorder::Opcode::MatrixTimesScalar:
        {
            return m_builder->CreateMatrixTimesScalar(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::VectorTimesMatrix:
        {
            return m_builder->CreateVectorTimesMatrix(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::MatrixTimesVector:
        {
            return m_builder->CreateMatrixTimesVector(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::MatrixTimesMatrix:
        {
            return m_builder->CreateMatrixTimesMatrix(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::OuterProduct:
        {
            return m_builder->CreateOuterProduct(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::DotProduct:
        {
            return m_builder->CreateDotProduct(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::Determinant:
        {
            return m_builder->CreateDeterminant(args[0]);
        }
    case BuilderRecorder::Opcode::MatrixInverse:
        {
            return m_builder->CreateMatrixInverse(args[0]);
        }

    // Replayer implementations of BuilderImplSubgroup methods
    case BuilderRecorder::Opcode::GetSubgroupSize:
        {
            return m_builder->CreateGetSubgroupSize();
        }
    case BuilderRecorder::Opcode::SubgroupElect:
        {
            return m_builder->CreateSubgroupElect();
        }
    case BuilderRecorder::Opcode::SubgroupAll:
        {
            return m_builder->CreateSubgroupAll(args[0], cast<ConstantInt>(args[1])->getZExtValue() != 0);
        }
    case BuilderRecorder::Opcode::SubgroupAny:
        {
            return m_builder->CreateSubgroupAny(args[0], cast<ConstantInt>(args[1])->getZExtValue() != 0);
        }
    case BuilderRecorder::Opcode::SubgroupAllEqual:
        {
            return m_builder->CreateSubgroupAllEqual(args[0], cast<ConstantInt>(args[1])->getZExtValue() != 0);
        }
    case BuilderRecorder::Opcode::SubgroupBroadcast:
        {
            return m_builder->CreateSubgroupBroadcast(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupBroadcastFirst:
        {
            return m_builder->CreateSubgroupBroadcastFirst(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallot:
        {
            return m_builder->CreateSubgroupBallot(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupInverseBallot:
        {
            return m_builder->CreateSubgroupInverseBallot(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotBitExtract:
        {
            return m_builder->CreateSubgroupBallotBitExtract(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotBitCount:
        {
            return m_builder->CreateSubgroupBallotBitCount(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotInclusiveBitCount:
        {
            return m_builder->CreateSubgroupBallotInclusiveBitCount(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotExclusiveBitCount:
        {
            return m_builder->CreateSubgroupBallotExclusiveBitCount(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotFindLsb:
        {
            return m_builder->CreateSubgroupBallotFindLsb(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupBallotFindMsb:
        {
            return m_builder->CreateSubgroupBallotFindMsb(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffle:
        {
            return m_builder->CreateSubgroupShuffle(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffleXor:
        {
            return m_builder->CreateSubgroupShuffleXor(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffleUp:
        {
            return m_builder->CreateSubgroupShuffleUp(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupShuffleDown:
        {
            return m_builder->CreateSubgroupShuffleDown(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupClusteredReduction:
        {
            Builder::GroupArithOp groupArithOp =
                static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
            return m_builder->CreateSubgroupClusteredReduction(groupArithOp, args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupClusteredInclusive:
        {
            Builder::GroupArithOp groupArithOp =
                static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
            return m_builder->CreateSubgroupClusteredInclusive(groupArithOp, args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupClusteredExclusive:
        {
            Builder::GroupArithOp groupArithOp =
                static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
            return m_builder->CreateSubgroupClusteredExclusive(groupArithOp, args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadBroadcast:
        {
            return m_builder->CreateSubgroupQuadBroadcast(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadSwapHorizontal:
        {
            return m_builder->CreateSubgroupQuadSwapHorizontal(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadSwapVertical:
        {
            return m_builder->CreateSubgroupQuadSwapVertical(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupQuadSwapDiagonal:
        {
            return m_builder->CreateSubgroupQuadSwapDiagonal(args[0]);
        }
    case BuilderRecorder::Opcode::SubgroupSwizzleQuad:
        {
            return m_builder->CreateSubgroupSwizzleQuad(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupSwizzleMask:
        {
            return m_builder->CreateSubgroupSwizzleMask(args[0], args[1]);
        }
    case BuilderRecorder::Opcode::SubgroupWriteInvocation:
        {
            return m_builder->CreateSubgroupWriteInvocation(args[0], args[1], args[2]);
        }
    case BuilderRecorder::Opcode::SubgroupMbcnt:
        {
            return m_builder->CreateSubgroupMbcnt(args[0]);
        }
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(BuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)
