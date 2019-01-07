/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcGfx6ConfigBuilder.h
 * @brief LLPC header file: contains declaration of class Llpc::Gfx6::ConfigBuilder.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcConfigBuilderBase.h"
#include "llpcGfx6Chip.h"

namespace Llpc
{

class Context;
struct ElfDataEntry;
struct ResourceUsage;

namespace Gfx6
{

// =====================================================================================================================
// Represents the builder to generate register configurations for GFX6-generation chips.
class ConfigBuilder : public ConfigBuilderBase
{
public:
    ConfigBuilder(llvm::Module* pModule) : ConfigBuilderBase(pModule) {}

    void BuildPalMetadata();

    Result BuildPipelineVsFsRegConfig(Context*            pContext,
                                      uint8_t**           ppConfig,
                                      size_t*             pConfigSize);

    Result BuildPipelineVsTsFsRegConfig(Context*            pContext,
                                        uint8_t**           ppConfig,
                                        size_t*             pConfigSize);

    Result BuildPipelineVsGsFsRegConfig(Context*            pContext,
                                        uint8_t**           ppConfig,
                                        size_t*             pConfigSize);

    Result BuildPipelineVsTsGsFsRegConfig(Context*            pContext,
                                          uint8_t**           ppConfig,
                                          size_t*             pConfigSize);

    Result BuildPipelineCsRegConfig(Context*            pContext,
                                    uint8_t**              ppConfig,
                                    size_t*             pConfigSize);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(ConfigBuilder);
    LLPC_DISALLOW_COPY_AND_ASSIGN(ConfigBuilder);

    template <typename T>
    Result BuildVsRegConfig(Context*            pContext,
                            ShaderStage         shaderStage,
                            T*                  pConfig);

    template <typename T>
    Result BuildHsRegConfig(Context*            pContext,
                            ShaderStage         shaderStage,
                            T*                  pConfig);

    template <typename T>
    Result BuildEsRegConfig(Context*            pContext,
                            ShaderStage         shaderStage,
                            T*                  pConfig);

    template <typename T>
    Result BuildLsRegConfig(Context*            pContext,
                            ShaderStage         shaderStage,
                            T*                  pConfig);

    template <typename T>
    Result BuildGsRegConfig(Context*            pContext,
                            ShaderStage         shaderStage,
                            T*                  pConfig);

    template <typename T>
    Result BuildPsRegConfig(Context*            pContext,
                            ShaderStage         shaderStage,
                            T*                  pConfig);

    Result BuildCsRegConfig(Context*             pContext,
                            ShaderStage          shaderStage,
                            PipelineCsRegConfig* pConfig);

    template <typename T>
    Result BuildUserDataConfig(Context*    pContext,
                               ShaderStage shaderStage,
                               uint32_t    startUserData,
                               T*          pConfig);

    template <typename T>
    void SetupVgtTfParam(Context* pContext, T* pConfig);

    void BuildApiHwShaderMapping(uint32_t           vsHwShader,
                                 uint32_t           tcsHwShader,
                                 uint32_t           tesHwShader,
                                 uint32_t           gsHwShader,
                                 uint32_t           fsHwShader,
                                 uint32_t           csHwShader,
                                 PipelineRegConfig* pConfig);

    static uint32_t SetupFloatingPointMode(Context* pContext, ShaderStage shaderStage);

};

} // Gfx6

} // Llpc
