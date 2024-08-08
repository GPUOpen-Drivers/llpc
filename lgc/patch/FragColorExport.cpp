/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  FragColorExport.cpp
 * @brief LLPC source file: contains implementation of class lgc::FragColorExport.
 ***********************************************************************************************************************
 */
#include "lgc/patch/FragColorExport.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/ResourceUsage.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-frag-color-export"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
//
// @param context : LLVM context
// @param pipelineState : Pipeline state
FragColorExport::FragColorExport(LgcContext *context) : m_lgcContext(context) {
}

// =====================================================================================================================
LowerFragColorExport::LowerFragColorExport() : m_exportValues(MaxColorTargets + 1, nullptr) {
}

// =====================================================================================================================
// Extract all the scalar elements of a scalar or vector type (with at-most four elements) input. Return an array of
// elements, padded with null values at the end when the input vector has less than 4 elements.
//
// @param input : The value we want to extract elements from
// @param builder : The IR builder for inserting instructions
// @param [out] results : The returned elements
static void extractElements(Value *input, BuilderBase &builder, std::array<Value *, 4> &results) {
  Type *valueTy = input->getType();
  unsigned compCount = valueTy->isVectorTy() ? cast<FixedVectorType>(valueTy)->getNumElements() : 1;
  assert(compCount <= 4 && "At-most four elements allowed\n");

  std::fill(results.begin(), results.end(), Constant::getNullValue(valueTy->getScalarType()));

  results[0] = input;
  if (!valueTy->isVectorTy())
    return;

  for (unsigned i = 0; i < compCount; i++) {
    results[i] = builder.CreateExtractElement(input, builder.getInt32(i));
  }
}

// =====================================================================================================================
// Executes fragment color export operations based on the specified output type and its location.
//
// @param output : Fragment color output
// @param hwColorExport : The color export target (MRT) of fragment color output
// @param builder : The IR builder for inserting instructions
// @param expFmt: The format for the given render target
// @param signedness: If output should be interpreted as a signed integer
// @param isDualSource: If it's under dualSourceBlend, it should be true
Value *FragColorExport::handleColorExportInstructions(Value *output, unsigned hwColorExport, BuilderBase &builder,
                                                      ExportFormat expFmt, const bool signedness,
                                                      const bool isDualSource) {
  assert(expFmt != EXP_FORMAT_ZERO);

  Type *outputTy = output->getType();
  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;
  Type *floatTy = builder.getFloatTy();
  Type *halfTy = builder.getHalfTy();
  Type *exportTypeMapping[] = {
      floatTy, //  EXP_FORMAT_ZERO = 0,
      floatTy, //  EXP_FORMAT_32_R = 1,
      floatTy, //  EXP_FORMAT_32_GR = 2,
      floatTy, //  EXP_FORMAT_32_AR = 3,
      halfTy,  //  EXP_FORMAT_FP16_ABGR = 4,
      halfTy,  //  EXP_FORMAT_UNORM16_ABGR = 5,
      halfTy,  //  EXP_FORMAT_SNORM16_ABGR = 6,
      halfTy,  //  EXP_FORMAT_UINT16_ABGR = 7,
      halfTy,  //  EXP_FORMAT_SINT16_ABGR = 8,
      floatTy, //  EXP_FORMAT_32_ABGR = 9,
  };

  const auto undefFloat = PoisonValue::get(builder.getFloatTy());
  const auto undefFloat16x2 = PoisonValue::get(FixedVectorType::get(builder.getHalfTy(), 2));
  const auto undefHalf = PoisonValue::get(halfTy);

  std::array<Value *, 4> comps{};
  std::array<Value *, 4> exports{undefFloat, undefFloat, undefFloat, undefFloat};
  unsigned exportMask = 0;

  Type *exportTy = exportTypeMapping[expFmt];

  // For 32bit output, we always to scalarize, but for 16bit output we may just operate on vector.
  if (exportTy->isFloatTy()) {
    if (compCount == 1) {
      comps[0] = output;
    } else {
      for (unsigned i = 0; i < compCount; ++i)
        comps[i] = builder.CreateExtractElement(output, builder.getInt32(i));
    }
  }

  switch (expFmt) {
  case EXP_FORMAT_32_R:
  case EXP_FORMAT_32_GR:
  case EXP_FORMAT_32_ABGR: {
    if (expFmt == EXP_FORMAT_32_R)
      compCount = 1;
    else if (expFmt == EXP_FORMAT_32_GR)
      compCount = compCount >= 2 ? 2 : 1;

    for (unsigned idx = 0; idx < compCount; ++idx) {
      unsigned compMask = 1 << idx;
      exports[idx] = convertToFloat(comps[idx], signedness, builder);
      exportMask |= compMask;
    }
    break;
  }
  case EXP_FORMAT_32_AR: {
    exports[0] = convertToFloat(comps[0], signedness, builder);
    exportMask = 1;
    if (compCount == 4) {
      exports[1] = convertToFloat(comps[3], signedness, builder);
      exportMask |= 0x2;
      compCount = 2;
    } else {
      compCount = 1;
    }
    break;
  }
  case EXP_FORMAT_FP16_ABGR: {
    const unsigned compactCompCount = (compCount + 1) / 2;
    exports[0] = exports[1] = undefFloat16x2;
    exportMask = compCount > 2 ? 0xF : 0x3;
    // convert to half type
    if (bitWidth <= 16) {
      output = convertToHalf(output, signedness, builder);
      extractElements(output, builder, comps);
      // re-pack
      for (unsigned idx = 0; idx < compactCompCount; ++idx) {
        unsigned compId1 = 2 * idx;
        unsigned compId2 = compId1 + 1;
        exports[idx] = builder.CreateInsertElement(undefFloat16x2, comps[compId1], builder.getInt32(0));
        if (!comps[compId2])
          comps[compId2] = undefHalf;
        exports[idx] = builder.CreateInsertElement(exports[idx], comps[compId2], builder.getInt32(1));
      }
    } else {
      if (outputTy->isIntOrIntVectorTy())
        output =
            builder.CreateBitCast(output, outputTy->isVectorTy() ? FixedVectorType::get(builder.getFloatTy(), compCount)
                                                                 : builder.getFloatTy());
      extractElements(output, builder, comps);

      for (unsigned idx = 0; idx < compactCompCount; ++idx) {
        unsigned compId1 = 2 * idx;
        unsigned compId2 = compId1 + 1;
        if (!comps[compId2])
          comps[compId2] = undefHalf;
        exports[idx] = builder.CreateIntrinsic(FixedVectorType::get(builder.getHalfTy(), 2),
                                               Intrinsic::amdgcn_cvt_pkrtz, {comps[compId1], comps[compId2]});
      }
    }
    break;
  }
  case EXP_FORMAT_UNORM16_ABGR:
  case EXP_FORMAT_SNORM16_ABGR:
  case EXP_FORMAT_UINT16_ABGR:
  case EXP_FORMAT_SINT16_ABGR: {
    assert(compCount <= 4);

    unsigned cvtIntrinsic;
    if (expFmt == EXP_FORMAT_SNORM16_ABGR || expFmt == EXP_FORMAT_UNORM16_ABGR) {
      output = convertToFloat(output, signedness, builder);
      cvtIntrinsic =
          expFmt == EXP_FORMAT_SNORM16_ABGR ? Intrinsic::amdgcn_cvt_pknorm_i16 : Intrinsic::amdgcn_cvt_pknorm_u16;
    } else {
      output = convertToInt(output, signedness, builder);
      cvtIntrinsic = expFmt == EXP_FORMAT_SINT16_ABGR ? Intrinsic::amdgcn_cvt_pk_i16 : Intrinsic::amdgcn_cvt_pk_u16;
    }
    extractElements(output, builder, comps);

    const unsigned compactCompCount = (compCount + 1) / 2;
    exports[0] = exports[1] = undefFloat16x2;
    exportMask = compCount > 2 ? 0xF : 0x3;
    for (unsigned idx = 0; idx < compactCompCount; idx++) {
      unsigned compId1 = 2 * idx;
      unsigned compId2 = compId1 + 1;
      if (!comps[compId2])
        comps[compId2] = undefHalf;
      Value *packedComps = builder.CreateIntrinsic(FixedVectorType::get(builder.getInt16Ty(), 2), cvtIntrinsic,
                                                   {comps[compId1], comps[compId2]});
      exports[idx] = builder.CreateBitCast(packedComps, FixedVectorType::get(builder.getHalfTy(), 2));
    }
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  if (isDualSource) {
    assert(m_lgcContext->getTargetInfo().getGfxIpVersion().major >= 11);
    // Save them for later dual-source-swizzle
    m_blendSourceChannels = exportTy->isHalfTy() ? (compCount + 1) / 2 : compCount;
    assert(hwColorExport <= 1);
    m_blendSources[hwColorExport].append(exports.begin(), exports.end());
    return nullptr;
  }

  if (m_lgcContext->getTargetInfo().getGfxIpVersion().major >= 11 && exportTy->isHalfTy()) {
    // GFX11 removes compressed export, simply use 32bit-data export.
    exportMask = 0;
    const unsigned compactCompCount = (compCount + 1) / 2;
    for (unsigned idx = 0; idx < compactCompCount; ++idx) {
      exports[idx] = builder.CreateBitCast(exports[idx], builder.getFloatTy());
      exportMask |= 1 << idx;
    }
    for (unsigned idx = compactCompCount; idx < 4; ++idx)
      exports[idx] = undefFloat;
  }

  Value *exportCall = nullptr;

  if (exportTy->isHalfTy() && m_lgcContext->getTargetInfo().getGfxIpVersion().major < 11) {
    // 16-bit export (compressed)
    Value *args[] = {
        builder.getInt32(EXP_TARGET_MRT_0 + hwColorExport), // tgt
        builder.getInt32(exportMask),                       // en
        exports[0],                                         // src0
        exports[1],                                         // src1
        builder.getFalse(),                                 // done
        builder.getTrue()                                   // vm
    };

    exportCall = builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_exp_compr, args);
  } else {
    Value *args[] = {
        builder.getInt32(EXP_TARGET_MRT_0 + hwColorExport), // tgt
        builder.getInt32(exportMask),                       // en
        exports[0],                                         // src0
        exports[1],                                         // src1
        exports[2],                                         // src2
        exports[3],                                         // src3
        builder.getFalse(),                                 // done
        builder.getTrue()                                   // vm
    };

    exportCall = builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_exp, args);
  }

  return exportCall;
}

// =====================================================================================================================
// Converts an output value to its half floating-point representation. This function is a "helper" in computing
// the export value based on shader export format.
//
// @param value : Output value
// @param signedness : Whether the type is signed (valid for integer type)
// @param builder : The IR builder for inserting instructions
Value *FragColorExport::convertToHalf(Value *value, bool signedness, BuilderBase &builder) const {
  Type *valueTy = value->getType();
  unsigned numElements = valueTy->isVectorTy() ? cast<FixedVectorType>(valueTy)->getNumElements() : 1;
  const unsigned bitWidth = valueTy->getScalarSizeInBits();
  Type *int16Ty = builder.getInt16Ty();
  Type *halfTy = builder.getHalfTy();
  if (valueTy->isVectorTy()) {
    int16Ty = FixedVectorType::get(builder.getInt16Ty(), numElements);
    halfTy = FixedVectorType::get(builder.getHalfTy(), numElements);
  }
  assert(bitWidth <= 16 && "we do not support 32bit here");
  if (bitWidth < 16) {
    if (signedness)
      value = builder.CreateSExt(value, int16Ty);
    else
      value = builder.CreateZExt(value, int16Ty);
  }
  if (valueTy->isIntOrIntVectorTy()) {
    // This is very corner-case that writing un-matching integer to half color buffer, the behavior is undefined.
    // I think here we should do a convert. But I am not quite sure on that. So I just keep the old behavior.
    value = builder.CreateBitCast(value, halfTy);
  }

  return value;
}

// =====================================================================================================================
// Converts an output value to its floating-point representation. This function is a "helper" in computing
// the export value based on shader export format.
//
// @param value : Output value
// @param signedness : Whether the type is signed (valid for integer type)
// @param builder : The IR builder for inserting instructions
Value *FragColorExport::convertToFloat(Value *value, bool signedness, BuilderBase &builder) const {
  Type *valueTy = value->getType();
  const unsigned bitWidth = valueTy->getScalarSizeInBits();
  unsigned numElements = valueTy->isVectorTy() ? cast<FixedVectorType>(valueTy)->getNumElements() : 1;
  Type *int32Ty = builder.getInt32Ty();
  Type *floatTy = builder.getFloatTy();
  if (valueTy->isVectorTy()) {
    int32Ty = FixedVectorType::get(builder.getInt32Ty(), numElements);
    floatTy = FixedVectorType::get(builder.getFloatTy(), numElements);
  }

  if (bitWidth <= 16) {
    if (valueTy->isIntOrIntVectorTy()) {
      if (signedness) {
        // %value = sext i8/i16 %value to i32
        value = builder.CreateSExt(value, int32Ty);
      } else {
        // %value = zext i8/i16 %value to i32
        value = builder.CreateZExt(value, int32Ty);
      }
      // %value = bitcast i32 %value to float
      value = builder.CreateBitCast(value, floatTy);
    } else {
      assert(bitWidth == 16);
      // %value = fpext half %value to float
      value = builder.CreateFPExt(value, floatTy);
    }
  } else {
    assert(bitWidth == 32);
    if (valueTy->isIntOrIntVectorTy()) {
      // %value = bitcast i32 %value to float
      value = builder.CreateBitCast(value, floatTy);
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
// @param builder : The IR builder for inserting instructions
Value *FragColorExport::convertToInt(Value *value, bool signedness, BuilderBase &builder) const {
  Type *valueTy = value->getType();
  const unsigned bitWidth = valueTy->getScalarSizeInBits();
  unsigned numElements = valueTy->isVectorTy() ? cast<FixedVectorType>(valueTy)->getNumElements() : 1;

  Type *int32Ty = builder.getInt32Ty();
  Type *int16Ty = builder.getInt16Ty();
  if (valueTy->isVectorTy()) {
    int32Ty = FixedVectorType::get(builder.getInt32Ty(), numElements);
    int16Ty = FixedVectorType::get(builder.getInt16Ty(), numElements);
  }
  if (bitWidth <= 16) {
    if (valueTy->isFloatingPointTy()) {
      // %value = bitcast half %value to i16
      value = builder.CreateBitCast(value, int16Ty);
    }

    if (signedness) {
      // %value = sext i16/i8 %value to i32
      value = builder.CreateSExt(value, int32Ty);
    } else {
      // %value = zext i16/i8 %value to i32
      value = builder.CreateZExt(value, int32Ty);
    }
  } else {
    assert(bitWidth == 32); // The valid bit width is 16 or 32
    if (valueTy->isFPOrFPVectorTy()) {
      // %value = bitcast float %value to i32
      value = builder.CreateBitCast(value, int32Ty);
    }
  }

  return value;
}

} // namespace lgc

// =====================================================================================================================
// Run the lower color export pass on a module
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerFragColorExport::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);

  m_context = &module.getContext();
  m_pipelineState = pipelineState;
  m_resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment);

  Function *fragEntryPoint = pipelineShaders.getEntryPoint(ShaderStage::Fragment);
  if (!fragEntryPoint)
    return PreservedAnalyses::all();

  // Find the return instruction as that will be the insertion point for the export instructions.
  // It is possible that there is no return instruction if there is an infinite loop.  See the shaderdb test
  // OpLoopMerge_TestIterationControls_lit.frag.  In that case, there should be no need for exports.
  ReturnInst *retInst = nullptr;
  for (auto &block : llvm::reverse(*fragEntryPoint)) {
    if (auto ret = dyn_cast<ReturnInst>(block.getTerminator())) {
      retInst = ret;
      break;
    }
  }
  if (!retInst)
    return PreservedAnalyses::all();

  BuilderBase builder(module.getContext());
  builder.SetInsertPoint(retInst);

  for (auto &inst : instructions(fragEntryPoint)) {
    unsigned addrSpace = 0;
    if (auto store = dyn_cast<StoreInst>(&inst))
      addrSpace = store->getPointerAddressSpace();
    else if (auto atomicRmw = dyn_cast<AtomicRMWInst>(&inst))
      addrSpace = atomicRmw->getPointerAddressSpace();
    else if (auto atomicXchg = dyn_cast<AtomicCmpXchgInst>(&inst))
      addrSpace = atomicXchg->getPointerAddressSpace();

    if (addrSpace == ADDR_SPACE_GLOBAL) {
      m_resUsage->resourceWrite = true;
      break;
    }
  }

  collectExportInfoForBuiltinOutput(fragEntryPoint, builder);
  collectExportInfoForGenericOutputs(fragEntryPoint, builder);

  // Now do color exports by color buffer.
  // Read the dynamicDualSourceBlend value from user data and jump to different branch basing on the value
  // 1. For dynamic pipeline, as the colorblendequation may be dynamic state, so there is a case like this:
  // Create blendEquation(only use srcColor) -> bindDynamicPipeline -> setBlendEquation(use srcColor1).
  // As a result, it needs switch the export instruction from normal export into dual exports.
  // 2. For static pipeline which will not use the usedata and identify whether do dualSourceBlend
  // Just according to the dualSourceBlendEnable flag.
  Value *dynamicIsDualSource = builder.getInt32(0);
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11) {
    if (m_pipelineState->isUnlinked() || m_pipelineState->getColorExportState().dualSourceBlendDynamicEnable) {
      dynamicIsDualSource = ShaderInputs::getSpecialUserData(UserDataMapping::CompositeData, builder);
      dynamicIsDualSource = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                                    {dynamicIsDualSource, builder.getInt32(7), builder.getInt32(1)});
    }
  }

  bool willGenerateColorExportShader = m_pipelineState->isUnlinked() && !m_pipelineState->hasColorExportFormats();
  if (willGenerateColorExportShader && !m_info.empty()) {
    createTailJump(fragEntryPoint, builder, dynamicIsDualSource);
    return PreservedAnalyses::none();
  }

  FragColorExport fragColorExport(m_pipelineState->getLgcContext());
  bool dummyExport = m_resUsage->builtInUsage.fs.discard || m_pipelineState->getOptions().forceFragColorDummyExport ||
                     m_pipelineState->getShaderModes()->getFragmentShaderMode().enablePops;
  FragColorExport::Key key = FragColorExport::computeKey(m_info, m_pipelineState);
  fragColorExport.generateExportInstructions(m_info, m_exportValues, dummyExport, m_pipelineState->getPalMetadata(),
                                             builder, dynamicIsDualSource, key);
  return (!m_info.empty() || dummyExport) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Updates the value in the entry in expFragColors that callInst is writing to.
//
// @param callInst : An call to the generic output export builtin in a fragment shader.
// @param [in/out] outFragColors : An array with the current color output information for each color output location.
// @param builder : builder to use
void LowerFragColorExport::updateFragColors(CallInst *callInst, MutableArrayRef<ColorOutputValueInfo> outFragColors,
                                            BuilderBase &builder) {
  const unsigned location = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
  const unsigned component = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue();
  Value *output = callInst->getOperand(2);
  assert(output->getType()->getScalarSizeInBits() <= 32); // 64-bit output is not allowed
  assert(component < 4);

  Type *outputTy = output->getType();

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);
  (void(bitWidth)); // unused

  auto &expFragColor = outFragColors[location];

  if (auto *vecTy = dyn_cast<FixedVectorType>(outputTy)) {
    assert(component + vecTy->getNumElements() <= 4);
    for (unsigned i = 0; i < vecTy->getNumElements(); ++i)
      expFragColor.value[component + i] = builder.CreateExtractElement(output, i);
  } else {
    expFragColor.value[component] = output;
  }
  BasicType outputType = m_resUsage->inOutUsage.fs.outputTypes[location];
  expFragColor.isSigned =
      (outputType == BasicType::Int8 || outputType == BasicType::Int16 || outputType == BasicType::Int);
}

// =====================================================================================================================
// Collects the information needed to generate the export instructions for all of the generic outputs of the fragment
// shader fragEntryPoint.  This information is stored in m_info and m_exportValues.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
void LowerFragColorExport::collectExportInfoForGenericOutputs(Function *fragEntryPoint, BuilderBase &builder) {
  std::unique_ptr<FragColorExport> fragColorExport(new FragColorExport(m_pipelineState->getLgcContext()));
  SmallVector<CallInst *, 8> colorExports;

  // Collect all of the exports in the fragment shader
  for (auto &func : *fragEntryPoint->getParent()) {
    if (!func.isDeclaration() || !func.getName().starts_with(lgcName::OutputExportGeneric))
      continue;
    for (auto user : func.users()) {
      auto callInst = cast<CallInst>(user);
      if (callInst->getFunction() == fragEntryPoint)
        colorExports.push_back(callInst);
    }
  }

  if (colorExports.empty())
    return;

  // Collect all of the values that need to be exported for each hardware color target.
  ColorOutputValueInfo outFragColors[MaxColorTargets] = {};
  {
    IRBuilderBase::InsertPointGuard ipg(builder);
    for (CallInst *callInst : colorExports) {
      builder.SetInsertPoint(callInst);
      updateFragColors(callInst, outFragColors, builder);
      callInst->eraseFromParent();
    }
  }

  // Recombine the vectors and pack them.
  unsigned exportIndex = 0;
  for (const auto &[location, info] : llvm::enumerate(ArrayRef(outFragColors))) {
    Type *compTy = nullptr;
    unsigned numComponents = 0;
    for (const auto &[i, value] : llvm::enumerate(info.value)) {
      if (value) {
        numComponents = i + 1;
        compTy = value->getType();
      }
    }
    if (!numComponents)
      continue;

    // Construct exported fragment colors
    Value *value = nullptr;
    if (numComponents == 1) {
      value = info.value[0];
    } else {
      value = PoisonValue::get(FixedVectorType::get(compTy, numComponents));
      for (unsigned i = 0; i < numComponents; ++i) {
        if (info.value[i])
          value = builder.CreateInsertElement(value, info.value[i], i);
      }
    }

    m_exportValues[exportIndex] = value;
    m_info.push_back({exportIndex, (unsigned)location, info.isSigned, value->getType()});
    ++exportIndex;
  }
}

// =====================================================================================================================
// Generates a return instruction or a tail call that will make all of the values for the exports available to the
// color export shader. The color export information is added to the pal metadata, so that everything needed to
// generate the color export shader is available.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
void LowerFragColorExport::createTailJump(Function *fragEntryPoint, BuilderBase &builder, Value *isDualSource) {
  // Add the export info to be used when linking shaders to generate the color export shader and compute the spi shader
  // color format in the metadata.
  m_pipelineState->getPalMetadata()->addColorExportInfo(m_info);
  m_pipelineState->getPalMetadata()->setDiscardState(m_resUsage->builtInUsage.fs.discard);

  // First build the type for passing outputs to the color export shader.
  SmallVector<Type *, 8> outputTypes;
  for (const ColorExportInfo &info : m_info) {
    outputTypes.push_back(getVgprTy(info.ty));
  }
  outputTypes.push_back(builder.getInt32Ty());

  // Now build the return value.
  SmallVector<Value *, 8> cesArgs;

  unsigned returnLocation = 0;
  for (unsigned idx = 0; idx < m_info.size(); ++idx) {
    const ColorExportInfo &info = m_info[idx];
    unsigned hwColorTarget = info.hwColorTarget;
    Value *output = m_exportValues[hwColorTarget];
    if (!output)
      continue;
    if (output->getType() != outputTypes[idx])
      output = generateValueForOutput(output, outputTypes[idx], builder);
    cesArgs.push_back(output);
    ++returnLocation;
  }
  cesArgs.push_back(isDualSource);

  if (m_pipelineState->getOptions().enableColorExportShader) {
    // Build color export function type
    auto funcTy = FunctionType::get(builder.getVoidTy(), outputTypes, false);
    // Convert color export shader address to function pointer
    auto funcTyPtr = funcTy->getPointerTo(ADDR_SPACE_CONST);
    auto colorShaderAddr = ShaderInputs::getSpecialUserData(UserDataMapping::ColorExportAddr, builder);
    AddressExtender addrExt(builder.GetInsertPoint()->getParent()->getParent());
    auto funcPtr = addrExt.extendWithPc(colorShaderAddr, funcTyPtr, builder);

    // Jump
    auto callInst = builder.CreateCall(funcTy, funcPtr, cesArgs);
    callInst->setCallingConv(CallingConv::AMDGPU_Gfx);
    callInst->addParamAttr(returnLocation, Attribute::InReg);
    callInst->setDoesNotReturn();
    callInst->setOnlyWritesMemory();
  } else {
    Type *retTy = StructType::get(*m_context, outputTypes);
    addFunctionArgs(fragEntryPoint, retTy, {}, {});
    Value *retVal = PoisonValue::get(retTy);
    for (int idx = 0; idx < cesArgs.size(); ++idx) {
      retVal = builder.CreateInsertValue(retVal, cesArgs[idx], idx);
    }
    retVal = builder.CreateRet(retVal);
    auto *retInst = builder.GetInsertPoint()->getParent()->getTerminator();
    retInst->eraseFromParent();
  }
}

// =====================================================================================================================
// Collects the information need to generate the export instruction for the builtin fragment outputs depth, stencil ref,
// and sample mask.  This information is added to m_info and m_exportValues.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
void LowerFragColorExport::collectExportInfoForBuiltinOutput(Function *module, BuilderBase &builder) {
  // Collect calls to the builtins
  Value *m_fragDepth = nullptr;
  Value *m_fragStencilRef = nullptr;
  Value *m_sampleMask = nullptr;
  for (auto &func : *module->getParent()) {
    if (!func.isDeclaration() || !func.getName().starts_with(lgcName::OutputExportBuiltIn))
      continue;
    for (auto user : func.users()) {
      auto callInst = cast<CallInst>(user);
      if (callInst->getFunction() != module)
        continue;

      Value *output = callInst->getOperand(callInst->arg_size() - 1); // Last argument
      unsigned builtInId = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
      switch (builtInId) {
      case BuiltInFragDepth: {
        m_fragDepth = output;
        break;
      }
      case BuiltInSampleMask: {
        assert(output->getType()->isArrayTy());
        if (!m_pipelineState->getOptions().disableSampleMask) {
          // NOTE: Only gl_SampleMask[0] is valid for us.
          m_sampleMask = builder.CreateExtractValue(output, {0});
          m_sampleMask = builder.CreateBitCast(m_sampleMask, builder.getFloatTy());
        }

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
    return;
  }

  auto &builtInUsage = m_resUsage->builtInUsage.fs;
  auto poison = PoisonValue::get(Type::getFloatTy(*m_context));
  Value *fragDepth = poison;
  Value *fragStencilRef = poison;
  Value *sampleMask = poison;

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

  ColorExportInfo info = {};
  info.hwColorTarget = MaxColorTargets;
  info.location = channelMask;
  info.isSigned = false;
  info.ty = FixedVectorType::get(builder.getFloatTy(), 4);
  m_info.push_back(info);

  Value *output = PoisonValue::get(info.ty);
  output = builder.CreateInsertElement(output, fragDepth, static_cast<uint64_t>(0));
  output = builder.CreateInsertElement(output, fragStencilRef, 1);
  output = builder.CreateInsertElement(output, sampleMask, 2);
  m_exportValues[MaxColorTargets] = output;
}

// =====================================================================================================================
// Generates a dummy export instruction.  Returns last export instruction that was generated.
//
// @param builder : The builder object that will be used to create new instructions.
CallInst *FragColorExport::addDummyExport(BuilderBase &builder) {
  auto zero = ConstantFP::get(builder.getFloatTy(), 0.0);
  auto poison = PoisonValue::get(builder.getFloatTy());
  Value *args[] = {
      builder.getInt32(EXP_TARGET_MRT_0), // tgt
      builder.getInt32(0x1),              // en
      zero,                               // src0
      poison,                             // src1
      poison,                             // src2
      poison,                             // src3
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
void FragColorExport::setDoneFlag(Value *exportInst, BuilderBase &builder) {
  if (!exportInst)
    return;

  CallInst *callInst = dyn_cast<CallInst>(exportInst);
  if (!callInst)
    return;

  unsigned intrinsicId = callInst->getIntrinsicID();
  if (intrinsicId == Intrinsic::amdgcn_exp)
    callInst->setOperand(6, builder.getTrue());
  else {
    assert(intrinsicId == Intrinsic::amdgcn_exp_compr);
    callInst->setOperand(4, builder.getTrue());
  }
}

// =====================================================================================================================
// Swizzle the output to MRT0/MRT1 for dual source blend on GFX11+, and return the last export instruction.
//
// @param builder : The builder object that will be used to create new instructions.
Value *FragColorExport::dualSourceSwizzle(unsigned waveSize, BuilderBase &builder) {
  Value *result0[4], *result1[4];
  auto undefFloat = PoisonValue::get(builder.getFloatTy());

  Value *threadId =
      builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});
  if (waveSize == 64)
    threadId = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), threadId});
  threadId = builder.CreateAnd(threadId, builder.getInt32(1));
  // mask: 0 1 0 1 0 1 ...
  Value *mask = builder.CreateICmpNE(threadId, builder.getInt32(0));
  bool onlyOneTarget = m_blendSources[1].empty();

  for (unsigned i = 0; i < m_blendSourceChannels; i++) {
    Value *src0 = m_blendSources[0][i];
    Value *src1 = onlyOneTarget ? PoisonValue::get(src0->getType()) : m_blendSources[1][i];
    src0 = builder.CreateBitCast(src0, builder.getInt32Ty());
    src1 = builder.CreateBitCast(src1, builder.getInt32Ty());

    src0 = builder.CreateSetInactive(src0, builder.getInt32(0));
    src1 = builder.CreateSetInactive(src1, builder.getInt32(0));
    // Construct a mask to help the later swizzle work. As we are mainly swapping neighbouring even/odd lanes afterward,
    // so we need the dpp8-mask(from LSB to MSB, each take 3bits): 1 0 3 2 5 4 7 6
    Value *dpp8 = builder.getInt32(1 | 0 << 3 | 3 << 6 | 2 << 9 | 5 << 12 | 4 << 15 | 7 << 18 | 6 << 21);

    // Swapping every even/odd lanes of Src1 (S10 means lane-0 of src1).
    // src1Shuffle: S11 S10 S13 S12 ...
    Value *src1Shuffle = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp8, builder.getInt32Ty(), {src1, dpp8});

    // blend0: S00 S10 S02 S12 ...
    Value *blend0 = builder.CreateSelect(mask, src1Shuffle, src0);
    blend0 = builder.CreateBitCast(blend0, builder.getFloatTy());
    result0[i] = blend0;

    // blend1: S11 S01 S13 S03 ...
    Value *blend1 = builder.CreateSelect(mask, src0, src1Shuffle);

    // blend1: S01 S11 S03 S13 ...
    blend1 = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp8, builder.getInt32Ty(), {blend1, dpp8});
    blend1 = builder.CreateBitCast(blend1, builder.getFloatTy());
    result1[i] = blend1;
  }

  for (unsigned i = m_blendSourceChannels; i < 4; i++) {
    result0[i] = undefFloat;
    result1[i] = undefFloat;
  }

  Value *args0[] = {
      builder.getInt32(EXP_TARGET_DUAL_SRC_0),            // tgt
      builder.getInt32((1 << m_blendSourceChannels) - 1), // en
      result0[0],                                         // src0
      result0[1],                                         // src1
      result0[2],                                         // src2
      result0[3],                                         // src3
      builder.getFalse(),                                 // done
      builder.getTrue()                                   // vm
  };
  builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_exp, args0);

  Value *args1[] = {
      builder.getInt32(EXP_TARGET_DUAL_SRC_1),            // tgt
      builder.getInt32((1 << m_blendSourceChannels) - 1), // en
      result1[0],                                         // src0
      result1[1],                                         // src1
      result1[2],                                         // src2
      result1[3],                                         // src3
      builder.getFalse(),                                 // done
      builder.getTrue()                                   // vm
  };
  return builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_exp, args1);
}

// =====================================================================================================================
// Update the color export information when enableFragColor is set.
//
// @param key : Color export info.
// @param originExpinfo : The original color export information for each color export in no particular order.
// @param needMrt0a: The flag to tell MRT0.a is required.
// @param pCbShaderMask: The cbShaderMask after update color export information
// @param [out] outExpinfo : The updated color export information when enableFragColor is true.
void FragColorExport::updateColorExportInfoWithBroadCastInfo(const Key &key, ArrayRef<ColorExportInfo> originExpinfo,
                                                             bool needMrt0a, SmallVector<ColorExportInfo> &outExpinfo,
                                                             unsigned *pCbShaderMask) {
  // As enableFragColor will only be enabled by OGL, so it will not consider on the dualSource cases.
  SmallVector<ColorExportInfo> broadCastInfo;
  if (key.enableFragColor) {
    auto &expInfo = originExpinfo[0];
    assert(expInfo.ty != nullptr);
    for (unsigned location = 0; location < MaxColorTargets; ++location) {
      if (key.expFmt[location] != 0)
        broadCastInfo.push_back({0, location, expInfo.isSigned, expInfo.ty});
    }
  }
  outExpinfo = key.enableFragColor ? broadCastInfo : SmallVector<ColorExportInfo>(originExpinfo);
  for (auto &exp : outExpinfo) {
    if (exp.hwColorTarget == MaxColorTargets)
      continue;
    const unsigned channelWriteMask = key.channelWriteMask[exp.location];
    unsigned expFormat = key.expFmt[exp.location];
    bool needUpdateMask = expFormat != 0 && (channelWriteMask > 0 || (exp.location == 0 && needMrt0a));
    if (needUpdateMask) {
      // For dualSource, the cbShaderMask will only be valid for location=0, other locations setting will be
      // redundant. ToDo: this point can be optimized when use different ShaderMaskMetaKey or compile different
      // shaders.
      *pCbShaderMask |= (0xF << (4 * exp.location));
    }
  }
}

// =====================================================================================================================
// Generates the export instructions based on the given color export information in dynamic state
//
// @param info : The color export information for each color export in no particular order.
// @param values : The values that are to be exported.  Indexed by the hw color target.
// @param exportFormat : The export format for each color target. Indexed by the hw color target.
// @param [out] palMetadata : The PAL metadata that will be extended with relevant information.
// @param builder : The builder object that will be used to create new instructions.
// @param dynamicIsDualSource: Identify whether it's in dynamicDualSourceBlend state
// @param key: Color export Info
void FragColorExport::generateExportInstructions(ArrayRef<ColorExportInfo> info, ArrayRef<Value *> values,
                                                 bool dummyExport, PalMetadata *palMetadata, BuilderBase &builder,
                                                 Value *dynamicIsDualSource, const Key &key) {
  Value *lastExport = nullptr;
  unsigned gfxip = m_lgcContext->getTargetInfo().getGfxIpVersion().major;

  // MRTZ export comes first if it exists (this is a HW requirement on gfx11+ and an optional good idea on earlier HW).
  // We make the assume here that it is also first in the info list.
  bool needMrt0a = key.colorExportState.alphaToCoverageEnable;
  if (!info.empty() && info[0].hwColorTarget == MaxColorTargets) {
    unsigned depthMask = info[0].location;

    // Depth export alpha comes from MRT0.a if there is MRT0.a and A2C is enabled on GFX11+
    Value *alpha = PoisonValue::get(builder.getFloatTy());
    if (!dummyExport && gfxip >= 11 && key.colorExportState.alphaToCoverageEnable) {
      for (auto &curInfo : info) {
        if (curInfo.location != 0)
          continue;

        auto *vecTy = dyn_cast<FixedVectorType>(values[curInfo.hwColorTarget]->getType());
        if (!vecTy || vecTy->getNumElements() < 4)
          break;

        // Mrt0 is enabled and its alpha channel is enabled
        alpha = builder.CreateExtractElement(values[curInfo.hwColorTarget], 3);
        if (alpha->getType()->isIntegerTy())
          alpha = builder.CreateBitCast(alpha, builder.getFloatTy());
        depthMask |= 0x8;
        needMrt0a = false;
        break;
      }
    }

    Value *output = values[MaxColorTargets];
    Value *fragDepth = builder.CreateExtractElement(output, static_cast<uint64_t>(0));
    Value *fragStencilRef = builder.CreateExtractElement(output, 1);
    Value *sampleMask = builder.CreateExtractElement(output, 2);
    Value *args[] = {
        builder.getInt32(EXP_TARGET_Z), // tgt
        builder.getInt32(depthMask),    // en
        fragDepth,                      // src0
        fragStencilRef,                 // src1
        sampleMask,                     // src2
        alpha,                          // src3
        builder.getFalse(),             // done
        builder.getTrue()               // vm
    };
    lastExport = builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(), args);

    unsigned depthExpFmt = EXP_FORMAT_ZERO;
    if (depthMask & 0x4)
      depthExpFmt = EXP_FORMAT_32_ABGR;
    else if (depthMask & 0x2)
      depthExpFmt = (depthMask & 0x8) ? EXP_FORMAT_32_ABGR : EXP_FORMAT_32_GR;
    else if (depthMask & 0x1)
      depthExpFmt = (depthMask & 0x8) ? EXP_FORMAT_32_AR : EXP_FORMAT_32_R;

    palMetadata->setSpiShaderZFormat(depthExpFmt);
    info = info.drop_front(1);
  }

  SmallVector<ExportFormat> finalExportFormats;
  SmallVector<ColorExportInfo> finalExpInfo;
  unsigned cbShaderMask = 0;

  ReturnInst *retInst = cast<ReturnInst>(builder.GetInsertPoint()->getParent()->getTerminator());
  Function *originFunc = builder.GetInsertPoint()->getParent()->getParent();
  BasicBlock *dualSourceBlock = nullptr;
  BasicBlock *normalExportBlock = nullptr;

  updateColorExportInfoWithBroadCastInfo(key, info, needMrt0a, finalExpInfo, &cbShaderMask);

  if (key.colorExportState.dualSourceBlendDynamicEnable && (gfxip >= 11)) {
    // For dynamiceState, whether do dualSourceBlend will depend on the user data.
    dualSourceBlock = BasicBlock::Create(m_lgcContext->getContext(), "dualSourceSwizzle", originFunc);
    normalExportBlock = BasicBlock::Create(m_lgcContext->getContext(), "normalExport", originFunc);
    Value *isDualSource = builder.CreateICmpNE(dynamicIsDualSource, builder.getInt32(0));
    builder.CreateCondBr(isDualSource, dualSourceBlock, normalExportBlock);
  } else {
    if (retInst) {
      retInst->eraseFromParent();
      retInst = nullptr;
    }
    if (key.colorExportState.dualSourceBlendEnable && (gfxip >= 11))
      // For only-static case, it will depend on dualSourceBlendEnable flag to do dualSourceBlend.
      dualSourceBlock = builder.GetInsertBlock();
    else
      // Both staticDualSourceBlend flag and dynamicDualSourceBlend flag are 0, don't export in dualSourceBlend.
      normalExportBlock = builder.GetInsertBlock();
  }

  // Construct ".dualSourceSwizzle" Block, only construct when the dynamicEnable is on and staticValue is true.
  if ((key.colorExportState.dualSourceBlendDynamicEnable || key.colorExportState.dualSourceBlendEnable) &&
      (gfxip >= 11)) {
    builder.SetInsertPoint(dualSourceBlock);

    for (unsigned idx = 0; idx < 2; idx++) {
      auto infoIt = llvm::find_if(finalExpInfo, [&](const ColorExportInfo &info) { return info.location == idx; });
      if (infoIt != finalExpInfo.end()) {
        auto dualExpFmt = static_cast<ExportFormat>(key.expFmt[idx]);
        const unsigned channelWriteMask = key.channelWriteMask[0];
        if (dualExpFmt != EXP_FORMAT_ZERO && (channelWriteMask > 0 || (infoIt->location == 0 && needMrt0a))) {
          // Collect info for dualSourceBlend and save then in m_blendSources, so set the last parameter=true;
          handleColorExportInstructions(values[infoIt->hwColorTarget], idx, builder, dualExpFmt, infoIt->isSigned,
                                        true);
          finalExportFormats.push_back(dualExpFmt);
        }
      }
    }

    if (m_blendSourceChannels > 0) {
      lastExport = dualSourceSwizzle(key.waveSize, builder);
      FragColorExport::setDoneFlag(lastExport, builder);
    }
    builder.CreateRetVoid();
  }

  // Construct ".normalExport" Block
  if (key.colorExportState.dualSourceBlendDynamicEnable || !key.colorExportState.dualSourceBlendEnable ||
      (gfxip < 11)) {
    builder.SetInsertPoint(normalExportBlock);
    unsigned hwColorExport = 0;
    for (unsigned location = 0; location < MaxColorTargets; ++location) {
      auto infoIt = llvm::find_if(
          finalExpInfo, [&](const ColorExportInfo &finalExpInfo) { return finalExpInfo.location == location; });
      if (infoIt == finalExpInfo.end())
        continue;
      assert(infoIt->hwColorTarget < MaxColorTargets);
      const unsigned channelWriteMask = key.channelWriteMask[location];
      auto expFmt = static_cast<ExportFormat>(key.expFmt[location]);
      bool needExpInst = (expFmt != 0) && (channelWriteMask > 0 || (location == 0 && needMrt0a));
      if (needExpInst) {
        // Don't collect info for dualSourceBlend just do normal color export, so set the last parameter=false;
        lastExport = handleColorExportInstructions(values[infoIt->hwColorTarget], hwColorExport, builder, expFmt,
                                                   infoIt->isSigned, false);
        finalExportFormats.push_back(expFmt);
        ++hwColorExport;
      }
    }
    if (!lastExport && dummyExport) {
      lastExport = FragColorExport::addDummyExport(builder);
      palMetadata->setPsDummyExport();
      finalExportFormats.push_back(EXP_FORMAT_32_R);
    }
    if (lastExport)
      FragColorExport::setDoneFlag(lastExport, builder);
    builder.CreateRetVoid();
  }

  if (retInst) {
    retInst->eraseFromParent();
  }

  palMetadata->updateSpiShaderColFormat(finalExportFormats);
  palMetadata->updateCbShaderMask(cbShaderMask);
}

// =====================================================================================================================
// Modified the given value so that it is of the given type.
//
// @param value : The value to be modified.
// @param outputTy : The type that the value should be converted to.
// @param builder : The builder object that will be used to create new instructions.
Value *LowerFragColorExport::generateValueForOutput(Value *value, Type *outputTy, BuilderBase &builder) {
  unsigned originalSize = value->getType()->getPrimitiveSizeInBits();
  unsigned finalSize = outputTy->getPrimitiveSizeInBits();
  if (originalSize < finalSize) {
    Type *smallerIntType = IntegerType::get(*m_context, originalSize);
    Type *largerIntType = IntegerType::get(*m_context, finalSize);
    if (smallerIntType != value->getType())
      value = builder.CreateBitCast(value, smallerIntType);
    value = builder.CreateZExt(value, largerIntType);
  }

  if (value->getType() != outputTy) {
    assert(value->getType()->getPrimitiveSizeInBits() == finalSize);
    value = builder.CreateBitCast(value, outputTy);
  }
  return value;
}

// =====================================================================================================================
// Generate a new fragment shader that has the minimum code needed to make PAL happy.
//
// @param [in/out] module : The LLVM module in which to add the shader.
// @param pipelineState : Pipeline state.
// @returns : the entry point for the null fragment shader.
Function *FragColorExport::generateNullFragmentShader(Module &module, PipelineState *pipelineState,
                                                      StringRef entryPointName) {
  Function *entryPoint = generateNullFragmentEntryPoint(module, pipelineState, entryPointName);
  generateNullFragmentShaderBody(entryPoint);
  return entryPoint;
}

// =====================================================================================================================
// Generate a new entry point for a null fragment shader.
//
// @param [in/out] module : The LLVM module in which to add the entry point.
// @param pipelineState : Pipeline state.
// @returns : The new entry point.
Function *FragColorExport::generateNullFragmentEntryPoint(Module &module, PipelineState *pipelineState,
                                                          StringRef entryPointName) {
  FunctionType *entryPointTy = FunctionType::get(Type::getVoidTy(module.getContext()), ArrayRef<Type *>(), false);
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, entryPointName, &module);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(entryPoint, ShaderStage::Fragment);
  entryPoint->setCallingConv(CallingConv::AMDGPU_PS);
  const unsigned waveSize = pipelineState->getShaderWaveSize(ShaderStage::Fragment);
  entryPoint->addFnAttr("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  return entryPoint;
}

// =====================================================================================================================
// Generate the body of the null fragment shader.
//
// @param [in/out] entryPoint : The function in which the code will be inserted.
void FragColorExport::generateNullFragmentShaderBody(llvm::Function *entryPoint) {
  BasicBlock *block = BasicBlock::Create(entryPoint->getContext(), "", entryPoint);
  BuilderBase builder(block);
  builder.CreateRetVoid();
}

// =====================================================================================================================
// Compute color export info.
//
// @param info : The color export information for each color export in no particular order.
// @param pipelineState : Pipeline state
// @returns : Color export info.
FragColorExport::Key FragColorExport::computeKey(ArrayRef<ColorExportInfo> infos, PipelineState *pipelineState) {
  FragColorExport::Key key = {};
  key.enableFragColor = pipelineState->getOptions().enableFragColor;
  key.colorExportState = pipelineState->getColorExportState();
  key.waveSize = pipelineState->getShaderWaveSize(ShaderStage::Fragment);

  if (!infos.empty() && infos[0].hwColorTarget == MaxColorTargets) {
    infos = infos.drop_front(1);
  }

  // DualSourceBlendDynamicEnable has been concluded from driver and compiler sides.
  // 1. Driver set dualSourceBlendDynamicEnable = true when dynamicDualSourceState
  //    feature is enable
  // 2. With Decoration "location=0, index=0(or 1)" in Shader.
  // Only in this way, the DualSourceBlendDynamicEnable can be set true finally.
  bool isDynamicDualSource = key.colorExportState.dualSourceBlendDynamicEnable;

  if (key.enableFragColor) {
    auto &expInfo = infos[0];
    assert(expInfo.ty != nullptr);
    for (unsigned location = 0; location < MaxColorTargets; ++location) {
      if (pipelineState->getColorExportFormat(location, isDynamicDualSource).dfmt != BufDataFormatInvalid) {
        key.expFmt[location] = pipelineState->computeExportFormat(expInfo.ty, location, isDynamicDualSource);
        key.channelWriteMask[location] =
            pipelineState->getColorExportFormat(location, isDynamicDualSource).channelWriteMask;
      }
    }
  } else {
    for (auto &info : infos) {
      key.expFmt[info.location] = pipelineState->computeExportFormat(info.ty, info.location, isDynamicDualSource);
      key.channelWriteMask[info.location] =
          pipelineState->getColorExportFormat(info.location, isDynamicDualSource).channelWriteMask;
    }
  }

  return key;
}
