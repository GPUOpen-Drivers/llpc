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
 * @file  llpcConfigBuilderBase.cpp
 * @brief LLPC source file: contains implementation of class Llpc::ConfigBuilderBase.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-config-builder-base"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include "llpcConfigBuilderBase.h"
#include "llpcAbiMetadata.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
ConfigBuilderBase::ConfigBuilderBase(
    llvm::Module* pModule)  // [in/out] LLVM module
    :
    m_pModule(pModule)
{
    m_pContext = static_cast<Context*>(&pModule->getContext());

    m_hasVs  = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageVertex)) != 0);
    m_hasTcs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessControl)) != 0);
    m_hasTes = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessEval)) != 0);
    m_hasGs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageGeometry)) != 0);

    m_gfxIp = m_pContext->GetGfxIpVersion();
}

// =====================================================================================================================
ConfigBuilderBase::~ConfigBuilderBase()
{
    delete[] m_pConfig;
}

// =====================================================================================================================
// Write the config into PAL metadata in the LLVM IR module
void ConfigBuilderBase::WritePalMetadata()
{
    std::vector<Metadata*> abiMeta;
    size_t sizeInDword = m_configSize / sizeof(uint32_t);
    // Configs are composed of DWORD key/value pair, the size should be even
    LLPC_ASSERT((sizeInDword % 2) == 0);
    for (size_t i = 0; i < sizeInDword; i += 2)
    {
        uint32_t key   = (reinterpret_cast<uint32_t*>(m_pConfig))[i];
        uint32_t value = (reinterpret_cast<uint32_t*>(m_pConfig))[i + 1];
        // Don't export invalid metadata key and value
        if (key == InvalidMetadataKey)
        {
            LLPC_ASSERT(value == InvalidMetadataValue);
        }
        else
        {
            abiMeta.push_back(ConstantAsMetadata::get(ConstantInt::get(m_pContext->Int32Ty(), key, false)));
            abiMeta.push_back(ConstantAsMetadata::get(ConstantInt::get(m_pContext->Int32Ty(), value, false)));
        }
    }

    auto pAbiMetaTuple = MDTuple::get(*m_pContext, abiMeta);
    auto pAbiMetaNode = m_pModule->getOrInsertNamedMetadata("amdgpu.pal.metadata");
    pAbiMetaNode->addOperand(pAbiMetaTuple);
}

