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
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "llpc-frag-color-export"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
// @param module : LLVM module
FragColorExport::FragColorExport(PipelineState *pipelineState, Module *module)
    : m_pipelineState(pipelineState), m_context(module ? &module->getContext() : nullptr) {
}

// =====================================================================================================================
// Executes fragment color export operations based on the specified output type and its location.
//
// @param output : Fragment color output
// @param location : Location of fragment color output
// @param insertPos : Where to insert fragment color export instructions
Value *FragColorExport::run(Value *output, unsigned location, Instruction *insertPos) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);

  Type *outputTy = output->getType();
  const unsigned origLoc = resUsage->inOutUsage.fs.outputOrigLocs[location];

  ExportFormat expFmt = EXP_FORMAT_ZERO;
  if (m_pipelineState->getColorExportState().dualSourceBlendEnable) {
    // Dual source blending is enabled
    expFmt = computeExportFormat(outputTy, 0);
  } else
    expFmt = computeExportFormat(outputTy, origLoc);

  resUsage->inOutUsage.fs.expFmts[location] = expFmt;
  if (expFmt == EXP_FORMAT_ZERO) {
    // Clear channel mask if shader export format is ZERO
    resUsage->inOutUsage.fs.cbShaderMask &= ~(0xF << (4 * origLoc));
  }

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  BasicType outputType = resUsage->inOutUsage.fs.outputTypes[origLoc];
  const bool signedness =
      (outputType == BasicType::Int8 || outputType == BasicType::Int16 || outputType == BasicType::Int);

  auto compTy = outputTy->isVectorTy() ? outputTy->getVectorElementType() : outputTy;
  unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() : 1;

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
  const auto undefFloat16x2 = UndefValue::get(VectorType::get(Type::getHalfTy(*m_context), 2));

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
      comps[0] = emitCall("llvm.amdgcn.cvt.pkrtz", VectorType::get(Type::getHalfTy(*m_context), 2),
                          {comps[0], comps[1]}, attribs, insertPos);

      if (compCount > 2) {
        comps[1] = emitCall("llvm.amdgcn.cvt.pkrtz", VectorType::get(Type::getHalfTy(*m_context), 2),
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
      Value *packedComps =
          emitCall(funcName, VectorType::get(Type::getInt16Ty(*m_context), 2), {comps[i], comps[i + 1]}, {}, insertPos);

      packedComps = new BitCastInst(packedComps, VectorType::get(Type::getHalfTy(*m_context), 2), "", insertPos);

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
      Value *packedComps =
          emitCall(funcName, VectorType::get(Type::getInt16Ty(*m_context), 2), {comps[i], comps[i + 1]}, {}, insertPos);

      packedComps = new BitCastInst(packedComps, VectorType::get(Type::getHalfTy(*m_context), 2), "", insertPos);

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
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_MRT_0 + location), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), compCount > 2 ? 0xF : 0x3),   // en
        comps[0],                                                                    // src0
        comps[1],                                                                    // src1
        ConstantInt::get(Type::getInt1Ty(*m_context), false),                        // done
        ConstantInt::get(Type::getInt1Ty(*m_context), true)                          // vm
    };

    exportCall = emitCall("llvm.amdgcn.exp.compr.v2f16", Type::getVoidTy(*m_context), args, {}, insertPos);
  } else {
    // 32-bit export
    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_MRT_0 + location), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), (1 << compCount) - 1),        // en
        comps[0],                                                                    // src0
        comps[1],                                                                    // src1
        comps[2],                                                                    // src2
        comps[3],                                                                    // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),                        // done
        ConstantInt::get(Type::getInt1Ty(*m_context), true)                          // vm
    };

    exportCall = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
  }

  return exportCall;
}

// =====================================================================================================================
// Determines the shader export format for a particular fragment color output. Value should be used to do programming
// for SPI_SHADER_COL_FORMAT.
//
// @param outputTy : Type of fragment data output
// @param location : Location of fragment data output
ExportFormat FragColorExport::computeExportFormat(Type *outputTy, unsigned location) const {
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  auto gpuWorkarounds = &m_pipelineState->getTargetInfo().getGpuWorkarounds();
  unsigned outputMask = outputTy->isVectorTy() ? (1 << outputTy->getVectorNumElements()) - 1 : 1;
  const auto cbState = &m_pipelineState->getColorExportState();
  const auto target = &m_pipelineState->getColorExportFormat(location);
  // NOTE: Alpha-to-coverage only takes effect for outputs from color target 0.
  const bool enableAlphaToCoverage = (cbState->alphaToCoverageEnable && location == 0);

  const bool blendEnabled = target->blendEnable;

  const bool isUnormFormat = (target->nfmt == BufNumFormatUnorm);
  const bool isSnormFormat = (target->nfmt == BufNumFormatSnorm);
  bool isFloatFormat = (target->nfmt == BufNumFormatFloat);
  const bool isUintFormat = (target->nfmt == BufNumFormatUint);
  const bool isSintFormat = (target->nfmt == BufNumFormatSint);
  const bool isSrgbFormat = (target->nfmt == BufNumFormatSrgb);

  if (target->dfmt == BufDataFormat8_8_8 || target->dfmt == BufDataFormat8_8_8_Bgr) {
    // These three-byte formats are handled by pretending they are float.
    isFloatFormat = true;
  }

  const unsigned maxCompBitCount = getMaxComponentBitCount(target->dfmt);

  const bool formatHasAlpha = hasAlpha(target->dfmt);
  const bool alphaExport =
      (outputMask == 0xF && (formatHasAlpha || target->blendSrcAlphaToColor || enableAlphaToCoverage));

  const CompSetting compSetting = computeCompSetting(target->dfmt);

  // Start by assuming EXP_FORMAT_ZERO (no exports)
  ExportFormat expFmt = EXP_FORMAT_ZERO;

  bool gfx8RbPlusEnable = false;
  if (gfxIp.major == 8 && gfxIp.minor == 1)
    gfx8RbPlusEnable = true;

  if (target->dfmt == BufDataFormatInvalid)
    expFmt = EXP_FORMAT_ZERO;
  else if (compSetting == CompSetting::OneCompRed && !alphaExport && !isSrgbFormat &&
           (!gfx8RbPlusEnable || maxCompBitCount == 32)) {
    // NOTE: When Rb+ is enabled, "R8 UNORM" and "R16 UNORM" shouldn't use "EXP_FORMAT_32_R", instead
    // "EXP_FORMAT_FP16_ABGR" and "EXP_FORMAT_UNORM16_ABGR" should be used for 2X exporting performance.
    expFmt = EXP_FORMAT_32_R;
  } else if (((isUnormFormat || isSnormFormat) && maxCompBitCount <= 10) || (isFloatFormat && maxCompBitCount <= 16) ||
             (isSrgbFormat && maxCompBitCount == 8))
    expFmt = EXP_FORMAT_FP16_ABGR;
  else if (isSintFormat &&
           (maxCompBitCount == 16 ||
            (!static_cast<bool>(gpuWorkarounds->gfx6.cbNoLt16BitIntClamp) && maxCompBitCount < 16)) &&
           !enableAlphaToCoverage) {
    // NOTE: On some hardware, the CB will not properly clamp its input if the shader export format is "UINT16"
    // "SINT16" and the CB format is less than 16 bits per channel. On such hardware, the workaround is picking
    // an appropriate 32-bit export format. If this workaround isn't necessary, then we can choose this higher
    // performance 16-bit export format in this case.
    expFmt = EXP_FORMAT_SINT16_ABGR;
  } else if (isSnormFormat && maxCompBitCount == 16 && !blendEnabled)
    expFmt = EXP_FORMAT_SNORM16_ABGR;
  else if (isUintFormat &&
           (maxCompBitCount == 16 ||
            (!static_cast<bool>(gpuWorkarounds->gfx6.cbNoLt16BitIntClamp) && maxCompBitCount < 16)) &&
           !enableAlphaToCoverage) {
    // NOTE: On some hardware, the CB will not properly clamp its input if the shader export format is "UINT16"
    // "SINT16" and the CB format is less than 16 bits per channel. On such hardware, the workaround is picking
    // an appropriate 32-bit export format. If this workaround isn't necessary, then we can choose this higher
    // performance 16-bit export format in this case.
    expFmt = EXP_FORMAT_UINT16_ABGR;
  } else if (isUnormFormat && maxCompBitCount == 16 && !blendEnabled)
    expFmt = EXP_FORMAT_UNORM16_ABGR;
  else if (((isUintFormat || isSintFormat) || (isFloatFormat && maxCompBitCount > 16) ||
            ((isUnormFormat || isSnormFormat) && maxCompBitCount == 16)) &&
           (compSetting == CompSetting::OneCompRed || compSetting == CompSetting::OneCompAlpha ||
            compSetting == CompSetting::TwoCompAlphaRed))
    expFmt = EXP_FORMAT_32_AR;
  else if (((isUintFormat || isSintFormat) || (isFloatFormat && maxCompBitCount > 16) ||
            ((isUnormFormat || isSnormFormat) && maxCompBitCount == 16)) &&
           compSetting == CompSetting::TwoCompGreenRed && !alphaExport)
    expFmt = EXP_FORMAT_32_GR;
  else if (((isUnormFormat || isSnormFormat) && maxCompBitCount == 16) || (isUintFormat || isSintFormat) ||
           (isFloatFormat && maxCompBitCount > 16))
    expFmt = EXP_FORMAT_32_ABGR;

  return expFmt;
}

// =====================================================================================================================
// This is the helper function for the algorithm to determine the shader export format.
//
// @param dfmt : Color attachment data format
CompSetting FragColorExport::computeCompSetting(BufDataFormat dfmt) {
  CompSetting compSetting = CompSetting::Invalid;
  switch (getNumChannels(dfmt)) {
  case 1:
    compSetting = CompSetting::OneCompRed;
    break;
  case 2:
    compSetting = CompSetting::TwoCompGreenRed;
    break;
  }
  return compSetting;
}

// =====================================================================================================================
// Get the number of channels
//
// @param dfmt : Color attachment data format
unsigned FragColorExport::getNumChannels(BufDataFormat dfmt) {
  switch (dfmt) {
  case BufDataFormatInvalid:
  case BufDataFormatReserved:
  case BufDataFormat8:
  case BufDataFormat16:
  case BufDataFormat32:
  case BufDataFormat64:
    return 1;
  case BufDataFormat4_4:
  case BufDataFormat8_8:
  case BufDataFormat16_16:
  case BufDataFormat32_32:
  case BufDataFormat64_64:
    return 2;
  case BufDataFormat8_8_8:
  case BufDataFormat8_8_8_Bgr:
  case BufDataFormat10_11_11:
  case BufDataFormat11_11_10:
  case BufDataFormat32_32_32:
  case BufDataFormat64_64_64:
  case BufDataFormat5_6_5:
  case BufDataFormat5_6_5_Bgr:
    return 3;
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
  case BufDataFormat8_8_8_8:
  case BufDataFormat16_16_16_16:
  case BufDataFormat32_32_32_32:
  case BufDataFormat8_8_8_8_Bgra:
  case BufDataFormat2_10_10_10_Bgra:
  case BufDataFormat64_64_64_64:
  case BufDataFormat4_4_4_4:
  case BufDataFormat4_4_4_4_Bgra:
  case BufDataFormat5_6_5_1:
  case BufDataFormat5_6_5_1_Bgra:
  case BufDataFormat1_5_6_5:
  case BufDataFormat5_9_9_9:
    return 4;
  }
  return 0;
}

// =====================================================================================================================
// Checks whether the alpha channel is present in the specified color attachment format.
//
// @param dfmt : Color attachment data format
bool FragColorExport::hasAlpha(BufDataFormat dfmt) {
  switch (dfmt) {
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
  case BufDataFormat8_8_8_8:
  case BufDataFormat16_16_16_16:
  case BufDataFormat32_32_32_32:
  case BufDataFormat8_8_8_8_Bgra:
  case BufDataFormat2_10_10_10_Bgra:
  case BufDataFormat64_64_64_64:
  case BufDataFormat4_4_4_4:
  case BufDataFormat4_4_4_4_Bgra:
  case BufDataFormat5_6_5_1:
  case BufDataFormat5_6_5_1_Bgra:
  case BufDataFormat1_5_6_5:
  case BufDataFormat5_9_9_9:
    return true;
  default:
    return false;
  }
}

// =====================================================================================================================
// Gets the maximum bit-count of any component in specified color attachment format.
//
// @param dfmt : Color attachment data format
unsigned FragColorExport::getMaxComponentBitCount(BufDataFormat dfmt) {
  switch (dfmt) {
  case BufDataFormatInvalid:
  case BufDataFormatReserved:
    return 0;
  case BufDataFormat4_4:
  case BufDataFormat4_4_4_4:
  case BufDataFormat4_4_4_4_Bgra:
    return 4;
  case BufDataFormat5_6_5:
  case BufDataFormat5_6_5_Bgr:
  case BufDataFormat5_6_5_1:
  case BufDataFormat5_6_5_1_Bgra:
  case BufDataFormat1_5_6_5:
    return 6;
  case BufDataFormat8:
  case BufDataFormat8_8:
  case BufDataFormat8_8_8:
  case BufDataFormat8_8_8_Bgr:
  case BufDataFormat8_8_8_8:
  case BufDataFormat8_8_8_8_Bgra:
    return 8;
  case BufDataFormat5_9_9_9:
    return 9;
  case BufDataFormat10_10_10_2:
  case BufDataFormat2_10_10_10:
  case BufDataFormat2_10_10_10_Bgra:
    return 10;
  case BufDataFormat10_11_11:
  case BufDataFormat11_11_10:
    return 11;
  case BufDataFormat16:
  case BufDataFormat16_16:
  case BufDataFormat16_16_16_16:
    return 16;
  case BufDataFormat32:
  case BufDataFormat32_32:
  case BufDataFormat32_32_32:
  case BufDataFormat32_32_32_32:
    return 32;
  case BufDataFormat64:
  case BufDataFormat64_64:
  case BufDataFormat64_64_64:
  case BufDataFormat64_64_64_64:
    return 64;
  }
  return 0;
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
