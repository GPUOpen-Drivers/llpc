/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ColorExportShader.cpp
 * @brief LGC source file: The class to generate the color export shader used when linking a pipeline.
 ***********************************************************************************************************************
 */

#include "ColorExportShader.h"
#include "lgc/patch/FragColorExport.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Target/TargetMachine.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Constructor. This is where we store all the information needed to generate the export shader; other methods
// do not need to look at PipelineState.
ColorExportShader::ColorExportShader(PipelineState *pipelineState, ArrayRef<ColorExportInfo> exports)
    : GlueShader(pipelineState), m_killEnabled(false) {
  m_exports.append(exports.begin(), exports.end());

  if (!pipelineState->getOptions().enableColorExportShader) {
    PalMetadata *metadata = pipelineState->getPalMetadata();
    auto dbShaderControl = metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                               .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::DbShaderControl]
                               .getMap(true);
    m_killEnabled = dbShaderControl[Util::Abi::DbShaderControlMetadataKey::KillEnable].getBool();
  }
  m_key = FragColorExport::computeKey(exports, pipelineState);
}

// =====================================================================================================================
// Get the string for this color export shader. This is some encoding or hash of the inputs to the
// createColorExportShader function that the front-end client can use as a cache key to avoid compiling the same glue
// shader more than once.
StringRef ColorExportShader::getString() {
  if (m_shaderString.empty()) {
    constexpr uint32_t estimatedTypeSize = 10;
    uint32_t sizeEstimate = (sizeof(ColorExportInfo) + estimatedTypeSize) * m_exports.size();
    // gfxIP.major
    sizeEstimate += sizeof(unsigned);
    sizeEstimate += sizeof(m_killEnabled);
    sizeEstimate += sizeof(m_pipelineState->getOptions().enableColorExportShader);

    // ColorExportState + MaxColorTargets * (expfmt + writeMask)
    sizeEstimate += sizeof(ColorExportState) + MaxColorTargets * (sizeof(unsigned) * 2);
    if (m_key.colorExportState.dualSourceBlendDynamicEnable || m_key.colorExportState.dualSourceBlendEnable) {
      sizeEstimate += sizeof(m_key.waveSize);
    }
    m_shaderString.reserve(sizeEstimate);

    for (ColorExportInfo colorExportInfo : m_exports) {
      m_shaderString += StringRef(reinterpret_cast<const char *>(&colorExportInfo.hwColorTarget),
                                  sizeof(colorExportInfo.hwColorTarget));
      m_shaderString +=
          StringRef(reinterpret_cast<const char *>(&colorExportInfo.isSigned), sizeof(colorExportInfo.isSigned));
      m_shaderString +=
          StringRef(reinterpret_cast<const char *>(&colorExportInfo.location), sizeof(colorExportInfo.location));
      m_shaderString += getTypeName(colorExportInfo.ty);
    }
    unsigned gfxip = m_lgcContext->getTargetInfo().getGfxIpVersion().major;
    m_shaderString += StringRef(reinterpret_cast<const char *>(&gfxip), sizeof(unsigned));
    m_shaderString += StringRef(reinterpret_cast<const char *>(&m_killEnabled), sizeof(m_killEnabled));
    m_shaderString += StringRef(reinterpret_cast<const char *>(&m_pipelineState->getOptions().enableColorExportShader),
                                sizeof(m_pipelineState->getOptions().enableColorExportShader));

    const ColorExportState *colorExportState = &m_key.colorExportState;
    m_shaderString += StringRef(reinterpret_cast<const char *>(colorExportState), sizeof(*colorExportState));
    for (unsigned location = 0; location < MaxColorTargets; ++location) {
      unsigned expFmt = m_key.expFmt[location];
      unsigned writeMask = m_key.channelWriteMask[location];
      m_shaderString += StringRef(reinterpret_cast<const char *>(&expFmt), sizeof(expFmt));
      m_shaderString += StringRef(reinterpret_cast<const char *>(&writeMask), sizeof(writeMask));
    }

    if (m_key.colorExportState.dualSourceBlendDynamicEnable || m_key.colorExportState.dualSourceBlendEnable) {
      unsigned waveSize = m_key.waveSize;
      m_shaderString += StringRef(reinterpret_cast<const char *>(&waveSize), sizeof(waveSize));
    }
  }
  return m_shaderString;
}

// =====================================================================================================================
// Get the symbol name of the main shader that this glue shader is prolog or epilog for
StringRef ColorExportShader::getMainShaderName() {
  return getEntryPointName(CallingConv::AMDGPU_PS, /*isFetchlessVs=*/false);
}

// =====================================================================================================================
// Generate the IR module for the color export shader
Module *ColorExportShader::generate() {
  // Create the function.
  Function *colorExportFunc = createColorExportFunc();

  // Process each fragment output.
  FragColorExport fragColorExport(m_lgcContext);
  auto ret = cast<ReturnInst>(colorExportFunc->back().getTerminator());
  BuilderBase builder(ret);

  SmallVector<Value *, 8> values(MaxColorTargets + 1, nullptr);
  unsigned exportSize = m_exports.size();
  unsigned lastIndex = 0;
  for (unsigned idx = 0; idx != exportSize; ++idx) {
    values[m_exports[idx].hwColorTarget] = colorExportFunc->getArg(idx);
    ++lastIndex;
  }

  PalMetadata palMetadata{m_pipelineState};

  Value *dynamicIsDualSource = colorExportFunc->getArg(lastIndex);
  fragColorExport.generateExportInstructions(m_exports, values, m_killEnabled, &palMetadata, builder,
                                             dynamicIsDualSource, m_key);

  // Handle on the dualSourceBlend case which may have two blocks with two returnInsts
  SmallVector<ReturnInst *, 8> retInsts;
  if (m_pipelineState->getOptions().enableColorExportShader) {
    for (auto &block : llvm::reverse(*colorExportFunc)) {
      if (auto ret = dyn_cast<ReturnInst>(block.getTerminator())) {
        retInsts.push_back(ret);
      }
    }
  }
  for (ReturnInst *inst : llvm::reverse(retInsts)) {
    builder.SetInsertPoint(inst);
    builder.CreateIntrinsic(Intrinsic::amdgcn_endpgm, {}, {});
    builder.CreateUnreachable();
    inst->eraseFromParent();
  }

  // Set pipeline hash.
  auto internalPipelineHash =
      palMetadata.getPipelineNode()[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  const auto &options = m_pipelineState->getOptions();
  internalPipelineHash[0] = options.hash[0];
  internalPipelineHash[1] = options.hash[1];

  palMetadata.updateDbShaderControl();
  palMetadata.record(colorExportFunc->getParent());

  return colorExportFunc->getParent();
}

// =====================================================================================================================
// Create module with function for the color export shader. On return, the function contains only the code to copy the
// wave dispatch SGPRs and VGPRs to the return value.
Function *ColorExportShader::createColorExportFunc() {
  // Create the module
  Module *module = new Module("colorExportShader", getContext());
  TargetMachine *targetMachine = m_lgcContext->getTargetMachine();
  module->setTargetTriple(targetMachine->getTargetTriple().getTriple());
  module->setDataLayout(targetMachine->createDataLayout());

  // Get the function type. Its inputs are the outputs from the unlinked pixel shader or similar.
  SmallVector<Type *, 16> entryTys;
  for (const auto &exp : m_exports)
    entryTys.push_back(exp.ty);
  entryTys.push_back(Type::getInt32Ty(getContext()));
  auto funcTy = FunctionType::get(Type::getVoidTy(getContext()), entryTys, false);

  // Create the function. Mark SGPR inputs as "inreg".
  Function *func = Function::Create(funcTy, GlobalValue::ExternalLinkage, getGlueShaderName(), module);
  if (m_pipelineState->getOptions().enableColorExportShader)
    func->setCallingConv(CallingConv::AMDGPU_Gfx);
  else
    func->setCallingConv(CallingConv::AMDGPU_PS);

  unsigned inRegIndex = m_exports.size();
  func->addParamAttr(inRegIndex, Attribute::InReg);

  func->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(func, ShaderStage::Fragment);

  BasicBlock *block = BasicBlock::Create(func->getContext(), "", func);
  BuilderBase builder(block);
  builder.CreateRetVoid();

  AttrBuilder attribBuilder(func->getContext());
  attribBuilder.addAttribute("InitialPSInputAddr", std::to_string(0xFFFFFFFF));
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Fragment);
  attribBuilder.addAttribute("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  func->addFnAttrs(attribBuilder);

  return func;
}

// =====================================================================================================================
// Update the color format entry in the PAL metadata.
//
// @param [in/out] outStream : The PAL metadata object in which to update the color format.
void ColorExportShader::updatePalMetadata(PalMetadata &palMetadata) {
}
