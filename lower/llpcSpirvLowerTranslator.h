/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerTranslator.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvLowerTranslator
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"

namespace Llpc
{

// =====================================================================================================================
// Pass to translate the SPIR-V modules and generate an IR module for the whole pipeline
class SpirvLowerTranslator : public SpirvLower
{
public:
    static char ID;
    SpirvLowerTranslator() : SpirvLower(ID)
    {
        initializeSpirvLowerTranslatorPass(*llvm::PassRegistry::getPassRegistry());
    }

    SpirvLowerTranslator(
        ShaderStage                 stage,        // Shader stage
        const PipelineShaderInfo*   pShaderInfo)  // [in] Shader info for this shader
        : SpirvLower(ID), m_shaderStage(stage), m_pShaderInfo(pShaderInfo)
    {
        initializeSpirvLowerTranslatorPass(*llvm::PassRegistry::getPassRegistry());
    }

    bool runOnModule(llvm::Module& module) override;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(SpirvLowerTranslator);

    void TranslateSpirvToLlvm(const PipelineShaderInfo* pShaderInfo,
                              llvm::Module*             pModule);

    // -----------------------------------------------------------------------------------------------------------------

    ShaderStage               m_shaderStage;    // Shader stage
    const PipelineShaderInfo* m_pShaderInfo;    // Input shader info
};

} // Llpc
