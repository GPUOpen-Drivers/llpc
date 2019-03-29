/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
    m_pModule(pModule),
    m_userDataLimit(0),
    m_spillThreshold(UINT32_MAX)
{
    m_pContext = static_cast<Context*>(&pModule->getContext());

    m_hasVs  = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageVertex)) != 0);
    m_hasTcs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessControl)) != 0);
    m_hasTes = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessEval)) != 0);
    m_hasGs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageGeometry)) != 0);

    m_gfxIp = m_pContext->GetGfxIpVersion();

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
    // Ensure we have a STREAM_OUT_TABLE_ENTRY, even if it is 0.
    SetStreamOutTableEntry(0);
#endif
}

// =====================================================================================================================
ConfigBuilderBase::~ConfigBuilderBase()
{
    delete[] m_pConfig;
}

// =====================================================================================================================
// Builds metadata API_HW_SHADER_MAPPING_HI/LO.
void ConfigBuilderBase::BuildApiHwShaderMapping(
    uint32_t           vsHwShader,    // Hardware shader mapping for vertex shader
    uint32_t           tcsHwShader,   // Hardware shader mapping for tessellation control shader
    uint32_t           tesHwShader,   // Hardware shader mapping for tessellation evaluation shader
    uint32_t           gsHwShader,    // Hardware shader mapping for geometry shader
    uint32_t           fsHwShader,    // Hardware shader mapping for fragment shader
    uint32_t           csHwShader)    // Hardware shader mapping for compute shader
{
    {
        Util::Abi::ApiHwShaderMapping apiHwShaderMapping = {};

        apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Cs)] = csHwShader;
        apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Vs)] = vsHwShader;
        apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Hs)] = tcsHwShader;
        apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Ds)] = tesHwShader;
        apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Gs)] = gsHwShader;
        apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Ps)] = fsHwShader;

        SetPseudoRegister(Util::Abi::PipelineMetadataBase |
                                uint32_t(Util::Abi::PipelineMetadataType::ApiHwShaderMappingLo),
                              apiHwShaderMapping.u32Lo);
        SetPseudoRegister(Util::Abi::PipelineMetadataBase |
                                uint32_t(Util::Abi::PipelineMetadataType::ApiHwShaderMappingHi),
                              apiHwShaderMapping.u32Hi);
    }
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
// =====================================================================================================================
// Set INDIRECT_TABLE_ENTRY
void ConfigBuilderBase::SetIndirectTableEntry(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmINDIRECT_TABLE_ENTRY, value);
    }
}

// =====================================================================================================================
// Set STREAM_OUT_TABLE_ENTRY
void ConfigBuilderBase::SetStreamOutTableEntry(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmSTREAM_OUT_TABLE_ENTRY, value);
    }
}
#endif

// =====================================================================================================================
// Set an API shader's hash in metadata
void ConfigBuilderBase::SetShaderHash(
    ShaderStage apiStage, // API shader stage
    uint64_t    hash64)   // Its hash
{
    {
        static const uint32_t hashKeys[] =
        {
            // In ShaderStage order
            mmAPI_VS_HASH_DWORD0,
            mmAPI_HS_HASH_DWORD0,
            mmAPI_DS_HASH_DWORD0,
            mmAPI_GS_HASH_DWORD0,
            mmAPI_PS_HASH_DWORD0,
            mmAPI_CS_HASH_DWORD0
        };
        SetPseudoRegister(hashKeys[apiStage], uint32_t(hash64));
        SetPseudoRegister(hashKeys[apiStage] + 1, uint32_t(hash64 >> 32));
    }
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_SGPRS for given hardware shader stage
void ConfigBuilderBase::SetNumAvailSgprs(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    uint32_t value)                   // Number of available SGPRs
{
    {
        static const uint32_t availSgprsKeys[] =
        {
            // In Util::Abi::HardwareStage order
            mmLS_NUM_AVAIL_SGPRS,
            mmHS_NUM_AVAIL_SGPRS,
            mmES_NUM_AVAIL_SGPRS,
            mmGS_NUM_AVAIL_SGPRS,
            mmVS_NUM_AVAIL_SGPRS,
            mmPS_NUM_AVAIL_SGPRS,
            mmCS_NUM_AVAIL_SGPRS
        };
        SetPseudoRegister(availSgprsKeys[uint32_t(hwStage)], value);
    }
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_VGPRS for given hardware shader stage
void ConfigBuilderBase::SetNumAvailVgprs(
    Util::Abi::HardwareStage hwStage, // HW shader stage
    uint32_t value)                   // Number of available VGPRs
{
    {
        static const uint32_t availVgprsKeys[] =
        {
            // In Util::Abi::HardwareStage order
            mmLS_NUM_AVAIL_VGPRS,
            mmHS_NUM_AVAIL_VGPRS,
            mmES_NUM_AVAIL_VGPRS,
            mmGS_NUM_AVAIL_VGPRS,
            mmVS_NUM_AVAIL_VGPRS,
            mmPS_NUM_AVAIL_VGPRS,
            mmCS_NUM_AVAIL_VGPRS
        };
        SetPseudoRegister(availVgprsKeys[uint32_t(hwStage)], value);
    }
}

// =====================================================================================================================
// Set USES_VIEWPORT_ARRAY_INDEX
void ConfigBuilderBase::SetUsesViewportArrayIndex(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmUSES_VIEWPORT_ARRAY_INDEX, value);
    }
}

// =====================================================================================================================
// Set PS_USES_UAVS
void ConfigBuilderBase::SetPsUsesUavs(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmPS_USES_UAVS, value);
    }
}

// =====================================================================================================================
// Set PS_WRITES_UAVS
void ConfigBuilderBase::SetPsWritesUavs(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmPS_WRITES_UAVS, value);
    }
}

// =====================================================================================================================
// Set PS_WRITES_DEPTH
void ConfigBuilderBase::SetPsWritesDepth(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmPS_WRITES_DEPTH, value);
    }
}

// =====================================================================================================================
// Set ES_GS_LDS_BYTE_SIZE
void ConfigBuilderBase::SetEsGsLdsByteSize(
    uint32_t value)   // Value to set
{
    {
        SetPseudoRegister(mmES_GS_LDS_BYTE_SIZE, value);
    }
}

// =====================================================================================================================
// Set USER_DATA_LIMIT (called once for the whole pipeline)
void ConfigBuilderBase::SetUserDataLimit()
{
    {
        SetPseudoRegister(mmUSER_DATA_LIMIT, m_userDataLimit);
    }
}

// =====================================================================================================================
// Set SPILL_THRESHOLD (called once for the whole pipeline)
void ConfigBuilderBase::SetSpillThreshold()
{
    {
        SetPseudoRegister(mmSPILL_THRESHOLD, m_spillThreshold);
    }
}

// =====================================================================================================================
// Set PIPELINE_HASH (called once for the whole pipeline)
void ConfigBuilderBase::SetPipelineHash()
{
    auto hash64 = m_pContext->GetPiplineHashCode();
    {
        SetPseudoRegister(mmPIPELINE_HASH_LO, uint32_t(hash64));
        SetPseudoRegister(mmPIPELINE_HASH_HI, uint32_t(hash64 >> 32));
    }
}

// =====================================================================================================================
// Set pseudo-register in PAL metadata
void ConfigBuilderBase::SetPseudoRegister(
    uint32_t reg,     // Register number
    uint32_t value)   // Value to set
{
    m_pseudoRegisters.push_back(reg);
    m_pseudoRegisters.push_back(value);
}

// =====================================================================================================================
// Write the config into PAL metadata in the LLVM IR module
void ConfigBuilderBase::WritePalMetadata()
{
    // Set whole-pipeline values.
    SetUserDataLimit();
    SetSpillThreshold();
    SetPipelineHash();

    {
        // Write the metadata into an IR metadata node.
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

        for (uint32_t value : m_pseudoRegisters)
        {
            abiMeta.push_back(ConstantAsMetadata::get(ConstantInt::get(m_pContext->Int32Ty(), value, false)));
        }

        auto pAbiMetaTuple = MDTuple::get(*m_pContext, abiMeta);
        auto pAbiMetaNode = m_pModule->getOrInsertNamedMetadata("amdgpu.pal.metadata");
        pAbiMetaNode->addOperand(pAbiMetaTuple);
    }
}

