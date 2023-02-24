/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  RegisterMetadataBuilder.h
 * @brief LLPC header file: contains declaration of class lgc::RegisterMetadataBuilder.
 ***********************************************************************************************************************
 */
#pragma once

#include "ConfigBuilderBase.h"
#include "llvm/ADT/DenseSet.h"

namespace lgc {

namespace Gfx9 {
// =====================================================================================================================
// Represents the builder to generate register configurations for GFX11 plus chips.
class RegisterMetadataBuilder : public ConfigBuilderBase {
public:
  RegisterMetadataBuilder(llvm::Module *module, PipelineState *PipelineState)
      : ConfigBuilderBase(module, PipelineState) {}

  void buildPalMetadata();

private:
  void buildLsHsRegisters();
  void buildEsGsRegisters();
  void buildPrimShaderRegisters();
  void buildHwVsRegisters();
  void buildPsRegisters();
  void buildCsRegisters(ShaderStage shaderStage);

  void buildShaderExecutionRegisters(Util::Abi::HardwareStage hwStageId, ShaderStage apiStage1, ShaderStage apiStage2);
  void buildPaSpecificRegisters();
  void setVgtShaderStagesEn(unsigned hwStageMask);
  void setIaMultVgtParam();
  void setVgtTfParam();

  bool m_isNggMode = false;
};

} // namespace Gfx9
} // namespace lgc
