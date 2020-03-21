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
 * @file  llpcGfx9ConfigBuilder.h
 * @brief LLPC header file: contains declaration of class lgc::Gfx9::ConfigBuilder.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcConfigBuilderBase.h"
#include "llpcGfx9Chip.h"

namespace lgc
{

struct ElfDataEntry;
struct ResourceUsage;

namespace Gfx9
{

// =====================================================================================================================
// Represents the builder to generate register configurations for GFX6-generation chips.
class ConfigBuilder : public ConfigBuilderBase
{
public:
    ConfigBuilder(llvm::Module* pModule, PipelineState* pPipelineState)
        : ConfigBuilderBase(pModule, pPipelineState) {}

    void BuildPalMetadata();

    void BuildPipelineVsFsRegConfig();
    void BuildPipelineVsTsFsRegConfig();
    void BuildPipelineVsGsFsRegConfig();
    void BuildPipelineVsTsGsFsRegConfig();

    void BuildPipelineNggVsFsRegConfig();
    void BuildPipelineNggVsTsFsRegConfig();
    void BuildPipelineNggVsGsFsRegConfig();
    void BuildPipelineNggVsTsGsFsRegConfig();

    void BuildPipelineCsRegConfig();

private:
    ConfigBuilder() = delete;
    ConfigBuilder(const ConfigBuilder&) = delete;
    ConfigBuilder& operator=(const ConfigBuilder&) = delete;

    template <typename T>
    void BuildVsRegConfig(ShaderStage         shaderStage,
                            T*                  pConfig);

    template <typename T>
    void BuildLsHsRegConfig(ShaderStage         shaderStage1,
                            ShaderStage         shaderStage2,
                            T*                  pConfig);

    template <typename T>
    void BuildEsGsRegConfig(ShaderStage         shaderStage1,
                            ShaderStage         shaderStage2,
                            T*                  pConfig);

    template <typename T>
    void BuildPrimShaderRegConfig(ShaderStage         shaderStage1,
                                  ShaderStage         shaderStage2,
                                  T*                  pConfig);

    template <typename T>
    void BuildPsRegConfig(ShaderStage         shaderStage,
                          T*                  pConfig);

    void BuildCsRegConfig(ShaderStage shaderStage,
                            CsRegConfig* pConfig);

    void BuildUserDataConfig(ShaderStage shaderStage1,
                               ShaderStage shaderStage2,
                               uint32_t    startUserData);

    void SetupVgtTfParam(LsHsRegConfig* pConfig);

    bool GetShaderWgpMode(ShaderStage shaderStage) const;
};

} // Gfx9

} // lgc
