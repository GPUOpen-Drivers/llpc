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

    SpirvLowerTranslator(llvm::ArrayRef<const PipelineShaderInfo*> shaderInfo) :
          SpirvLower(ID), m_shaderInfo(shaderInfo)
    {
        initializeSpirvLowerTranslatorPass(*llvm::PassRegistry::getPassRegistry());
    }

    bool runOnModule(llvm::Module& module) override;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(SpirvLowerTranslator);

    llvm::Module* TranslateSpirvToLlvm(const BinaryData*           pSpirvBin,
                                       ShaderStage                 shaderStage,
                                       const char*                 pEntryTarget,
                                       const VkSpecializationInfo* pSpecializationInfo
                                      ) const;
    Result OptimizeSpirv(const BinaryData* pSpirvBinIn, BinaryData* pSpirvBinOut) const;
    void CleanOptimizedSpirv(BinaryData* pSpirvBin) const;

    // -----------------------------------------------------------------------------------------------------------------

    llvm::ArrayRef<const PipelineShaderInfo*> m_shaderInfo;   // Input shader info array
};

} // Llpc
