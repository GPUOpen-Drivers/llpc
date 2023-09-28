/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
FragColorExport::FragColorExport(LLVMContext *context, PipelineState *pipelineState)
    : m_context(context), m_pipelineState(pipelineState) {
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
static void extractElements(Value *input, BuilderBase &builder, SmallVectorImpl<Value *> &results) {
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
Value *FragColorExport::handleColorExportInstructions(Value *output, unsigned hwColorExport, BuilderBase &builder,
                                                      ExportFormat expFmt, const bool signedness) {
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

  SmallVector<Value *, 4> comps(4);

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

  const auto undefFloat = PoisonValue::get(builder.getFloatTy());
  const auto undefFloat16x2 = PoisonValue::get(FixedVectorType::get(builder.getHalfTy(), 2));

  switch (expFmt) {
  case EXP_FORMAT_32_R: {
    compCount = 1;
    comps[0] = convertToFloat(comps[0], signedness, builder);
    break;
  }
  case EXP_FORMAT_32_GR: {
    if (compCount >= 2) {
      compCount = 2;
      comps[0] = convertToFloat(comps[0], signedness, builder);
      comps[1] = convertToFloat(comps[1], signedness, builder);
    } else {
      compCount = 1;
      comps[0] = convertToFloat(comps[0], signedness, builder);
    }
    break;
  }
  case EXP_FORMAT_32_AR: {
    if (compCount == 4) {
      compCount = 2;
      comps[0] = convertToFloat(comps[0], signedness, builder);
      comps[1] = convertToFloat(comps[3], signedness, builder);
    } else {
      compCount = 1;
      comps[0] = convertToFloat(comps[0], signedness, builder);
    }
    break;
  }
  case EXP_FORMAT_32_ABGR: {
    for (unsigned i = 0; i < compCount; ++i)
      comps[i] = convertToFloat(comps[i], signedness, builder);

    for (unsigned i = compCount; i < 4; ++i)
      comps[i] = undefFloat;
    break;
  }
  case EXP_FORMAT_FP16_ABGR: {
    // convert to half type
    if (bitWidth <= 16) {
      output = convertToHalf(output, signedness, builder);
      extractElements(output, builder, comps);
      // re-pack
      comps[0] = builder.CreateInsertElement(undefFloat16x2, comps[0], builder.getInt32(0));
      comps[0] = builder.CreateInsertElement(comps[0], comps[1], builder.getInt32(1));
      if (compCount > 2) {
        comps[1] = builder.CreateInsertElement(undefFloat16x2, comps[2], builder.getInt32(0));
        comps[1] = builder.CreateInsertElement(comps[1], comps[3], builder.getInt32(1));
      }
    } else {
      if (outputTy->isIntOrIntVectorTy())
        output =
            builder.CreateBitCast(output, outputTy->isVectorTy() ? FixedVectorType::get(builder.getFloatTy(), compCount)
                                                                 : builder.getFloatTy());
      extractElements(output, builder, comps);

      Attribute::AttrKind attribs[] = {Attribute::ReadNone};
      comps[0] = builder.CreateNamedCall("llvm.amdgcn.cvt.pkrtz", FixedVectorType::get(builder.getHalfTy(), 2),
                                         {comps[0], comps[1]}, attribs);
      if (compCount > 2)
        comps[1] = builder.CreateNamedCall("llvm.amdgcn.cvt.pkrtz", FixedVectorType::get(builder.getHalfTy(), 2),
                                           {comps[2], comps[3]}, attribs);
    }
    break;
  }
  case EXP_FORMAT_UNORM16_ABGR:
  case EXP_FORMAT_SNORM16_ABGR: {
    output = convertToFloat(output, signedness, builder);
    extractElements(output, builder, comps);

    StringRef funcName =
        expFmt == EXP_FORMAT_SNORM16_ABGR ? "llvm.amdgcn.cvt.pknorm.i16" : "llvm.amdgcn.cvt.pknorm.u16";

    for (unsigned idx = 0; idx < (compCount + 1) / 2; idx++) {
      Value *packedComps = builder.CreateNamedCall(funcName, FixedVectorType::get(builder.getInt16Ty(), 2),
                                                   {comps[2 * idx], comps[2 * idx + 1]}, {});

      comps[idx] = builder.CreateBitCast(packedComps, FixedVectorType::get(builder.getHalfTy(), 2));
    }

    break;
  }
  case EXP_FORMAT_UINT16_ABGR:
  case EXP_FORMAT_SINT16_ABGR: {
    assert(compCount <= 4);
    output = convertToInt(output, signedness, builder);
    extractElements(output, builder, comps);

    StringRef funcName = expFmt == EXP_FORMAT_SINT16_ABGR ? "llvm.amdgcn.cvt.pk.i16" : "llvm.amdgcn.cvt.pk.u16";

    for (unsigned idx = 0; idx < (compCount + 1) / 2; idx++) {
      Value *packedComps = builder.CreateNamedCall(funcName, FixedVectorType::get(builder.getInt16Ty(), 2),
                                                   {comps[2 * idx], comps[2 * idx + 1]}, {});

      comps[idx] = builder.CreateBitCast(packedComps, FixedVectorType::get(builder.getHalfTy(), 2));
    }

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11 &&
      (m_pipelineState->getColorExportState().dualSourceBlendEnable ||
       m_pipelineState->getColorExportState().dynamicDualSourceBlendEnable)) {
    // Save them for later dual-source-swizzle
    m_blendSourceChannels = exportTy->isHalfTy() ? (compCount + 1) / 2 : compCount;
    assert(hwColorExport <= 1);
    m_blendSources[hwColorExport].append(comps.begin(), comps.end());
    return nullptr;
  }

  Value *exportCall = nullptr;

  if (exportTy->isHalfTy()) {
    // GFX11 removes compressed export, simply use 32bit-data export.
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11) {
      // Translate compCount into the number of 32bit data.
      compCount = (compCount + 1) / 2;
      for (unsigned i = 0; i < compCount; i++)
        comps[i] = builder.CreateBitCast(comps[i], builder.getFloatTy());
      for (unsigned i = compCount; i < 4; i++)
        comps[i] = undefFloat;

      Value *args[] = {
          builder.getInt32(EXP_TARGET_MRT_0 + hwColorExport), // tgt
          builder.getInt32((1 << compCount) - 1),             // en
          comps[0],                                           // src0
          comps[1],                                           // src1
          comps[2],                                           // src2
          comps[3],                                           // src3
          builder.getFalse(),                                 // done
          builder.getTrue()                                   // vm
      };

      return builder.CreateNamedCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {});
    }

    // 16-bit export (compressed)
    if (compCount <= 2)
      comps[1] = undefFloat16x2;
    Value *args[] = {
        builder.getInt32(EXP_TARGET_MRT_0 + hwColorExport), // tgt
        builder.getInt32(compCount > 2 ? 0xF : 0x3),        // en
        comps[0],                                           // src0
        comps[1],                                           // src1
        builder.getFalse(),                                 // done
        builder.getTrue()                                   // vm
    };

    exportCall = builder.CreateNamedCall("llvm.amdgcn.exp.compr.v2f16", Type::getVoidTy(*m_context), args, {});
  } else {
    // 32-bit export
    for (unsigned i = compCount; i < 4; i++)
      comps[i] = undefFloat;

    Value *args[] = {
        builder.getInt32(EXP_TARGET_MRT_0 + hwColorExport), // tgt
        builder.getInt32((1 << compCount) - 1),             // en
        comps[0],                                           // src0
        comps[1],                                           // src1
        comps[2],                                           // src2
        comps[3],                                           // src3
        builder.getFalse(),                                 // done
        builder.getTrue()                                   // vm
    };

    exportCall = builder.CreateNamedCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {});
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
  if (runImpl(module, pipelineShaders, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the lower color export pass on a module
//
// @param [in/out] module : Module
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool LowerFragColorExport::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                   PipelineState *pipelineState) {
  m_context = &module.getContext();
  m_pipelineState = pipelineState;
  m_resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);

  Function *fragEntryPoint = pipelineShaders.getEntryPoint(ShaderStageFragment);
  if (!fragEntryPoint)
    return false;

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
    return false;

  BuilderBase builder(module.getContext());
  builder.SetInsertPoint(retInst);

  collectExportInfoForBuiltinOutput(fragEntryPoint, builder);
  collectExportInfoForGenericOutputs(fragEntryPoint, builder);

  bool willGenerateColorExportShader = m_pipelineState->isUnlinked() && !m_pipelineState->hasColorExportFormats();
  if (willGenerateColorExportShader && !m_info.empty()) {
    if (m_pipelineState->getOptions().enableColorExportShader)
      jumpColorExport(fragEntryPoint, builder);
    else
      generateReturn(fragEntryPoint, builder);
    return true;
  }

  FragColorExport fragColorExport(m_context, m_pipelineState);
  SmallVector<ExportFormat, 8> exportFormat(MaxColorTargets + 1, EXP_FORMAT_ZERO);
  for (auto &exp : m_info) {
    exportFormat[exp.hwColorTarget] =
        static_cast<ExportFormat>(m_pipelineState->computeExportFormat(exp.ty, exp.location));
  }
  bool dummyExport =
      (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 10 || m_resUsage->builtInUsage.fs.discard);
  fragColorExport.generateExportInstructions(m_info, m_exportValues, exportFormat, dummyExport, builder);
  return !m_info.empty() || dummyExport;
}

// =====================================================================================================================
// Updates the value in the entry in expFragColors that callInst is writing to.
//
// @param callInst : An call to the generic output export builtin in a fragment shader.
// @param [in/out] expFragColors : An array with the current color export information for each hw color target.
void LowerFragColorExport::updateFragColors(CallInst *callInst, ColorExportValueInfo expFragColors[],
                                            BuilderBase &builder) {
  const unsigned location = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
  const unsigned component = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue();
  Value *output = callInst->getOperand(2);
  assert(output->getType()->getScalarSizeInBits() <= 32); // 64-bit output is not allowed

  InOutLocationInfo origLocInfo;
  origLocInfo.setLocation(location);
  origLocInfo.setComponent(component);
  auto locInfoMapIt = m_resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
  if (locInfoMapIt == m_resUsage->inOutUsage.outputLocInfoMap.end())
    return;
  unsigned hwColorTarget = locInfoMapIt->second.getLocation();

  Type *outputTy = output->getType();

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);
  (void(bitWidth)); // unused

  auto compTy = outputTy->isVectorTy() ? cast<VectorType>(outputTy)->getElementType() : outputTy;
  unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;

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

  while (component + compCount > expFragColor.value.size())
    expFragColor.value.push_back(PoisonValue::get(compTy));

  for (unsigned i = 0; i < compCount; ++i)
    expFragColor.value[component + i] = outputComps[i];

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
  const unsigned channelMask = ((1 << compCount) - 1);
  m_resUsage->inOutUsage.fs.cbShaderMask |= (channelMask << (4 * location));

  // Construct exported fragment colors
  if (compCount == 1)
    output = expFragColor[0];
  else {
    const auto compTy = expFragColor[0]->getType();

    output = PoisonValue::get(FixedVectorType::get(compTy, compCount));
    for (unsigned i = 0; i < compCount; ++i) {
      assert(expFragColor[i]->getType() == compTy);
      output = builder.CreateInsertElement(output, expFragColor[i], i);
    }
  }
  return output;
}

// =====================================================================================================================
// Collects the information needed to generate the export instructions for all of the generic outputs of the fragment
// shader fragEntryPoint.  This information is stored in m_info and m_exportValues.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
void LowerFragColorExport::collectExportInfoForGenericOutputs(Function *fragEntryPoint, BuilderBase &builder) {
  std::unique_ptr<FragColorExport> fragColorExport(new FragColorExport(m_context, m_pipelineState));
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

  if (colorExports.empty())
    return;

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

  // Recombine the values being exported for each hw color target.
  for (unsigned hwColorTarget = 0; hwColorTarget < MaxColorTargets; ++hwColorTarget) {
    unsigned location = m_resUsage->inOutUsage.fs.outputOrigLocs[hwColorTarget];
    if (location == InvalidValue)
      m_exportValues[hwColorTarget] = nullptr;
    else
      m_exportValues[hwColorTarget] = getOutputValue(expFragColors[hwColorTarget].value, location, builder);
  }

  // Add the color export information to the palmetadata.
  for (unsigned hwColorTarget = 0; hwColorTarget < MaxColorTargets; ++hwColorTarget) {
    Value *output = m_exportValues[hwColorTarget];
    if (!output)
      continue;
    const ColorExportValueInfo &colorExportInfo = expFragColors[hwColorTarget];
    m_info.push_back({hwColorTarget, colorExportInfo.location, colorExportInfo.isSigned, output->getType()});
  }
}

// =====================================================================================================================
// Generates a return instruction that will make all of the values for the exports available to the color export shader.
// The color export information is added to the pal metadata, so that everything needed to generate the color export
// shader is available.
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
Value *LowerFragColorExport::generateReturn(Function *fragEntryPoint, BuilderBase &builder) {
  // Add the export info to be used when linking shaders to generate the color export shader and compute the spi shader
  // color format in the metadata.
  m_pipelineState->getPalMetadata()->addColorExportInfo(m_info);

  ReturnInst *retInst = cast<ReturnInst>(builder.GetInsertPoint()->getParent()->getTerminator());

  // First build the return type for the fragment shader.
  SmallVector<Type *, 8> outputTypes;
  for (const ColorExportInfo &info : m_info) {
    outputTypes.push_back(getVgprTy(info.ty));
  }
  Type *retTy = StructType::get(*m_context, outputTypes);
  addFunctionArgs(fragEntryPoint, retTy, {}, {});

  // Now build the return value.
  Value *retVal = PoisonValue::get(retTy);
  unsigned returnLocation = 0;
  for (unsigned idx = 0; idx < m_info.size(); ++idx) {
    const ColorExportInfo &info = m_info[idx];
    unsigned hwColorTarget = info.hwColorTarget;
    Value *output = m_exportValues[hwColorTarget];
    if (!output)
      continue;
    if (output->getType() != outputTypes[idx])
      output = generateValueForOutput(output, outputTypes[idx], builder);
    retVal = builder.CreateInsertValue(retVal, output, returnLocation);
    ++returnLocation;
  }
  retVal = builder.CreateRet(retVal);
  retInst->eraseFromParent();
  return retVal;
}

// =====================================================================================================================
// Jump to color export shader if explicitly build color export shader
//
// @param fragEntryPoint : The fragment shader to which we should add the export instructions.
// @param builder : The builder object that will be used to create new instructions.
llvm::Value *LowerFragColorExport::jumpColorExport(llvm::Function *fragEntryPoint, BuilderBase &builder) {

  // Add the export info to be used when linking shaders to generate the color export shader and compute the spi shader
  // color format in the metadata.
  m_pipelineState->getPalMetadata()->addColorExportInfo(m_info);
  m_pipelineState->getPalMetadata()->setDiscardState(m_resUsage->builtInUsage.fs.discard);

  // First build the argument type for the fragment shader.
  SmallVector<Type *, 8> outputTypes;
  for (const ColorExportInfo &info : m_info) {
    outputTypes.push_back(getVgprTy(info.ty));
  }
  Type *argTy = StructType::get(*m_context, outputTypes);

  // Now build the argument value.
  Value *argVal = PoisonValue::get(argTy);
  unsigned returnLocation = 0;
  for (unsigned idx = 0; idx < m_info.size(); ++idx) {
    const ColorExportInfo &info = m_info[idx];
    unsigned hwColorTarget = info.hwColorTarget;
    Value *output = m_exportValues[hwColorTarget];
    if (!output)
      continue;
    if (output->getType() != outputTypes[idx])
      output = generateValueForOutput(output, outputTypes[idx], builder);
    argVal = builder.CreateInsertValue(argVal, output, returnLocation);
    ++returnLocation;
  }

  // Build color export function type
  auto funcTy = FunctionType::get(Type::getVoidTy(*m_context), {argTy}, false);

  // Convert color export shader address to function pointer
  auto funcTyPtr = funcTy->getPointerTo(ADDR_SPACE_CONST);
  auto colorShaderAddr = ShaderInputs::getSpecialUserData(UserDataMapping::ColorExportAddr, builder);
  AddressExtender addrExt(builder.GetInsertPoint()->getParent()->getParent());
  auto funcPtr = addrExt.extendWithPc(colorShaderAddr, funcTyPtr, builder);

  // Jump
  auto callInst = builder.CreateCall(funcTy, funcPtr, argVal);
  callInst->setCallingConv(CallingConv::AMDGPU_Gfx);
  callInst->setDoesNotReturn();
  callInst->setOnlyWritesMemory();
  return callInst;
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
    if (!func.isDeclaration() || !func.getName().startswith(lgcName::OutputExportBuiltIn))
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
Value *FragColorExport::dualSourceSwizzle(BuilderBase &builder) {
  Value *result0[4], *result1[4];
  unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageFragment);
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
  builder.CreateNamedCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args0, {});

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
  return builder.CreateNamedCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args1, {});
}

// =====================================================================================================================
// Generates the export instructions based on the given color export information.
//
// @param info : The color export information for each color export in no particular order.
// @param values : The values that are to be exported.  Indexed by the hw color target.
// @param exportFormat : The export format for each color target. Indexed by the hw color target.
// @param builder : The builder object that will be used to create new instructions.
void FragColorExport::generateExportInstructions(ArrayRef<lgc::ColorExportInfo> info, ArrayRef<llvm::Value *> values,
                                                 ArrayRef<ExportFormat> exportFormat, bool dummyExport,
                                                 BuilderBase &builder) {
  SmallVector<ExportFormat> finalExportFormats;
  Value *lastExport = nullptr;
  unsigned hwColorExport = 0;
  for (const ColorExportInfo &exp : info) {
    Value *output = values[exp.hwColorTarget];
    if (exp.hwColorTarget != MaxColorTargets) {
      ExportFormat expFmt = exportFormat[exp.hwColorTarget];
      if (expFmt != EXP_FORMAT_ZERO) {
        lastExport = handleColorExportInstructions(output, hwColorExport, builder, expFmt, exp.isSigned);
        finalExportFormats.push_back(expFmt);
        ++hwColorExport;
      }
    } else {
      // Depth export alpha comes from MRT0.a if there is MRT0.a and A2C is enable on GFX11+
      Value *alpha = PoisonValue::get(Type::getFloatTy(*m_context));
      unsigned depthMask = exp.location;
      if (!dummyExport && m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11 &&
          m_pipelineState->getColorExportState().alphaToCoverageEnable) {
        bool canCopyAlpha = false;
        for (auto &curInfo : info) {
          if (curInfo.hwColorTarget == EXP_TARGET_MRT_0 && (exportFormat[EXP_TARGET_MRT_0] > EXP_FORMAT_32_GR)) {
            // Mrt0 is enabled and its alpha channel is enabled
            canCopyAlpha = true;
            break;
          }
        }
        if (canCopyAlpha) {
          // Update Mrtz.a and its mask
          alpha = builder.CreateExtractElement(values[EXP_TARGET_MRT_0], 3);
          depthMask |= 0x8;
        }
      }
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

      m_pipelineState->getPalMetadata()->setSpiShaderZFormat(depthExpFmt);
    }
  }

  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11 &&
      (m_pipelineState->getColorExportState().dualSourceBlendEnable ||
       m_pipelineState->getColorExportState().dynamicDualSourceBlendEnable))
    lastExport = dualSourceSwizzle(builder);

  if (!lastExport && dummyExport) {
    lastExport = FragColorExport::addDummyExport(builder);
    finalExportFormats.push_back(EXP_FORMAT_32_R);
  }

  if (lastExport)
    FragColorExport::setDoneFlag(lastExport, builder);

  m_pipelineState->getPalMetadata()->updateSpiShaderColFormat(finalExportFormats);
  m_pipelineState->getPalMetadata()->updateCbShaderMask(info);
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
  setShaderStage(entryPoint, ShaderStageFragment);
  entryPoint->setCallingConv(CallingConv::AMDGPU_PS);
  if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 10) {
    const unsigned waveSize = pipelineState->getShaderWaveSize(ShaderStageFragment);
    entryPoint->addFnAttr("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  }
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
