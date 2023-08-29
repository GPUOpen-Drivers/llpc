/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "BuilderRecorder.h"
#include "lgc/LgcContext.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-builder-replayer"

using namespace lgc;
using namespace llvm;

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
  BuilderImpl builderImpl(pipelineState);
  m_builder = &builderImpl;

  SmallVector<Function *, 8> funcsToRemove;
  unsigned opcodeMetaKindId = module.getContext().getMDKindID(BuilderCallOpcodeMetadataName);

  for (auto &func : module) {
    // Skip non-declarations; they are definitely not lgc.create.* calls.
    if (!func.isDeclaration()) {
      if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 10) {
        // NOTE: The sub-attribute 'wavefrontsize' of 'target-features' is set in advance to let optimization
        // pass know we are in which wavesize mode.
        ShaderStage shaderStage = lgc::getShaderStage(&func);
        if (shaderStage != ShaderStageInvalid) {
          unsigned waveSize = pipelineState->getShaderWaveSize(shaderStage);
          func.addFnAttr("target-features", ",+wavefrontsize" + std::to_string(waveSize));
        }
      }

      continue;
    }

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
                 .startswith(BuilderRecorder::getCallName(static_cast<BuilderOpcode>(opcode))) &&
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
  m_builder = nullptr;

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
  case BuilderOpcode::Nop:
  default: {
    llvm_unreachable("Should never be called!");
    return nullptr;
  }

  // Replayer implementation of ArithBuilder methods
  case BuilderOpcode::CubeFaceCoord: {
    return m_builder->CreateCubeFaceCoord(args[0]);
  }

  case BuilderOpcode::CubeFaceIndex: {
    return m_builder->CreateCubeFaceIndex(args[0]);
  }

  case BuilderOpcode::FpTruncWithRounding: {
    auto roundingMode = static_cast<RoundingMode>(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateFpTruncWithRounding(args[0], call->getType(), roundingMode);
  }

  case BuilderOpcode::QuantizeToFp16: {
    return m_builder->CreateQuantizeToFp16(args[0]);
  }

  case BuilderOpcode::SMod: {
    return m_builder->CreateSMod(args[0], args[1]);
  }

  case BuilderOpcode::FMod: {
    return m_builder->CreateFMod(args[0], args[1]);
  }

  case BuilderOpcode::Fma: {
    return m_builder->CreateFma(args[0], args[1], args[2]);
  }

  case BuilderOpcode::Tan: {
    return m_builder->CreateTan(args[0]);
  }

  case BuilderOpcode::ASin: {
    return m_builder->CreateASin(args[0]);
  }

  case BuilderOpcode::ACos: {
    return m_builder->CreateACos(args[0]);
  }

  case BuilderOpcode::ATan: {
    return m_builder->CreateATan(args[0]);
  }

  case BuilderOpcode::ATan2: {
    return m_builder->CreateATan2(args[0], args[1]);
  }

  case BuilderOpcode::Sinh: {
    return m_builder->CreateSinh(args[0]);
  }

  case BuilderOpcode::Cosh: {
    return m_builder->CreateCosh(args[0]);
  }

  case BuilderOpcode::Tanh: {
    return m_builder->CreateTanh(args[0]);
  }

  case BuilderOpcode::ASinh: {
    return m_builder->CreateASinh(args[0]);
  }

  case BuilderOpcode::ACosh: {
    return m_builder->CreateACosh(args[0]);
  }

  case BuilderOpcode::ATanh: {
    return m_builder->CreateATanh(args[0]);
  }

  case BuilderOpcode::Power: {
    return m_builder->CreatePower(args[0], args[1]);
  }

  case BuilderOpcode::Exp: {
    return m_builder->CreateExp(args[0]);
  }

  case BuilderOpcode::Log: {
    return m_builder->CreateLog(args[0]);
  }

  case BuilderOpcode::Sqrt: {
    return m_builder->CreateSqrt(args[0]);
  }

  case BuilderOpcode::InverseSqrt: {
    return m_builder->CreateInverseSqrt(args[0]);
  }

  case BuilderOpcode::SAbs: {
    return m_builder->CreateSAbs(args[0]);
  }

  case BuilderOpcode::FSign: {
    return m_builder->CreateFSign(args[0]);
  }

  case BuilderOpcode::SSign: {
    return m_builder->CreateSSign(args[0]);
  }

  case BuilderOpcode::Fract: {
    return m_builder->CreateFract(args[0]);
  }

  case BuilderOpcode::SmoothStep: {
    return m_builder->CreateSmoothStep(args[0], args[1], args[2]);
  }

  case BuilderOpcode::Ldexp: {
    return m_builder->CreateLdexp(args[0], args[1]);
  }

  case BuilderOpcode::ExtractSignificand: {
    return m_builder->CreateExtractSignificand(args[0]);
  }

  case BuilderOpcode::ExtractExponent: {
    return m_builder->CreateExtractExponent(args[0]);
  }

  case BuilderOpcode::CrossProduct: {
    return m_builder->CreateCrossProduct(args[0], args[1]);
  }

  case BuilderOpcode::NormalizeVector: {
    return m_builder->CreateNormalizeVector(args[0]);
  }

  case BuilderOpcode::FaceForward: {
    return m_builder->CreateFaceForward(args[0], args[1], args[2]);
  }

  case BuilderOpcode::Reflect: {
    return m_builder->CreateReflect(args[0], args[1]);
  }

  case BuilderOpcode::Refract: {
    return m_builder->CreateRefract(args[0], args[1], args[2]);
  }

  case BuilderOpcode::Derivative: {
    return m_builder->CreateDerivative(args[0],                                     // inputValue
                                       cast<ConstantInt>(args[1])->getZExtValue(),  // isY
                                       cast<ConstantInt>(args[2])->getZExtValue()); // isFine
  }

  case BuilderOpcode::FClamp: {
    return m_builder->CreateFClamp(args[0], args[1], args[2]);
  }

  case BuilderOpcode::FMin: {
    return m_builder->CreateFMin(args[0], args[1]);
  }

  case BuilderOpcode::FMax: {
    return m_builder->CreateFMax(args[0], args[1]);
  }

  case BuilderOpcode::FMin3: {
    return m_builder->CreateFMin3(args[0], args[1], args[2]);
  }

  case BuilderOpcode::FMax3: {
    return m_builder->CreateFMax3(args[0], args[1], args[2]);
  }

  case BuilderOpcode::FMid3: {
    return m_builder->CreateFMid3(args[0], args[1], args[2]);
  }

  case BuilderOpcode::IsInf: {
    return m_builder->CreateIsInf(args[0]);
  }

  case BuilderOpcode::IsNaN: {
    return m_builder->CreateIsNaN(args[0]);
  }

  case BuilderOpcode::InsertBitField: {
    return m_builder->CreateInsertBitField(args[0], args[1], args[2], args[3]);
  }

  case BuilderOpcode::ExtractBitField: {
    return m_builder->CreateExtractBitField(args[0], args[1], args[2], cast<ConstantInt>(args[3])->getZExtValue());
  }

  case BuilderOpcode::FindSMsb: {
    return m_builder->CreateFindSMsb(args[0]);
  }

  case BuilderOpcode::CountLeadingSignBits: {
    return m_builder->CreateCountLeadingSignBits(args[0]);
  }

  case BuilderOpcode::FMix: {
    return m_builder->createFMix(args[0], args[1], args[2]);
  }

  // Replayer implementations of DescBuilder methods
  case BuilderOpcode::LoadBufferDesc: {
    return m_builder->CreateLoadBufferDesc(cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                                           cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                                           args[2],                                     // descIndex
                                           cast<ConstantInt>(args[3])->getZExtValue()); // flags
  }

  case BuilderOpcode::GetDescStride:
    return m_builder->CreateGetDescStride(static_cast<ResourceNodeType>(cast<ConstantInt>(args[0])->getZExtValue()),
                                          static_cast<ResourceNodeType>(cast<ConstantInt>(args[1])->getZExtValue()),
                                          cast<ConstantInt>(args[2])->getZExtValue(),  // descSet
                                          cast<ConstantInt>(args[3])->getZExtValue()); // binding

  case BuilderOpcode::GetDescPtr:
    return m_builder->CreateGetDescPtr(
        static_cast<ResourceNodeType>(cast<ConstantInt>(args[0])->getZExtValue()),
        static_cast<ResourceNodeType>(cast<ConstantInt>(args[1])->getZExtValue()), // abstractType
        cast<ConstantInt>(args[2])->getZExtValue(),                                // descSet
        cast<ConstantInt>(args[3])->getZExtValue()                                 // binding
    );

  case BuilderOpcode::LoadPushConstantsPtr: {
    return m_builder->CreateLoadPushConstantsPtr();
  }

  // Replayer implementations of ImageBuilder methods
  case BuilderOpcode::ImageLoad: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *coord = args[3];
    Value *mipLevel = args.size() > 4 ? &*args[4] : nullptr;
    return m_builder->CreateImageLoad(call->getType(), dim, flags, imageDesc, coord, mipLevel);
  }

  case BuilderOpcode::ImageLoadWithFmask: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *fmaskDesc = args[3];
    Value *coord = args[4];
    Value *sampleNum = args[5];
    return m_builder->CreateImageLoadWithFmask(call->getType(), dim, flags, imageDesc, fmaskDesc, coord, sampleNum);
  }

  case BuilderOpcode::ImageStore: {
    Value *texel = args[0];
    unsigned dim = cast<ConstantInt>(args[1])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[2])->getZExtValue();
    Value *imageDesc = args[3];
    Value *coord = args[4];
    Value *mipLevel = args.size() > 5 ? &*args[5] : nullptr;
    return m_builder->CreateImageStore(texel, dim, flags, imageDesc, coord, mipLevel);
  }

  case BuilderOpcode::ImageSample: {
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

  case BuilderOpcode::ImageSampleConvert: {
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

  case BuilderOpcode::ImageGather: {
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

  case BuilderOpcode::ImageAtomic: {
    unsigned atomicOp = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned dim = cast<ConstantInt>(args[1])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[2])->getZExtValue();
    auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[3])->getZExtValue());
    Value *imageDesc = args[4];
    Value *coord = args[5];
    Value *inputValue = args[6];
    return m_builder->CreateImageAtomic(atomicOp, dim, flags, ordering, imageDesc, coord, inputValue);
  }

  case BuilderOpcode::ImageAtomicCompareSwap: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    auto ordering = static_cast<AtomicOrdering>(cast<ConstantInt>(args[2])->getZExtValue());
    Value *imageDesc = args[3];
    Value *coord = args[4];
    Value *inputValue = args[5];
    Value *comparatorValue = args[6];
    return m_builder->CreateImageAtomicCompareSwap(dim, flags, ordering, imageDesc, coord, inputValue, comparatorValue);
  }

  case BuilderOpcode::ImageQueryLevels: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    return m_builder->CreateImageQueryLevels(dim, flags, imageDesc);
  }

  case BuilderOpcode::ImageQuerySamples: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    return m_builder->CreateImageQuerySamples(dim, flags, imageDesc);
  }

  case BuilderOpcode::ImageQuerySize: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *lod = args[3];
    return m_builder->CreateImageQuerySize(dim, flags, imageDesc, lod);
  }

  case BuilderOpcode::ImageGetLod: {
    unsigned dim = cast<ConstantInt>(args[0])->getZExtValue();
    unsigned flags = cast<ConstantInt>(args[1])->getZExtValue();
    Value *imageDesc = args[2];
    Value *samplerDesc = args[3];
    Value *coord = args[4];
    return m_builder->CreateImageGetLod(dim, flags, imageDesc, samplerDesc, coord);
  }

  // Replayer implementations of InOutBuilder methods
  case BuilderOpcode::ReadGenericInput: {
    InOutInfo inputInfo(cast<ConstantInt>(args[4])->getZExtValue());
    return m_builder->CreateReadGenericInput(call->getType(),                                  // Result type
                                             cast<ConstantInt>(args[0])->getZExtValue(),       // Location
                                             args[1],                                          // Location offset
                                             isa<PoisonValue>(args[2]) ? nullptr : &*args[2],  // Element index
                                             cast<ConstantInt>(args[3])->getZExtValue(),       // Location count
                                             inputInfo,                                        // Input info
                                             isa<PoisonValue>(args[5]) ? nullptr : &*args[5]); // Vertex index
  }

  case BuilderOpcode::ReadPerVertexInput: {
    InOutInfo inputInfo(cast<ConstantInt>(args[4])->getZExtValue());
    return m_builder->CreateReadPerVertexInput(call->getType(),                                 // Result type
                                               cast<ConstantInt>(args[0])->getZExtValue(),      // Location
                                               args[1],                                         // Location offset
                                               isa<PoisonValue>(args[2]) ? nullptr : &*args[2], // Element index
                                               cast<ConstantInt>(args[3])->getZExtValue(),      // Location count
                                               inputInfo,                                       // Input info
                                               args[5]);                                        // Vertex index
  }

  case BuilderOpcode::ReadGenericOutput: {
    InOutInfo outputInfo(cast<ConstantInt>(args[4])->getZExtValue());
    return m_builder->CreateReadGenericOutput(call->getType(),                                  // Result type
                                              cast<ConstantInt>(args[0])->getZExtValue(),       // Location
                                              args[1],                                          // Location offset
                                              isa<PoisonValue>(args[2]) ? nullptr : &*args[2],  // Element index
                                              cast<ConstantInt>(args[3])->getZExtValue(),       // Location count
                                              outputInfo,                                       // Output info
                                              isa<PoisonValue>(args[5]) ? nullptr : &*args[5]); // Vertex index
  }

  case BuilderOpcode::WriteGenericOutput: {
    InOutInfo outputInfo(cast<ConstantInt>(args[5])->getZExtValue());
    return m_builder->CreateWriteGenericOutput(args[0],                                          // Value to write
                                               cast<ConstantInt>(args[1])->getZExtValue(),       // Location
                                               args[2],                                          // Location offset
                                               isa<PoisonValue>(args[3]) ? nullptr : &*args[3],  // Element index
                                               cast<ConstantInt>(args[4])->getZExtValue(),       // Location count
                                               outputInfo,                                       // Output info
                                               isa<PoisonValue>(args[6]) ? nullptr : &*args[6]); // Vertex index
  }

  case BuilderOpcode::WriteXfbOutput: {
    InOutInfo outputInfo(cast<ConstantInt>(args[6])->getZExtValue());
    return m_builder->CreateWriteXfbOutput(args[0],                                    // Value to write
                                           cast<ConstantInt>(args[1])->getZExtValue(), // IsBuiltIn
                                           cast<ConstantInt>(args[2])->getZExtValue(), // Location/builtIn
                                           cast<ConstantInt>(args[3])->getZExtValue(), // XFB buffer ID
                                           cast<ConstantInt>(args[4])->getZExtValue(), // XFB stride
                                           args[5],                                    // XFB byte offset
                                           outputInfo);
  }

  case BuilderOpcode::ReadBaryCoord: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
    InOutInfo inputInfo(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateReadBaryCoord(builtIn,                                          // BuiltIn
                                          inputInfo,                                        // Input info
                                          isa<PoisonValue>(args[2]) ? nullptr : &*args[2]); // auxInterpValue
  }

  case BuilderOpcode::ReadBuiltInInput: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
    InOutInfo inputInfo(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateReadBuiltInInput(builtIn,                                          // BuiltIn
                                             inputInfo,                                        // Input info
                                             isa<PoisonValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                             isa<PoisonValue>(args[3]) ? nullptr : &*args[3]); // Index
  }

  case BuilderOpcode::ReadBuiltInOutput: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[0])->getZExtValue());
    InOutInfo outputInfo(cast<ConstantInt>(args[1])->getZExtValue());
    return m_builder->CreateReadBuiltInOutput(builtIn,                                          // BuiltIn
                                              outputInfo,                                       // Output info
                                              isa<PoisonValue>(args[2]) ? nullptr : &*args[2],  // Vertex index
                                              isa<PoisonValue>(args[3]) ? nullptr : &*args[3]); // Index
  }

  case BuilderOpcode::WriteBuiltInOutput: {
    auto builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(args[1])->getZExtValue());
    InOutInfo outputInfo(cast<ConstantInt>(args[2])->getZExtValue());
    return m_builder->CreateWriteBuiltInOutput(args[0],                                          // Val to write
                                               builtIn,                                          // BuiltIn
                                               outputInfo,                                       // Output info
                                               isa<PoisonValue>(args[3]) ? nullptr : &*args[3],  // Vertex index
                                               isa<PoisonValue>(args[4]) ? nullptr : &*args[4]); // Index
  }

  case BuilderOpcode::ImageBvhIntersectRay: {
    Value *bvhNodePtr = args[0];
    Value *extent = args[1];
    Value *origin = args[2];
    Value *direction = args[3];
    Value *invDirection = args[4];
    Value *imageDesc = args[5];
    return m_builder->CreateImageBvhIntersectRay(bvhNodePtr, extent, origin, direction, invDirection, imageDesc);
  }

  // Replayer implementations of MiscBuilder methods
  case BuilderOpcode::EmitVertex: {
    return m_builder->CreateEmitVertex(cast<ConstantInt>(args[0])->getZExtValue());
  }

  case BuilderOpcode::EndPrimitive: {
    return m_builder->CreateEndPrimitive(cast<ConstantInt>(args[0])->getZExtValue());
  }

  case BuilderOpcode::Barrier: {
    return m_builder->CreateBarrier();
  }

  case BuilderOpcode::Kill: {
    return m_builder->CreateKill();
  }
  case BuilderOpcode::ReadClock: {
    bool realtime = (cast<ConstantInt>(args[0])->getZExtValue() != 0);
    return m_builder->CreateReadClock(realtime);
  }
  case BuilderOpcode::DemoteToHelperInvocation: {
    return m_builder->CreateDemoteToHelperInvocation();
  }
  case BuilderOpcode::IsHelperInvocation: {
    return m_builder->CreateIsHelperInvocation();
  }
  case BuilderOpcode::DebugBreak: {
    return m_builder->CreateDebugBreak();
  }
  case BuilderOpcode::TransposeMatrix: {
    return m_builder->CreateTransposeMatrix(args[0]);
  }
  case BuilderOpcode::MatrixTimesScalar: {
    return m_builder->CreateMatrixTimesScalar(args[0], args[1]);
  }
  case BuilderOpcode::VectorTimesMatrix: {
    return m_builder->CreateVectorTimesMatrix(args[0], args[1]);
  }
  case BuilderOpcode::MatrixTimesVector: {
    return m_builder->CreateMatrixTimesVector(args[0], args[1]);
  }
  case BuilderOpcode::MatrixTimesMatrix: {
    return m_builder->CreateMatrixTimesMatrix(args[0], args[1]);
  }
  case BuilderOpcode::OuterProduct: {
    return m_builder->CreateOuterProduct(args[0], args[1]);
  }
  case BuilderOpcode::DotProduct: {
    return m_builder->CreateDotProduct(args[0], args[1]);
  }
  case BuilderOpcode::IntegerDotProduct: {
    Value *vector1 = args[0];
    Value *vector2 = args[1];
    Value *accumulator = args[2];
    unsigned flags = cast<ConstantInt>(args[3])->getZExtValue();
    return m_builder->CreateIntegerDotProduct(vector1, vector2, accumulator, flags);
  }
  case BuilderOpcode::Determinant: {
    return m_builder->CreateDeterminant(args[0]);
  }
  case BuilderOpcode::MatrixInverse: {
    return m_builder->CreateMatrixInverse(args[0]);
  }
  case BuilderOpcode::GetWaveSize: {
    return m_builder->CreateGetWaveSize();
  }
  // Replayer implementations of SubgroupBuilder methods
  case BuilderOpcode::GetSubgroupSize: {
    return m_builder->CreateGetSubgroupSize();
  }
  case BuilderOpcode::SubgroupElect: {
    return m_builder->CreateSubgroupElect();
  }
  case BuilderOpcode::SubgroupAll: {
    return m_builder->CreateSubgroupAll(args[0]);
  }
  case BuilderOpcode::SubgroupAny: {
    return m_builder->CreateSubgroupAny(args[0]);
  }
  case BuilderOpcode::SubgroupAllEqual: {
    return m_builder->CreateSubgroupAllEqual(args[0]);
  }
  case BuilderOpcode::SubgroupRotate: {
    return m_builder->CreateSubgroupRotate(args[0], args[1], isa<PoisonValue>(args[2]) ? nullptr : &*args[2]);
  }
  case BuilderOpcode::SubgroupBroadcast: {
    return m_builder->CreateSubgroupBroadcast(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupBroadcastWaterfall: {
    return m_builder->CreateSubgroupBroadcastWaterfall(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupBroadcastFirst: {
    return m_builder->CreateSubgroupBroadcastFirst(args[0]);
  }
  case BuilderOpcode::SubgroupBallot: {
    return m_builder->CreateSubgroupBallot(args[0]);
  }
  case BuilderOpcode::SubgroupInverseBallot: {
    return m_builder->CreateSubgroupInverseBallot(args[0]);
  }
  case BuilderOpcode::SubgroupBallotBitExtract: {
    return m_builder->CreateSubgroupBallotBitExtract(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupBallotBitCount: {
    return m_builder->CreateSubgroupBallotBitCount(args[0]);
  }
  case BuilderOpcode::SubgroupBallotInclusiveBitCount: {
    return m_builder->CreateSubgroupBallotInclusiveBitCount(args[0]);
  }
  case BuilderOpcode::SubgroupBallotExclusiveBitCount: {
    return m_builder->CreateSubgroupBallotExclusiveBitCount(args[0]);
  }
  case BuilderOpcode::SubgroupBallotFindLsb: {
    return m_builder->CreateSubgroupBallotFindLsb(args[0]);
  }
  case BuilderOpcode::SubgroupBallotFindMsb: {
    return m_builder->CreateSubgroupBallotFindMsb(args[0]);
  }
  case BuilderOpcode::SubgroupShuffle: {
    return m_builder->CreateSubgroupShuffle(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupShuffleXor: {
    return m_builder->CreateSubgroupShuffleXor(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupShuffleUp: {
    return m_builder->CreateSubgroupShuffleUp(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupShuffleDown: {
    return m_builder->CreateSubgroupShuffleDown(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupClusteredReduction: {
    Builder::GroupArithOp groupArithOp = static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
    return m_builder->CreateSubgroupClusteredReduction(groupArithOp, args[1], args[2]);
  }
  case BuilderOpcode::SubgroupClusteredInclusive: {
    Builder::GroupArithOp groupArithOp = static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
    return m_builder->CreateSubgroupClusteredInclusive(groupArithOp, args[1], args[2]);
  }
  case BuilderOpcode::SubgroupClusteredExclusive: {
    Builder::GroupArithOp groupArithOp = static_cast<Builder::GroupArithOp>(cast<ConstantInt>(args[0])->getZExtValue());
    return m_builder->CreateSubgroupClusteredExclusive(groupArithOp, args[1], args[2]);
  }
  case BuilderOpcode::SubgroupQuadBroadcast: {
    return m_builder->CreateSubgroupQuadBroadcast(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupQuadSwapHorizontal: {
    return m_builder->CreateSubgroupQuadSwapHorizontal(args[0]);
  }
  case BuilderOpcode::SubgroupQuadSwapVertical: {
    return m_builder->CreateSubgroupQuadSwapVertical(args[0]);
  }
  case BuilderOpcode::SubgroupQuadSwapDiagonal: {
    return m_builder->CreateSubgroupQuadSwapDiagonal(args[0]);
  }
  case BuilderOpcode::SubgroupSwizzleQuad: {
    return m_builder->CreateSubgroupSwizzleQuad(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupSwizzleMask: {
    return m_builder->CreateSubgroupSwizzleMask(args[0], args[1]);
  }
  case BuilderOpcode::SubgroupWriteInvocation: {
    return m_builder->CreateSubgroupWriteInvocation(args[0], args[1], args[2]);
  }
  case BuilderOpcode::SubgroupMbcnt: {
    return m_builder->CreateSubgroupMbcnt(args[0]);
  }
  }
}
