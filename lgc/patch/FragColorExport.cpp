/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  FragColorExport.cpp
 * @brief LLPC source file: contains implementation of class lgc::FragColorExport.
 ***********************************************************************************************************************
 */
#include "lgc/patch/FragColorExport.h"
#include "lgc/BuilderBase.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/ResourceUsage.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-frag-color-export"

using namespace lgc;
using namespace llvm;

namespace {

// The information needed for an export to a hardware color target.
struct ColorExportValueInfo {
  std::vector<Value *> value; // The value of each component to be exported.
  unsigned location;          // The location that corresponds to the hardware color target.
  bool isSigned;              // True if the values should be interpreted as signed integers.
};

// =====================================================================================================================
// Pass to lower color export calls
class LowerFragColorExport : public ModulePass {
public:
  LowerFragColorExport() : ModulePass(ID) {}
  LowerFragColorExport(const LowerFragColorExport &) = delete;
  LowerFragColorExport &operator=(const LowerFragColorExport &) = delete;

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
  }

  virtual bool runOnModule(Module &module) override;

  static char ID; // ID of this pass

private:
  void updateFragColors(CallInst *callInst, ColorExportValueInfo expFragColors[], BuilderBase &builder);
  Value *getOutputValue(ArrayRef<Value *> expFragColor, unsigned int location, BuilderBase &builder);
  CallInst *addExportForGenericOutputs(Function *fragEntryPoint, BuilderBase &builder);
  CallInst *addExportForBuiltinOutput(Function *module, BuilderBase &builder);
  CallInst *addDummyExport(BuilderBase &builder);
  void setDoneFlag(CallInst *exportInst, BuilderBase &builder);

  LLVMContext *m_context;         // The context the pass is being run in.
  PipelineState *m_pipelineState; // The pipeline state
  ResourceUsage *m_resUsage;      // The resource usage object from the pipeline state.
};

} // namespace

namespace lgc {

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
// @param module : LLVM module
FragColorExport::FragColorExport(LLVMContext *context) : m_context(context) {
}

// =====================================================================================================================
// Executes fragment color export operations based on the specified output type and its location.
//
// @param output : Fragment color output
// @param hwColorTarget : The render target (MRT) of fragment color output
// @param insertPos : Where to insert fragment color export instructions
// @param expFmt: The format for the given render target
// @param signedness: If output should be interpreted as a signed integer
Value *FragColorExport::run(Value *output, unsigned hwColorTarget, Instruction *insertPos, ExportFormat expFmt,
                            const bool signedness) {
  Type *outputTy = output->getType();
  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  auto compTy = outputTy->isVectorTy() ? cast<VectorType>(outputTy)->getElementType() : outputTy;
  unsigned compCount = outputTy->isVectorTy() ? cast<VectorType>(outputTy)->getNumElements() : 1;

  Value *comps[4] = {nullptr};
  if (compCount == 1)
    comps[0] = output;
  else {
    for (unsigned i = 0; i < compCount; ++i) {
      comps[i] = ExtractElementInst::Create(output, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }
  }

  bool comprExp = false;
  bool needPack = false;

  const auto undefFloat = UndefValue::get(Type::getFloatTy(*m_context));
  const auto undefFloat16 = UndefValue::get(Type::getHalfTy(*m_context));
  const auto undefFloat16x2 = UndefValue::get(FixedVectorType::get(Type::getHalfTy(*m_context), 2));

  switch (expFmt) {
  case EXP_FORMAT_ZERO: {
    break;
  }
  case EXP_FORMAT_32_R: {
    compCount = 1;
    comps[0] = convertToFloat(comps[0], signedness, insertPos);
    comps[1] = undefFloat;
    comps[2] = undefFloat;
    comps[3] = undefFloat;
    break;
  }
  case EXP_FORMAT_32_GR: {
    if (compCount >= 2) {
      compCount = 2;
      comps[0] = convertToFloat(comps[0], signedness, insertPos);
      comps[1] = convertToFloat(comps[1], signedness, insertPos);
      comps[2] = undefFloat;
      comps[3] = undefFloat;
    } else {
      compCount = 1;
      comps[0] = convertToFloat(comps[0], signedness, insertPos);
      comps[1] = undefFloat;
      comps[2] = undefFloat;
      comps[3] = undefFloat;
    }
    break;
  }
  case EXP_FORMAT_32_AR: {
    if (compCount == 4) {
      compCount = 2;
      comps[0] = convertToFloat(comps[0], signedness, insertPos);
      comps[1] = convertToFloat(comps[3], signedness, insertPos);
      comps[2] = undefFloat;
      comps[3] = undefFloat;
    } else {
      compCount = 1;
      comps[0] = convertToFloat(comps[0], signedness, insertPos);
      comps[1] = undefFloat;
      comps[2] = undefFloat;
      comps[3] = undefFloat;
    }
    break;
  }
  case EXP_FORMAT_32_ABGR: {
    for (unsigned i = 0; i < compCount; ++i)
      comps[i] = convertToFloat(comps[i], signedness, insertPos);

    for (unsigned i = compCount; i < 4; ++i)
      comps[i] = undefFloat;
    break;
  }
  case EXP_FORMAT_FP16_ABGR: {
    comprExp = true;

    if (bitWidth == 8) {
      needPack = true;

      // Cast i8 to float16
      assert(compTy->isIntegerTy());
      for (unsigned i = 0; i < compCount; ++i) {
        if (signedness) {
          // %comp = sext i8 %comp to i16
          comps[i] = new SExtInst(comps[i], Type::getInt16Ty(*m_context), "", insertPos);
        } else {
          // %comp = zext i8 %comp to i16
          comps[i] = new ZExtInst(comps[i], Type::getInt16Ty(*m_context), "", insertPos);
        }

        // %comp = bitcast i16 %comp to half
        comps[i] = new BitCastInst(comps[i], Type::getHalfTy(*m_context), "", insertPos);
      }

      for (unsigned i = compCount; i < 4; ++i)
        comps[i] = undefFloat16;
    } else if (bitWidth == 16) {
      needPack = true;

      if (compTy->isIntegerTy()) {
        // Cast i16 to float16
        for (unsigned i = 0; i < compCount; ++i) {
          // %comp = bitcast i16 %comp to half
          comps[i] = new BitCastInst(comps[i], Type::getHalfTy(*m_context), "", insertPos);
        }
      }

      for (unsigned i = compCount; i < 4; ++i)
        comps[i] = undefFloat16;
    } else {
      if (compTy->isIntegerTy()) {
        // Cast i32 to float
        for (unsigned i = 0; i < compCount; ++i) {
          // %comp = bitcast i32 %comp to float
          comps[i] = new BitCastInst(comps[i], Type::getFloatTy(*m_context), "", insertPos);
        }
      }

      for (unsigned i = compCount; i < 4; ++i)
        comps[i] = undefFloat;

      Attribute::AttrKind attribs[] = {Attribute::ReadNone};

      // Do packing
      comps[0] = emitCall("llvm.amdgcn.cvt.pkrtz", FixedVectorType::get(Type::getHalfTy(*m_context), 2),
                          {comps[0], comps[1]}, attribs, insertPos);

      if (compCount > 2) {
        comps[1] = emitCall("llvm.amdgcn.cvt.pkrtz", FixedVectorType::get(Type::getHalfTy(*m_context), 2),
                            {comps[2], comps[3]}, attribs, insertPos);
      } else
        comps[1] = undefFloat16x2;
    }

    break;
  }
  case EXP_FORMAT_UNORM16_ABGR:
  case EXP_FORMAT_SNORM16_ABGR: {
    comprExp = true;
    needPack = true;

    for (unsigned i = 0; i < compCount; ++i) {
      // Convert the components to float value if necessary
      comps[i] = convertToFloat(comps[i], signedness, insertPos);
    }

    assert(compCount <= 4);
    // Make even number of components;
    if ((compCount % 2) != 0) {
      comps[compCount] = ConstantFP::get(Type::getFloatTy(*m_context), 0.0);
      compCount++;
    }

    StringRef funcName =
        expFmt == EXP_FORMAT_SNORM16_ABGR ? "llvm.amdgcn.cvt.pknorm.i16" : "llvm.amdgcn.cvt.pknorm.u16";

    for (unsigned i = 0; i < compCount; i += 2) {
      Value *packedComps = emitCall(funcName, FixedVectorType::get(Type::getInt16Ty(*m_context), 2),
                                    {comps[i], comps[i + 1]}, {}, insertPos);

      packedComps = new BitCastInst(packedComps, FixedVectorType::get(Type::getHalfTy(*m_context), 2), "", insertPos);

      comps[i] =
          ExtractElementInst::Create(packedComps, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);

      comps[i + 1] =
          ExtractElementInst::Create(packedComps, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
    }

    for (unsigned i = compCount; i < 4; ++i)
      comps[i] = undefFloat16;

    break;
  }
  case EXP_FORMAT_UINT16_ABGR:
  case EXP_FORMAT_SINT16_ABGR: {
    comprExp = true;
    needPack = true;

    for (unsigned i = 0; i < compCount; ++i) {
      // Convert the components to int value if necessary
      comps[i] = convertToInt(comps[i], signedness, insertPos);
    }

    assert(compCount <= 4);
    // Make even number of components;
    if ((compCount % 2) != 0) {
      comps[compCount] = ConstantInt::get(Type::getInt32Ty(*m_context), 0), compCount++;
    }

    StringRef funcName = expFmt == EXP_FORMAT_SINT16_ABGR ? "llvm.amdgcn.cvt.pk.i16" : "llvm.amdgcn.cvt.pk.u16";

    for (unsigned i = 0; i < compCount; i += 2) {
      Value *packedComps = emitCall(funcName, FixedVectorType::get(Type::getInt16Ty(*m_context), 2),
                                    {comps[i], comps[i + 1]}, {}, insertPos);

      packedComps = new BitCastInst(packedComps, FixedVectorType::get(Type::getHalfTy(*m_context), 2), "", insertPos);

      comps[i] =
          ExtractElementInst::Create(packedComps, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);

      comps[i + 1] =
          ExtractElementInst::Create(packedComps, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
    }

    for (unsigned i = compCount; i < 4; ++i)
      comps[i] = undefFloat16;

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  Value *exportCall = nullptr;

  if (expFmt == EXP_FORMAT_ZERO) {
    // Do nothing
  } else if (comprExp) {
    // 16-bit export (compressed)
    if (needPack) {
      // Do packing

      // %comp[0] = insertelement <2 x half> undef, half %comp[0], i32 0
      comps[0] = InsertElementInst::Create(undefFloat16x2, comps[0], ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                           "", insertPos);

      // %comp[0] = insertelement <2 x half> %comp[0], half %comp[1], i32 1
      comps[0] = InsertElementInst::Create(comps[0], comps[1], ConstantInt::get(Type::getInt32Ty(*m_context), 1), "",
                                           insertPos);

      if (compCount > 2) {
        // %comp[1] = insertelement <2 x half> undef, half %comp[2], i32 0
        comps[1] = InsertElementInst::Create(undefFloat16x2, comps[2],
                                             ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);

        // %comp[1] = insertelement <2 x half> %comp[1], half %comp[3], i32 1
        comps[1] = InsertElementInst::Create(comps[1], comps[3], ConstantInt::get(Type::getInt32Ty(*m_context), 1), "",
                                             insertPos);
      } else
        comps[1] = undefFloat16x2;
    }

    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_MRT_0 + hwColorTarget), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), compCount > 2 ? 0xF : 0x3),        // en
        comps[0],                                                                         // src0
        comps[1],                                                                         // src1
        ConstantInt::get(Type::getInt1Ty(*m_context), false),                             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), true)                               // vm
    };

    exportCall = emitCall("llvm.amdgcn.exp.compr.v2f16", Type::getVoidTy(*m_context), args, {}, insertPos);
  } else {
    // 32-bit export
    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_MRT_0 + hwColorTarget), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), (1 << compCount) - 1),             // en
        comps[0],                                                                         // src0
        comps[1],                                                                         // src1
        comps[2],                                                                         // src2
        comps[3],                                                                         // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),                             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), true)                               // vm
    };

    exportCall = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
  }

  return exportCall;
}

// =====================================================================================================================
// Converts an output component value to its floating-point representation. This function is a "helper" in computing
// the export value based on shader export format.
//
// @param value : Output component value
// @param signedness : Whether the type is signed (valid for integer type)
// @param insertPos : Where to insert conversion instructions
Value *FragColorExport::convertToFloat(Value *value, bool signedness, Instruction *insertPos) const {
  Type *valueTy = value->getType();
  assert(valueTy->isFloatingPointTy() || valueTy->isIntegerTy()); // Should be floating-point/integer scalar

  const unsigned bitWidth = valueTy->getScalarSizeInBits();
  if (bitWidth == 8) {
    assert(valueTy->isIntegerTy());
    if (signedness) {
      // %value = sext i8 %value to i32
      value = new SExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    } else {
      // %value = zext i8 %value to i32
      value = new ZExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    }

    // %value = bitcast i32 %value to float
    value = new BitCastInst(value, Type::getFloatTy(*m_context), "", insertPos);
  } else if (bitWidth == 16) {
    if (valueTy->isFloatingPointTy()) {
      // %value = fpext half %value to float
      value = new FPExtInst(value, Type::getFloatTy(*m_context), "", insertPos);
    } else {
      if (signedness) {
        // %value = sext i16 %value to i32
        value = new SExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
      } else {
        // %value = zext i16 %value to i32
        value = new ZExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
      }

      // %value = bitcast i32 %value to float
      value = new BitCastInst(value, Type::getFloatTy(*m_context), "", insertPos);
    }
  } else {
    assert(bitWidth == 32); // The valid bit width is 16 or 32
    if (valueTy->isIntegerTy()) {
      // %value = bitcast i32 %value to float
      value = new BitCastInst(value, Type::getFloatTy(*m_context), "", insertPos);
    }
  }

  return value;
}

// =====================================================================================================================
// Converts an output component value to its integer representation. This function is a "helper" in computing the
// export value based on shader export format.
//
// @param value : Output component value
// @param signedness : Whether the type is signed (valid for integer type)
// @param insertPos : Where to insert conversion instructions
Value *FragColorExport::convertToInt(Value *value, bool signedness, Instruction *insertPos) const {
  Type *valueTy = value->getType();
  assert(valueTy->isFloatingPointTy() || valueTy->isIntegerTy()); // Should be floating-point/integer scalar

  const unsigned bitWidth = valueTy->getScalarSizeInBits();
  if (bitWidth == 8) {
    assert(valueTy->isIntegerTy());

    if (signedness) {
      // %value = sext i8 %value to i32
      value = new SExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    } else {
      // %value = zext i8 %value to i32
      value = new ZExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    }
  } else if (bitWidth == 16) {
    if (valueTy->isFloatingPointTy()) {
      // %value = bicast half %value to i16
      value = new BitCastInst(value, Type::getInt16Ty(*m_context), "", insertPos);
    }

    if (signedness) {
      // %value = sext i16 %value to i32
      value = new SExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    } else {
      // %value = zext i16 %value to i32
      value = new ZExtInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    }
  } else {
    assert(bitWidth == 32); // The valid bit width is 16 or 32
    if (valueTy->isFloatingPointTy()) {
      // %value = bitcast float %value to i32
      value = new BitCastInst(value, Type::getInt32Ty(*m_context), "", insertPos);
    }
  }

  return value;
}

} // namespace lgc

char LowerFragColorExport::ID = 0;

// =====================================================================================================================
// Create the color export pass
ModulePass *lgc::createLowerFragColorExport() {
  return new LowerFragColorExport();
}

// =====================================================================================================================
// Run the lower color export pass on a module
//
// @param [in/out] module : Module
bool LowerFragColorExport::runOnModule(Module &module) {
  m_context = &module.getContext();
  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  m_resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);

  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  Function *fragEntryPoint = pipelineShaders->getEntryPoint(ShaderStageFragment);
  if (!fragEntryPoint)
    return false;

  auto retInst = fragEntryPoint->back().getTerminator();
  BuilderBase builder(module.getContext());
  builder.SetInsertPoint(retInst);

  CallInst *lastExport = nullptr;
  lastExport = addExportForBuiltinOutput(fragEntryPoint, builder);
  CallInst *lastGenericExport = addExportForGenericOutputs(fragEntryPoint, builder);
  if (lastGenericExport != nullptr) {
    lastExport = lastGenericExport;
  }

  m_resUsage->inOutUsage.fs.dummyExport =
      (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 10 || m_resUsage->builtInUsage.fs.discard);
  if (!lastExport && m_resUsage->inOutUsage.fs.dummyExport) {
    lastExport = addDummyExport(builder);
  }

  setDoneFlag(lastExport, builder);

  return lastExport != nullptr;
}

// =====================================================================================================================
// Updates the value in the entry in expFragColors that callInst is writing to.
//
// @param callInst : An call to the generic output export builtin in a fragment shader.
// @param [in/out] expFragColors : An array with the current color export information for each hw color target.
void LowerFragColorExport::updateFragColors(CallInst *callInst, ColorExportValueInfo expFragColors[],
                                            BuilderBase &builder) {
  unsigned location = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
  const unsigned compIdx = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue();
  Value *output = callInst->getOperand(2);

  auto it = m_resUsage->inOutUsage.outputLocMap.find(location);
  if (it == m_resUsage->inOutUsage.outputLocMap.end())
    return;
  unsigned hwColorTarget = it->second;

  Type *outputTy = output->getType();

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);
  (void(bitWidth)); // unused

  auto compTy = outputTy->isVectorTy() ? cast<VectorType>(outputTy)->getElementType() : outputTy;
  unsigned compCount = outputTy->isVectorTy() ? cast<VectorType>(outputTy)->getNumElements() : 1;

  std::vector<Value *> outputComps;
  for (unsigned i = 0; i < compCount; ++i) {
    Value *outputComp = nullptr;
    if (compCount == 1)
      outputComp = output;
    else {
      outputComp = builder.CreateExtractElement(output, i);
    }
    outputComps.push_back(outputComp);
  }

  assert(hwColorTarget < MaxColorTargets);
  auto &expFragColor = expFragColors[hwColorTarget];

  while (compIdx + compCount > expFragColor.value.size())
    expFragColor.value.push_back(UndefValue::get(compTy));

  for (unsigned i = 0; i < compCount; ++i)
    expFragColor.value[compIdx + i] = outputComps[i];

  expFragColor.location = location;
  BasicType outputType = m_resUsage->inOutUsage.fs.outputTypes[location];
  expFragColor.isSigned =
      (outputType == BasicType::Int8 || outputType == BasicType::Int16 || outputType == BasicType::Int);
}

// =====================================================================================================================
// Returns a value that is a combination of the values in expFragColor into a single value.  Returns a nullptr if no
// value needs to be exported.
//
// @param expFragColor : The array of values that will be exported in each component.
// @param location : The location of this color export.
// @param builder : The builder object that will be used to create new instructions.
Value *LowerFragColorExport::getOutputValue(ArrayRef<Value *> expFragColor, unsigned int location,
                                            BuilderBase &builder) {
  if (expFragColor.empty())
    return nullptr;

  Value *output = nullptr;
  unsigned compCount = expFragColor.size();
  assert(compCount <= 4);

  // Set CB shader mask
  // TODO: This will be moved in a later PR.
  const unsigned channelMask = ((1 << compCount) - 1);
  m_resUsage->inOutUsage.fs.cbShaderMask |= (channelMask << (4 * location));

  // Construct exported fragment colors
  if (compCount == 1)
    output = expFragColor[0];
  else {
    const auto compTy = expFragColor[0]->getType();

    output = UndefValue::get(FixedVectorType::get(compTy, compCount));
    for (unsigned i = 0; i < compCount; ++i) {
      assert(expFragColor[i]->getType() == compTy);
      output = builder.CreateInsertElement(output, expFragColor[i], i);
    }
  }
  return output;
}

// =====================================================================================================================
// Generates the export instructions for all of the generic output of the fragment shader fragEntryPoint.  Returns the
// last export instruction that was generated.  Returns nullptr if no export instructions were generated.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
CallInst *LowerFragColorExport::addExportForGenericOutputs(Function *fragEntryPoint, BuilderBase &builder) {
  std::unique_ptr<FragColorExport> fragColorExport(new FragColorExport(m_context));
  SmallVector<CallInst *, 8> colorExports;

  // Collect all of the exports in the fragment shader
  for (auto &func : *fragEntryPoint->getParent()) {
    if (!func.isDeclaration() || !func.getName().startswith(lgcName::OutputExportGeneric))
      continue;
    for (auto user : func.users()) {
      auto callInst = cast<CallInst>(user);
      if (callInst->getFunction() == fragEntryPoint)
        colorExports.push_back(callInst);
    }
  }

  if (colorExports.empty()) {
    SmallVector<ColorExportInfo, 8> info;
    const auto &builtInUsage = m_resUsage->builtInUsage.fs;
    bool hasDepthExpFmtZero = !(builtInUsage.sampleMask || builtInUsage.fragStencilRef || builtInUsage.fragDepth);
    m_pipelineState->getPalMetadata()->updateSpiShaderColFormat(info, hasDepthExpFmtZero, builtInUsage.discard);
    return nullptr;
  }

  // Collect all of the values that need to be exported for each hardware color target.
  auto originalInsPos = builder.GetInsertPoint();
  ColorExportValueInfo expFragColors[MaxColorTargets] = {};
  for (CallInst *callInst : colorExports) {
    builder.SetInsertPoint(callInst);
    updateFragColors(callInst, expFragColors, builder);
    callInst->eraseFromParent();
  }

  // This insertion point should be the return instruction, so we know we can dereference the iterator.
  builder.SetInsertPoint(&*originalInsPos);

  Value *exportValues[MaxColorTargets] = {};

  // Recombine the values being exported for each hw color target.
  for (unsigned hwColorTarget = 0; hwColorTarget < MaxColorTargets; ++hwColorTarget) {
    unsigned location = m_resUsage->inOutUsage.fs.outputOrigLocs[hwColorTarget];
    if (location == InvalidValue)
      exportValues[hwColorTarget] = nullptr;
    else
      exportValues[hwColorTarget] = getOutputValue(expFragColors[hwColorTarget].value, location, builder);
  }

  // Add the color export information to the palmetadata.
  SmallVector<ColorExportInfo, 8> info;
  for (unsigned hwColorTarget = 0; hwColorTarget < MaxColorTargets; ++hwColorTarget) {
    Value *output = exportValues[hwColorTarget];
    if (output == nullptr)
      continue;
    const ColorExportValueInfo &colorExportInfo = expFragColors[hwColorTarget];
    info.push_back({hwColorTarget, colorExportInfo.location, colorExportInfo.isSigned, output->getType()});
  }

  if (info.empty()) {
    const auto &builtInUsage = m_resUsage->builtInUsage.fs;
    bool hasDepthExpFmtZero = !(builtInUsage.sampleMask || builtInUsage.fragStencilRef || builtInUsage.fragDepth);
    m_pipelineState->getPalMetadata()->updateSpiShaderColFormat(info, hasDepthExpFmtZero, builtInUsage.discard);
    return nullptr;
  }

  if (!m_pipelineState->isUnlinked() || m_pipelineState->hasColorExportFormats()) {
    // Whole-pipeline compilation (or shader compilation where we were given the color export formats).
    // Lower each color export
    CallInst *lastExport = nullptr;
    for (const ColorExportInfo &colorExportInfo : info) {
      Value *output = exportValues[colorExportInfo.hwColorTarget];
      ExportFormat expFmt =
          static_cast<ExportFormat>(m_pipelineState->computeExportFormat(colorExportInfo.ty, colorExportInfo.location));
      lastExport = static_cast<CallInst *>(fragColorExport->run(
          output, colorExportInfo.hwColorTarget, &*builder.GetInsertPoint(), expFmt, colorExportInfo.isSigned));
    }
    const auto &builtInUsage = m_resUsage->builtInUsage.fs;
    bool hasDepthExpFmtZero = !(builtInUsage.sampleMask || builtInUsage.fragStencilRef || builtInUsage.fragDepth);
    m_pipelineState->getPalMetadata()->updateSpiShaderColFormat(info, hasDepthExpFmtZero, builtInUsage.discard);
    return lastExport;
  }

  // Add the export info to be used when linking shaders to generate the color export shader and compute the spi shader
  // color format in the metadata.
  m_pipelineState->getPalMetadata()->addColorExportInfo(info);

  // TODO: The export values should be returned.
  return nullptr;
}

// =====================================================================================================================
// Generates the export instructions for builtin fragment outputs depth, stencil ref, and sample mask.  Returns the
// last export instruction that was generated.  Returns nullptr if no export instructions were generated.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
CallInst *LowerFragColorExport::addExportForBuiltinOutput(Function *module, BuilderBase &builder) {
  // Collect calls to the builtins
  Value *m_fragDepth = nullptr;
  Value *m_fragStencilRef = nullptr;
  Value *m_sampleMask = nullptr;
  for (auto &func : *module->getParent()) {
    if (!func.isDeclaration() || !func.getName().startswith(lgcName::OutputExportBuiltIn))
      continue;
    for (auto user : func.users()) {
      auto callInst = cast<CallInst>(user);
      if (callInst->getFunction() != module)
        continue;

      Value *output = callInst->getOperand(callInst->getNumArgOperands() - 1); // Last argument
      unsigned builtInId = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
      switch (builtInId) {
      case BuiltInFragDepth: {
        m_fragDepth = output;
        break;
      }
      case BuiltInSampleMask: {
        assert(output->getType()->isArrayTy());

        // NOTE: Only gl_SampleMask[0] is valid for us.
        m_sampleMask = builder.CreateExtractValue(output, {0});
        m_sampleMask = builder.CreateBitCast(m_sampleMask, builder.getFloatTy());
        break;
      }
      case BuiltInFragStencilRef: {
        m_fragStencilRef = builder.CreateBitCast(output, builder.getFloatTy());
        break;
      }
      default: {
        llvm_unreachable("Unexpected builtin output in fragment shader.");
        break;
      }
      }
    }
  }

  if (!m_fragDepth && !m_fragStencilRef && !m_sampleMask) {
    return nullptr;
  }

  auto &builtInUsage = m_resUsage->builtInUsage.fs;
  auto undef = UndefValue::get(Type::getFloatTy(*m_context));
  Value *fragDepth = undef;
  Value *fragStencilRef = undef;
  Value *sampleMask = undef;

  unsigned channelMask = 0x1; // Always export gl_FragDepth
  if (m_fragDepth) {
    assert(builtInUsage.fragDepth);
    (void(builtInUsage)); // unused
    fragDepth = m_fragDepth;
  }

  if (m_fragStencilRef) {
    assert(builtInUsage.fragStencilRef);
    (void(builtInUsage)); // unused
    channelMask |= 2;
    fragStencilRef = m_fragStencilRef;
  }

  if (m_sampleMask) {
    assert(builtInUsage.sampleMask);
    (void(builtInUsage)); // unused
    channelMask |= 4;
    sampleMask = m_sampleMask;
  }

  Value *args[] = {
      builder.getInt32(EXP_TARGET_Z), // tgt
      builder.getInt32(channelMask),  // en
      fragDepth,                      // src0
      fragStencilRef,                 // src1
      sampleMask,                     // src2
      undef,                          // src3
      builder.getFalse(),             // done
      builder.getTrue()               // vm
  };
  return builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(), args);
}

// =====================================================================================================================
// Generates a dummy export instruction.  Returns last export instruction that was generated.
//
// @param builder : The builder object that will be used to create new instructions.
CallInst *LowerFragColorExport::addDummyExport(BuilderBase &builder) {
  auto zero = ConstantFP::get(builder.getFloatTy(), 0.0);
  auto undef = UndefValue::get(builder.getFloatTy());
  Value *args[] = {
      builder.getInt32(EXP_TARGET_MRT_0), // tgt
      builder.getInt32(0x1),              // en
      zero,                               // src0
      undef,                              // src1
      undef,                              // src2
      undef,                              // src3
      builder.getFalse(),                 // done
      builder.getTrue()                   // vm
  };
  return builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(), args);
}

// =====================================================================================================================
// Sets the done flag on the given export instruction.
//
// @param [in/out] exportInst : The export instruction to be updated.
// @param builder : The builder object that will be used to create new instructions.
void LowerFragColorExport::setDoneFlag(CallInst *exportInst, BuilderBase &builder) {
  if (!exportInst)
    return;

  unsigned intrinsicId = exportInst->getIntrinsicID();
  if (intrinsicId == Intrinsic::amdgcn_exp)
    exportInst->setOperand(6, builder.getTrue());
  else {
    assert(intrinsicId == Intrinsic::amdgcn_exp_compr);
    exportInst->setOperand(4, builder.getTrue());
  }
}

// =====================================================================================================================
// Initialize the lower fragment color export pass
INITIALIZE_PASS(LowerFragColorExport, DEBUG_TYPE, "Lower fragment color export calls", false, false)
