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
 * @file  Gfx6ConfigBuilder.h
 * @brief LLPC header file: contains declaration of class lgc::Gfx6::ConfigBuilder.
 ***********************************************************************************************************************
 */
#pragma once

#include "ConfigBuilderBase.h"
#include "Gfx6Chip.h"

namespace lgc {

namespace Gfx6 {

// =====================================================================================================================
// Represents the builder to generate register configurations for GFX6-generation chips.
class ConfigBuilder : public ConfigBuilderBase {
public:
  ConfigBuilder(llvm::Module *module, PipelineState *pipelineState) : ConfigBuilderBase(module, pipelineState) {}

  void buildPalMetadata();

  void buildPipelineVsFsRegConfig();
  void buildPipelineVsTsFsRegConfig();
  void buildPipelineVsGsFsRegConfig();
  void buildPipelineVsTsGsFsRegConfig();
  void buildPipelineCsRegConfig();

private:
  ConfigBuilder() = delete;
  ConfigBuilder(const ConfigBuilder &) = delete;
  ConfigBuilder &operator=(const ConfigBuilder &) = delete;

  template <typename T> void buildVsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildHsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildEsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildLsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildGsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildPsRegConfig(ShaderStage shaderStage, T *config);
  void buildCsRegConfig(ShaderStage shaderStage, CsRegConfig *config);

  template <typename T> void setupVgtTfParam(T *config);
};

} // namespace Gfx6

} // namespace lgc
