/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderReplayer.cpp
 * @brief LLPC source file: BuilderReplayer pass
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderReplayer.h"
#include "BuilderImpl.h"
#include "lgc/LgcContext.h"
#include "lgc/builder/BuilderRecorder.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-builder-replayer"

using namespace lgc;
using namespace llvm;

namespace {

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class LegacyBuilderReplayer final : public ModulePass, BuilderRecorderMetadataKinds {
public:
  LegacyBuilderReplayer() : ModulePass(ID), m_impl(nullptr) {}
  LegacyBuilderReplayer(Pipeline *pipeline);

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<LegacyPipelineStateWrapper>();
  }

  bool runOnModule(Module &module) override;

  static char ID;

private:
  LegacyBuilderReplayer(const LegacyBuilderReplayer &) = delete;
  LegacyBuilderReplayer &operator=(const LegacyBuilderReplayer &) = delete;

  BuilderReplayer m_impl;
};

} // namespace

char LegacyBuilderReplayer::ID = 0;

// =====================================================================================================================
// Create BuilderReplayer pass
//
// @param pipeline : Pipeline object
ModulePass *lgc::createLegacyBuilderReplayer(Pipeline *pipeline) {
  return new LegacyBuilderReplayer(pipeline);
}

// =====================================================================================================================
// Constructor
//
// @param pipeline : Pipeline object
BuilderReplayer::BuilderReplayer(Pipeline *pipeline)
    : BuilderRecorderMetadataKinds(static_cast<LLVMContext &>(pipeline->getContext())) {
}

// =====================================================================================================================
// Constructor
//
// @param pipeline : Pipeline object
LegacyBuilderReplayer::LegacyBuilderReplayer(Pipeline *pipeline) : ModulePass(ID), m_impl(pipeline) {
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyBuilderReplayer::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  return m_impl.runImpl(module, pipelineState);
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses BuilderReplayer::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  runImpl(module, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
//
// @param module : Module to run this pass on
// @returns : True if the module was modified by the transformation and false otherwise
bool BuilderReplayer::runImpl(Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Running the pass of replaying LLPC builder calls\n");

  // Set up the pipeline state from the specified linked IR module.
  pipelineState->readState(&module);

  // Create the BuilderImpl to replay into, passing it the PipelineState
  LgcContext *builderContext = pipelineState->getLgcContext();
  m_builder.reset(new BuilderImpl(builderContext, pipelineState));

  SmallVector<Function *, 8> funcsToRemove;

  for (auto &func : module) {
    // Skip non-declarations; they are definitely not lgc.create.* calls.
    if (!func.isDeclaration())
      continue;

    // Get the opcode if it is an lgc.create.* call, either from the metadata on the declaration, or
    // (in the case that there is no metadata because we are running the lgc command-line tool on the
    // output from "amdllpc -emit-lgc") from the name with string searching.
    unsigned opcode = 0;
    if (const MDNode *funcMeta = func.getMetadata(opcodeMetaKindId)) {
      const ConstantAsMetadata *metaConst = cast<ConstantAsMetadata>(funcMeta->getOperand(0));
      opcode = cast<ConstantInt>(metaConst->getValue())->getZExtValue();
      assert(func.getName().startswith(BuilderCallPrefix));
      assert(func.getName()
                 .slice(strlen(BuilderCallPrefix), StringRef::npos)
                 .startswith(BuilderRecorder::getCallName(static_cast<BuilderRecorder::Opcode>(opcode))) &&
             "lgc.create.* mismatch!");
    } else {
      if (!func.getName().startswith(BuilderCallPrefix))
        continue; // Not lgc.create.* call
      opcode = BuilderRecorder::getOpcodeFromName(func.getName());
    }

    // Replay all call uses of the function declaration.
    while (!func.use_empty()) {
      CallInst *call = cast<CallInst>(func.use_begin()->getUser());
      replayCall(opcode, call);
    }

    func.clearMetadata();
    assert(func.user_empty());
    funcsToRemove.push_back(&func);
  }

  for (Function *const func : funcsToRemove)
    func->eraseFromParent();

  return true;
}

// =====================================================================================================================
// Replay a recorded builder call.
//
// @param opcode : The builder call opcode
// @param call : The builder call to process
void BuilderReplayer::replayCall(unsigned opcode, CallInst *call) {
  // Change shader stage if necessary.
  Function *enclosingFunc = call->getParent()->getParent();
  if (enclosingFunc != m_enclosingFunc) {
    m_enclosingFunc = enclosingFunc;

    auto mapIt = m_shaderStageMap.find(enclosingFunc);
    ShaderStage stage = ShaderStageInvalid;
    if (mapIt == m_shaderStageMap.end()) {
      stage = getShaderStage(enclosingFunc);
      m_shaderStageMap[enclosingFunc] = stage;
    } else
      stage = mapIt->second;
    m_builder->setShaderStage(stage);
  }

  // Set the insert point on the Builder. Also sets debug location to that of call.
  m_builder->SetInsertPoint(call);

  // Process the builder call.
  LLVM_DEBUG(dbgs() << "Replaying " << *call << "\n");
  Value *newValue = processCall(opcode, call);

  // Replace uses of the call with the new value, take the name, remove the old call.
  if (newValue) {
    LLVM_DEBUG(dbgs() << "  replacing with: " << *newValue << "\n");
    call->replaceAllUsesWith(newValue);
    if (auto newInst = dyn_cast<Instruction>(newValue)) {
      if (call->getName() != "")
        newInst->takeName(call);
    }
  }
  call->eraseFromParent();
}

// =====================================================================================================================
// Process one recorder builder call.
// Returns the replacement value, or nullptr in the case that we do not want the caller to replace uses of
// call with the new value.
//
// @param opcode : The builder call opcode
// @param call : The builder call to process
Value *BuilderReplayer::processCall(unsigned opcode, CallInst *call) {
  // Set builder fast math flags from the recorded call.
  if (isa<FPMathOperator>(call))
    m_builder->setFastMathFlags(call->getFastMathFlags());
  else
    m_builder->clearFastMathFlags();

  // Get the args.
  auto args = ArrayRef<Use>(&call->getOperandList()[0], call->arg_size());

  switch (opcode) {
  case BuilderRecorder::Opcode::Nop:
  default: {
    llvm_unreachable("Should never be called!");
    return nullptr;
  }

  // Replayer implementation of ArithBuilder methods
  case BuilderRecorder::CubeFaceCoord: {
    return m_builder->CreateCubeFaceCoord(args[0]);
  }

  case BuilderRecorder::CubeFaceIndex: {
    return m_builder->CreateCubeFaceIndex(args[0]);
  }

  case BuilderRecorder::FpTruncWithRounding: {
    auto roundingMode = static_cast<RoundingMode>(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateFpTruncWithRounding(args[0], call->getType(), roundingMode);
  }

  case BuilderRecorder::QuantizeToFp16: {
    return m_builder->CreateQuantizeToFp16(args[0]);
  }

  case BuilderRecorder::SMod: {
    return m_builder->CreateSMod(args[0], args[1]);
  }

  case BuilderRecorder::FMod: {
    return m_builder->CreateFMod(args[0], args[1]);
  }

  case BuilderRecorder::Fma: {
    return m_builder->CreateFma(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Tan: {
    return m_builder->CreateTan(args[0]);
  }

  case BuilderRecorder::ASin: {
    return m_builder->CreateASin(args[0]);
  }

  case BuilderRecorder::ACos: {
    return m_builder->CreateACos(args[0]);
  }

  case BuilderRecorder::ATan: {
    return m_builder->CreateATan(args[0]);
  }

  case BuilderRecorder::ATan2: {
    return m_builder->CreateATan2(args[0], args[1]);
  }

  case BuilderRecorder::Sinh: {
    return m_builder->CreateSinh(args[0]);
  }

  case BuilderRecorder::Cosh: {
    return m_builder->CreateCosh(args[0]);
  }

  case BuilderRecorder::Tanh: {
    return m_builder->CreateTanh(args[0]);
  }

  case BuilderRecorder::ASinh: {
    return m_builder->CreateASinh(args[0]);
  }

  case BuilderRecorder::ACosh: {
    return m_builder->CreateACosh(args[0]);
  }

  case BuilderRecorder::ATanh: {
    return m_builder->CreateATanh(args[0]);
  }

  case BuilderRecorder::Power: {
    return m_builder->CreatePower(args[0], args[1]);
  }

  case BuilderRecorder::Exp: {
    return m_builder->CreateExp(args[0]);
  }

  case BuilderRecorder::Log: {
    return m_builder->CreateLog(args[0]);
  }

  case BuilderRecorder::Sqrt: {
    return m_builder->CreateSqrt(args[0]);
  }

  case BuilderRecorder::InverseSqrt: {
    return m_builder->CreateInverseSqrt(args[0]);
  }

  case BuilderRecorder::SAbs: {
    return m_builder->CreateSAbs(args[0]);
  }

  case BuilderRecorder::FSign: {
    return m_builder->CreateFSign(args[0]);
  }

  case BuilderRecorder::SSign: {
    return m_builder->CreateSSign(args[0]);
  }

  case BuilderRecorder::Fract: {
    return m_builder->CreateFract(args[0]);
  }

  case BuilderRecorder::SmoothStep: {
    return m_builder->CreateSmoothStep(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Ldexp: {
    return m_builder->CreateLdexp(args[0], args[1]);
  }

  case BuilderRecorder::ExtractSignificand: {
    return m_builder->CreateExtractSignificand(args[0]);
  }

  case BuilderRecorder::ExtractExponent: {
    return m_builder->CreateExtractExponent(args[0]);
  }

  case BuilderRecorder::CrossProduct: {
    return m_builder->CreateCrossProduct(args[0], args[1]);
  }

  case BuilderRecorder::NormalizeVector: {
    return m_builder->CreateNormalizeVector(args[0]);
  }

  case BuilderRecorder::FaceForward: {
    return m_builder->CreateFaceForward(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Reflect: {
    return m_builder->CreateReflect(args[0], args[1]);
  }

  case BuilderRecorder::Refract: {
    return m_builder->CreateRefract(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Opcode::Derivative: {
    return m_builder->CreateDerivative(args[0],                                     // inputValue
                                       cast<ConstantInt>(args[1])->getZExtValue(),  // isY
                                       cast<ConstantInt>(args[2])->getZExtValue()); // isFine
  }

  case BuilderRecorder::Opcode::FClamp: {
    return m_builder->CreateFClamp(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Opcode::FMin: {
    return m_builder->CreateFMin(args[0], args[1]);
  }

  case BuilderRecorder::Opcode::FMax: {
    return m_builder->CreateFMax(args[0], args[1]);
  }

  case BuilderRecorder::Opcode::FMin3: {
    return m_builder->CreateFMin3(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Opcode::FMax3: {
    return m_builder->CreateFMax3(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Opcode::FMid3: {
    return m_builder->CreateFMid3(args[0], args[1], args[2]);
  }

  case BuilderRecorder::Opcode::IsInf: {
    return m_builder->CreateIsInf(args[0]);
  }

  case BuilderRecorder::Opcode::IsNaN: {
    return m_builder->CreateIsNaN(args[0]);
  }

  case BuilderRecorder::Opcode::InsertBitField: {
    return m_builder->CreateInsertBitField(args[0], args[1], args[2], args[3]);
  }

  case BuilderRecorder::Opcode::ExtractBitField: {
    return m_builder->CreateExtractBitField(args[0], args[1], args[2], cast<ConstantInt>(args[3])->getZExtValue());
  }

  case BuilderRecorder::Opcode::FindSMsb: {
    return m_builder->CreateFindSMsb(args[0]);
  }

  case BuilderRecorder::Opcode::FMix: {
    return m_builder->createFMix(args[0], args[1], args[2]);
  }

  // Replayer implementations of DescBuilder methods
  case BuilderRecorder::Opcode::LoadBufferDesc: {
    assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(call->getType(), m_builder->getInt8Ty()));
    return m_builder->CreateLoadBufferDesc(cast<ConstantInt>(args[0])->getZExtValue(), // descSet
                                           cast<ConstantInt>(args[1])->getZExtValue(), // binding
                                           args[2],                                    // descIndex
                                           cast<ConstantInt>(args[3])->getZExtValue(), // flags
                                           m_builder->getInt8Ty());                    // pointeeTy
  }

  case BuilderRecorder::Opcode::GetDescStride:
    return m_builder->CreateGetDescStride(static_cast<ResourceNodeType>(cast<ConstantInt>(args[0])->getZExtValue()),
                                          static_cast<ResourceNodeType>(cast<ConstantInt>(args[1])->getZExtValue()),
                                          cast<ConstantInt>(args[2])->getZExtValue(),  // descSet
                                          cast<ConstantInt>(args[3])->getZExtValue()); // binding

  case BuilderRecorder::Opcode::GetDescPtr:
    return m_builder->CreateGetDescPtr(
        static_cast<ResourceNodeType>(cast<ConstantInt>(args[0])->getZExtValue()),
        static_cast<ResourceNodeType>(cast<ConstantInt>(args[1])->getZExtValue()), // abstractType
        cast<ConstantInt>(args[2])->getZExtValue(),                                // descSet
        cast<ConstantInt>(args[3])->getZExtValue()                                 // binding
    );

  case BuilderRecorder::Opcode::LoadPushConstantsPtr: {
    return m_builder->CreateLoadPushConstantsPtr(call->getType()); // returnTy
  }

  case BuilderRecorder::Opcode::GetBufferDescLength: {
    return m_builder->CreateGetBufferDescLength(args[0],  // buffer descriptor
                                                args[1]); // offset
  }

  case BuilderRecorder::Opcode::PtrDiff: {
    return m_builder->CreatePtrDiff(args[0]->getType(), // ty
                                    args[1],            // lhs
                                    args[2]);           // rhs
  }

  // Replayer implementations of ImageBuilder methods
  case BuilderRecorder::Opcode::ImageLoad: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *coord = args[3];
    Value *mipLevel = args.size() > 4 ? &*args[4] : nullptr;
    return m_builder->CreateImageLoad(call->getType(), dim, flags, imageDesc, coord, mipLevel);
  }

  case BuilderRecorder::Opcode::ImageLoadWithFmask: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *fmaskDesc = args[3];
    Value *coord = args[4];
    Value *sampleNum = args[5];
    return m_builder->CreateImageLoadWithFmask(call->getType(), dim, flags, imageDesc, fmaskDesc, coord, sampleNum);
  }

  case BuilderRecorder::Opcode::ImageStore: {
    Value *texel = args[0];
    unsigned dim = cast<ConstantInt>(args[1])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[2])->getZExtValue();
    Value *imageDesc = args[3];
    Value *coord = args[4];
    Value *mipLevel = args.size() > 5 ? &*args[5] : nullptr;
    return m_builder->CreateImageStore(texel, dim, flags, imageDesc, coord, mipLevel);
  }

  case BuilderRecorder::Opcode::ImageSample: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *samplerDesc = args[3];
    unsigned argsMask = cast<ConstantInt>(args[4])->getZExtValue();
    SmallVector<Value *, Builder::ImageAddressCount> address;
    address.resize(Builder::ImageAddressCount);
    args = args.slice(5);
    for (unsigned i = 0; i != Builder::ImageAddressCount; ++i) {
      if ((argsMask >> i) & 1) {
        address[i] = args[0];
        args = args.slice(1);
      }
    }
    return m_builder->CreateImageSample(call->getType(), dim, flags, imageDesc, samplerDesc, address);
  }

  case BuilderRecorder::Opcode::ImageSampleConvert: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDescArray = args[2];
    Value *samplerDesc = args[3];
    unsigned argsMask = cast<ConstantInt>(args[4])->getZExtValue();
    SmallVector<Value *, Builder::ImageAddressCount> address;
    address.resize(Builder::ImageAddressCount);
    args = args.slice(5);
    for (unsigned i = 0; i != Builder::ImageAddressCount; ++i) {
      if ((argsMask >> i) & 1) {
        address[i] = args[0];
        args = args.slice(1);
      }
    }
    return m_builder->CreateImageSampleConvert(call->getType(), dim, flags, imageDescArray, samplerDesc, address);
  }

  case BuilderRecorder::Opcode::ImageGather: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *samplerDesc = args[3];
    unsigned argsMask = cast<ConstantInt>(args[4])->getZExtValue();
    SmallVector<Value *, Builder::ImageAddressCount> address;
    address.resize(Builder::ImageAddressCount);
    args = args.slice(5);
    for (unsigned i = 0; i != Builder::ImageAddressCount; ++i) {
      if ((argsMask >> i) & 1) {
        address[i] = args[0];
        args = args.slice(1);
      }
    }
    return m_builder->CreateImageGather(call->getType(), dim, flags, imageDesc, samplerDesc, address);
  }

  case BuilderRecorder::Opcode::ImageAtomic: {
    unsigned atomicOp = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned dim = cast<ConstantInt>(args[1])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[2])->getZExtValue();
    auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[3])->getZExtValue());
    Value *imageDesc = args[4];
    Value *coord = args[5];
    Value *inputValue = args[6];
    return m_builder->CreateImageAtomic(atomicOp, dim, flags, ordering, imageDesc, coord, inputValue);
  }

  case BuilderRecorder::Opcode::ImageAtomicCompareSwap: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[2])->getZExtValue());
    Value *imageDesc = args[3];
    Value *coord = args[4];
    Value *inputValue = args[5];
    Value *comparatorValue = args[6];
    return m_builder->CreateImageAtomicCompareSwap(dim, flags, ordering, imageDesc, coord, inputValue, comparatorValue);
  }

  case BuilderRecorder::Opcode::ImageQueryLevels: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    return m_builder->CreateImageQueryLevels(dim, flags, imageDesc);
  }

  case BuilderRecorder::Opcode::ImageQuerySamples: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    return m_builder->CreateImageQuerySamples(dim, flags, imageDesc);
  }

  case BuilderRecorder::Opcode::ImageQuerySize: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *lod = args[3];
    return m_builder->CreateImageQuerySize(dim, flags, imageDesc, lod);
  }

  case BuilderRecorder::Opcode::ImageGetLod: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *samplerDesc = args[3];
    Value *coord = args[4];
    return m_builder->CreateImageGetLod(dim, flags, imageDesc, samplerDesc, coord);
  }

  // Replayer implementations of InOutBuilder methods
  case BuilderRecorder::Opcode::ReadGenericInput: {
    InOutInfo inputInfo(cast<ConstantInt>(args[4])->getZExtValue());
    return m_builder->CreateReadGenericInput(call->getType(),                                 // Result type
                                             cast<ConstantInt>(args[0])->getZExtValue(),      // Location
                                             args[1],                                         // Location offset
                                             isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Element index
                                             cast<ConstantInt>(args[3])->getZExtValue(),      // Location count
                                             inputInfo,                                       // Input info
                                             isa<UndefValue>(args[5]) ? nullptr : &*args[5]); // Vertex index
  }

  case BuilderRecorder::Opcode::ReadPerVertexInput: {
    InOutInfo inputInfo(cast<ConstantInt>(args[4])->getZExtValue());
    return m_builder->CreateReadPerVertexInput(call->getType(),                                // Result type
                                               cast<ConstantInt>(args[0])->getZExtValue(),     // Location
                                               args[1],                                        // Location offset
                                               isa<UndefValue>(args[2]) ? nullptr : &*args[2], // Element index
                                               cast<ConstantInt>(args[3])->getZExtValue(),     // Location count
                                               inputInfo,                                      // Input info
                                               args[5]);                                       // Vertex index
  }

  case BuilderRecorder::Opcode::ReadGenericOutput: {
    InOutInfo outputInfo(cast<ConstantInt>(args[4])->getZExtValue());
    return m_builder->CreateReadGenericOutput(call->getType(),                                 // Result type
                                              cast<ConstantInt>(args[0])->getZExtValue(),      // Location
                                              args[1],                                         // Location offset
                                              isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Element index
                                              cast<ConstantInt>(args[3])->getZExtValue(),      // Location count
                                              outputInfo,                                      // Output info
                                              isa<UndefValue>(args[5]) ? nullptr : &*args[5]); // Vertex index
  }

  case BuilderRecorder::Opcode::WriteGenericOutput: {
    InOutInfo outputInfo(cast<ConstantInt>(args[5])->getZExtValue());
    return m_builder->CreateWriteGenericOutput(args[0],                                         // Value to write
                                               cast<ConstantInt>(args[1])->getZExtValue(),      // Location
                                               args[2],                                         // Location offset
                                               isa<UndefValue>(args[3]) ? nullptr : &*args[3],  // Element index
                                               cast<ConstantInt>(args[4])->getZExtValue(),      // Location count
                                               outputInfo,                                      // Output info
                                               isa<UndefValue>(args[6]) ? nullptr : &*args[6]); // Vertex index
  }

  case BuilderRecorder::Opcode::WriteXfbOutput: {
    InOutInfo outputInfo(cast<ConstantInt>(args[6])->getZExtValue());
    return m_builder->CreateWriteXfbOutput(args[0],                                    // Value to write
                                           cast<ConstantInt>(args[1])->getZExtValue(), // IsBuiltIn
                                           cast<ConstantInt>(args[2])->getZExtValue(), // Location/builtIn
                                           cast<ConstantInt>(args[3])->getZExtValue(), // XFB buffer ID
                                           cast<ConstantInt>(args[4])->getZExtValue(), // XFB stride
                                           args[5],                                    // XFB byte offset
                                           outputInfo);
  }

  case BuilderRecorder::Opcode::ReadBaryCoord: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
    InOutInfo inputInfo(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateReadBaryCoord(builtIn,                                         // BuiltIn
                                          inputInfo,                                       // Input info
                                          isa<UndefValue>(args[2]) ? nullptr : &*args[2]); // auxInterpValue
  }

  case BuilderRecorder::Opcode::ReadBuiltInInput: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
    InOutInfo inputInfo(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateReadBuiltInInput(builtIn,                                         // BuiltIn
                                             inputInfo,                                       // Input info
                                             isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                             isa<UndefValue>(args[3]) ? nullptr : &*args[3]); // Index
  }

  case BuilderRecorder::Opcode::ReadBuiltInOutput: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
    InOutInfo outputInfo(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateReadBuiltInOutput(builtIn,                                         // BuiltIn
                                              outputInfo,                                      // Output info
                                              isa<UndefValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                              isa<UndefValue>(args[3]) ? nullptr : &*args[3]); // Index
  }

  case BuilderRecorder::Opcode::WriteBuiltInOutput: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[1])->getZExtValue());
    InOutInfo outputInfo(cast<ConstantInt>(args[2])->getZExtValue());
    return m_builder->CreateWriteBuiltInOutput(args[0],                                         // Val to write
                                               builtIn,                                         // BuiltIn
                                               outputInfo,                                      // Output info
                                               isa<UndefValue>(args[3]) ? nullptr : &*args[3],  // Vertex index
                                               isa<UndefValue>(args[4]) ? nullptr : &*args[4]); // Index
  }

#if VKI_RAY_TRACING
  case BuilderRecorder::Opcode::ImageBvhIntersectRayAMD: {
    Value *bvhNodePtr = args[0];
    Value *extent = args[1];
    Value *origin = args[2];
    Value *direction = args[3];
    Value *invDirection = args[4];
    Value *imageDesc = args[5];
    return m_builder->CreateImageBvhIntersectRay(bvhNodePtr, extent, origin, direction, invDirection, imageDesc);
  }

#endif
  case BuilderRecorder::Opcode::ReadTaskPayload: {
    return m_builder->CreateReadTaskPayload(call->getType(), // Result type
                                            args[0]);        // Byte offset within the payload structure
  }

  case BuilderRecorder::Opcode::WriteTaskPayload: {
    return m_builder->CreateWriteTaskPayload(args[0],  // Value to write
                                             args[1]); // Byte offset within the payload structure
  }

  case BuilderRecorder::Opcode::TaskPayloadAtomic: {
    unsigned atomicOp = cast<ConstantInt>(args[0])->getZExtValue();
    auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[1])->getZExtValue());
    Value *inputValue = args[2];
    Value *byteOffset = args[3];
    return m_builder->CreateTaskPayloadAtomic(atomicOp, ordering, inputValue, byteOffset);
  }

  case BuilderRecorder::Opcode::TaskPayloadAtomicCompareSwap: {
    auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[0])->getZExtValue());
    Value *inputValue = args[1];
    Value *comparatorValue = args[2];
    Value *byteOffset = args[3];
    return m_builder->CreateTaskPayloadAtomicCompareSwap(ordering, inputValue, comparatorValue, byteOffset);
  }

  // Replayer implementations of MiscBuilder methods
  case BuilderRecorder::Opcode::EmitVertex: {
    return m_builder->CreateEmitVertex(cast<ConstantInt>(args[0])->getZExtValue());
  }

  case BuilderRecorder::Opcode::EndPrimitive: {
    return m_builder->CreateEndPrimitive(cast<ConstantInt>(args[0])->getZExtValue());
  }

  case BuilderRecorder::Opcode::Barrier: {
    return m_builder->CreateBarrier();
  }

  case BuilderRecorder::Opcode::Kill: {
    return m_builder->CreateKill();
  }
  case BuilderRecorder::Opcode::ReadClock: {
    bool realtime = (cast<ConstantInt>(args[0])->getZExtValue() != 0);
    return m_builder->CreateReadClock(realtime);
  }
  case BuilderRecorder::Opcode::DemoteToHelperInvocation: {
    return m_builder->CreateDemoteToHelperInvocation();
  }
  case BuilderRecorder::Opcode::IsHelperInvocation: {
    return m_builder->CreateIsHelperInvocation();
  }
  case BuilderRecorder::Opcode::EmitMeshTasks: {
    return m_builder->CreateEmitMeshTasks(args[0], args[1], args[2]);
  }
  case BuilderRecorder::Opcode::SetMeshOutputs: {
    return m_builder->CreateSetMeshOutputs(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::TransposeMatrix: {
    return m_builder->CreateTransposeMatrix(args[0]);
  }
  case BuilderRecorder::Opcode::MatrixTimesScalar: {
    return m_builder->CreateMatrixTimesScalar(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::VectorTimesMatrix: {
    return m_builder->CreateVectorTimesMatrix(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::MatrixTimesVector: {
    return m_builder->CreateMatrixTimesVector(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::MatrixTimesMatrix: {
    return m_builder->CreateMatrixTimesMatrix(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::OuterProduct: {
    return m_builder->CreateOuterProduct(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::DotProduct: {
    return m_builder->CreateDotProduct(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::IntegerDotProduct: {
    Value *vector1 = args[0];
    Value *vector2 = args[1];
    Value *accumulator = args[2];
    unsigned flags = cast<ConstantInt>(args[3])->getZExtValue();
    return m_builder->CreateIntegerDotProduct(vector1, vector2, accumulator, flags);
  }
  case BuilderRecorder::Opcode::Determinant: {
    return m_builder->CreateDeterminant(args[0]);
  }
  case BuilderRecorder::Opcode::MatrixInverse: {
    return m_builder->CreateMatrixInverse(args[0]);
  }
  case BuilderRecorder::Opcode::GetWaveSize: {
    return m_builder->CreateGetWaveSize();
  }
  // Replayer implementations of SubgroupBuilder methods
  case BuilderRecorder::Opcode::GetSubgroupSize: {
    return m_builder->CreateGetSubgroupSize();
  }
  case BuilderRecorder::Opcode::SubgroupElect: {
    return m_builder->CreateSubgroupElect();
  }
  case BuilderRecorder::Opcode::SubgroupAll: {
    return m_builder->CreateSubgroupAll(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupAny: {
    return m_builder->CreateSubgroupAny(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupAllEqual: {
    return m_builder->CreateSubgroupAllEqual(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBroadcast: {
    return m_builder->CreateSubgroupBroadcast(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupBroadcastWaterfall: {
    return m_builder->CreateSubgroupBroadcastWaterfall(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupBroadcastFirst: {
    return m_builder->CreateSubgroupBroadcastFirst(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBallot: {
    return m_builder->CreateSubgroupBallot(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupInverseBallot: {
    return m_builder->CreateSubgroupInverseBallot(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBallotBitExtract: {
    return m_builder->CreateSubgroupBallotBitExtract(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupBallotBitCount: {
    return m_builder->CreateSubgroupBallotBitCount(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBallotInclusiveBitCount: {
    return m_builder->CreateSubgroupBallotInclusiveBitCount(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBallotExclusiveBitCount: {
    return m_builder->CreateSubgroupBallotExclusiveBitCount(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBallotFindLsb: {
    return m_builder->CreateSubgroupBallotFindLsb(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupBallotFindMsb: {
    return m_builder->CreateSubgroupBallotFindMsb(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupShuffle: {
    return m_builder->CreateSubgroupShuffle(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupShuffleXor: {
    return m_builder->CreateSubgroupShuffleXor(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupShuffleUp: {
    return m_builder->CreateSubgroupShuffleUp(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupShuffleDown: {
    return m_builder->CreateSubgroupShuffleDown(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupClusteredReduction: {
    Builder::GroupArithOp groupArithOp = static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
    return m_builder->CreateSubgroupClusteredReduction(groupArithOp, args[1], args[2]);
  }
  case BuilderRecorder::Opcode::SubgroupClusteredInclusive: {
    Builder::GroupArithOp groupArithOp = static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
    return m_builder->CreateSubgroupClusteredInclusive(groupArithOp, args[1], args[2]);
  }
  case BuilderRecorder::Opcode::SubgroupClusteredExclusive: {
    Builder::GroupArithOp groupArithOp = static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
    return m_builder->CreateSubgroupClusteredExclusive(groupArithOp, args[1], args[2]);
  }
  case BuilderRecorder::Opcode::SubgroupQuadBroadcast: {
    return m_builder->CreateSubgroupQuadBroadcast(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupQuadSwapHorizontal: {
    return m_builder->CreateSubgroupQuadSwapHorizontal(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupQuadSwapVertical: {
    return m_builder->CreateSubgroupQuadSwapVertical(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupQuadSwapDiagonal: {
    return m_builder->CreateSubgroupQuadSwapDiagonal(args[0]);
  }
  case BuilderRecorder::Opcode::SubgroupSwizzleQuad: {
    return m_builder->CreateSubgroupSwizzleQuad(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupSwizzleMask: {
    return m_builder->CreateSubgroupSwizzleMask(args[0], args[1]);
  }
  case BuilderRecorder::Opcode::SubgroupWriteInvocation: {
    return m_builder->CreateSubgroupWriteInvocation(args[0], args[1], args[2]);
  }
  case BuilderRecorder::Opcode::SubgroupMbcnt: {
    return m_builder->CreateSubgroupMbcnt(args[0]);
  }
  }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(LegacyBuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)
