/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
    if (pipelineState->useRegisterFieldFormat()) {
      auto dbShaderControl = metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::GraphicsRegisters]
                                 .getMap(true)[Util::Abi::GraphicsRegisterMetadataKey::DbShaderControl]
                                 .getMap(true);
      m_killEnabled = dbShaderControl[Util::Abi::DbShaderControlMetadataKey::KillEnable].getBool();
    } else {
      DB_SHADER_CONTROL shaderControl = {};
      shaderControl.u32All = metadata->getRegister(mmDB_SHADER_CONTROL);
      m_killEnabled = shaderControl.bits.KILL_ENABLE;
    }
  }
}

// =====================================================================================================================
// Get the string for this color export shader. This is some encoding or hash of the inputs to the
// createColorExportShader function that the front-end client can use as a cache key to avoid compiling the same glue
// shader more than once.
StringRef ColorExportShader::getString() {
  if (m_shaderString.empty()) {
    constexpr uint32_t estimatedTypeSize = 10;
    uint32_t sizeEstimate = (sizeof(ColorExportInfo) + estimatedTypeSize) * m_exports.size();
    sizeEstimate += sizeof(m_killEnabled);
    sizeEstimate += sizeof(ColorExportState) + MaxColorTargets * sizeof(ColorExportFormat);
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
    m_shaderString += StringRef(reinterpret_cast<const char *>(&m_killEnabled), sizeof(m_killEnabled));

    const ColorExportState *colorExportState = &m_pipelineState->getColorExportState();
    m_shaderString += StringRef(reinterpret_cast<const char *>(colorExportState), sizeof(*colorExportState));
    for (unsigned location = 0; location < MaxColorTargets; ++location) {
      const ColorExportFormat *colorExportFormat = &m_pipelineState->getColorExportFormat(location);
      m_shaderString += StringRef(reinterpret_cast<const char *>(colorExportFormat), sizeof(*colorExportFormat));
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
  FragColorExport fragColorExport(&getContext(), m_pipelineState);
  auto ret = cast<ReturnInst>(colorExportFunc->back().getTerminator());
  BuilderBase builder(ret);

  SmallVector<Value *, 8> values(MaxColorTargets + 1, nullptr);
  for (unsigned idx = 0; idx != m_exports.size(); ++idx) {
    values[m_exports[idx].hwColorTarget] = colorExportFunc->getArg(idx);
  }

  PalMetadata palMetadata{m_pipelineState, m_pipelineState->useRegisterFieldFormat()};

  bool dummyExport = m_lgcContext->getTargetInfo().getGfxIpVersion().major < 10 || m_killEnabled;
  fragColorExport.generateExportInstructions(m_exports, values, dummyExport, &palMetadata, builder);

  if (m_pipelineState->getOptions().enableColorExportShader) {
    builder.CreateIntrinsic(Intrinsic::amdgcn_endpgm, {}, {});
    builder.CreateUnreachable();
    ret->eraseFromParent();
  }

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
  auto funcTy = FunctionType::get(Type::getVoidTy(getContext()), entryTys, false);

  // Create the function. Mark SGPR inputs as "inreg".
  Function *func = Function::Create(funcTy, GlobalValue::ExternalLinkage, getGlueShaderName(), module);
  if (m_pipelineState->getOptions().enableColorExportShader)
    func->setCallingConv(CallingConv::AMDGPU_Gfx);
  else
    func->setCallingConv(CallingConv::AMDGPU_PS);

  func->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(func, ShaderStageFragment);

  BasicBlock *block = BasicBlock::Create(func->getContext(), "", func);
  BuilderBase builder(block);
  builder.CreateRetVoid();

  AttrBuilder attribBuilder(func->getContext());
  attribBuilder.addAttribute("InitialPSInputAddr", std::to_string(0xFFFFFFFF));
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 10) {
    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageFragment);
    attribBuilder.addAttribute("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  }
  func->addFnAttrs(attribBuilder);

  return func;
}

// =====================================================================================================================
// Update the color format entry in the PAL metadata.
//
// @param [in/out] outStream : The PAL metadata object in which to update the color format.
void ColorExportShader::updatePalMetadata(PalMetadata &palMetadata) {
}
