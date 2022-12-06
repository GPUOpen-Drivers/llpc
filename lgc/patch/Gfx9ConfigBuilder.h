/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Gfx9ConfigBuilder.h
 * @brief LLPC header file: contains declaration of class lgc::Gfx9::ConfigBuilder.
 ***********************************************************************************************************************
 */
#pragma once

#include "ConfigBuilderBase.h"
#include "Gfx9Chip.h"

namespace lgc {

namespace Gfx9 {

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

  void buildPipelineNggVsFsRegConfig();
  void buildPipelineNggVsTsFsRegConfig();
  void buildPipelineNggVsGsFsRegConfig();
  void buildPipelineNggVsTsGsFsRegConfig();

  void buildPipelineMeshFsConfig();
  void buildPipelineTaskMeshFsConfig();

  void buildPipelineCsRegConfig();

private:
  ConfigBuilder() = delete;
  ConfigBuilder(const ConfigBuilder &) = delete;
  ConfigBuilder &operator=(const ConfigBuilder &) = delete;

  template <typename T> void buildVsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildLsHsRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *config);
  template <typename T> void buildEsGsRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *config);
  template <typename T> void buildPrimShaderRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *config);
  template <typename T> void buildPsRegConfig(ShaderStage shaderStage, T *config);
  template <typename T> void buildMeshRegConfig(ShaderStage shaderStage, T *config);
  void buildCsRegConfig(ShaderStage shaderStage, CsRegConfig *config);

  void setupVgtTfParam(LsHsRegConfig *config);
  template <typename T> void setupPaSpecificRegisters(T *config);
};

} // namespace Gfx9

} // namespace lgc
