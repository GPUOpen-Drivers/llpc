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

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 477
    // Only generate MsgPack PAL metadata for PAL client 477 onwards. PAL changed the .note record type
    // from 13 to 32 at that point, and not using MsgPack metadata before that avoids some compatibility
    // problems.
    m_document = std::make_unique<msgpack::Document>();
#else
    LLPC_NEVER_CALLED();
#endif

    m_pipelineNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines]
                                              .getArray(true)[0].getMap(true);

    SetApiName("Vulkan"); // TODO: Client API name should be from ICD.
}

// =====================================================================================================================
ConfigBuilderBase::~ConfigBuilderBase()
{
}

// =====================================================================================================================
/// Adds the .shaders.$(apiStage).hardware_mapping node to the PAL metadata.
///
/// @param [in] apiStage The API shader stage
/// @param [in] hwStages The HW stage(s) that the API shader is mapped to, as a combination of
///                      @ref Util::Abi::HardwareStageFlagBits.
void ConfigBuilderBase::AddApiHwShaderMapping(
    ShaderStage apiStage,
    uint32_t hwStages)
{
    auto hwMappingNode = GetApiShaderNode(apiStage)[Util::Abi::ShaderMetadataKey::HardwareMapping]
                            .getArray(true);
    for (uint32_t hwStage = 0; hwStage < uint32_t(Util::Abi::HardwareStage::Count); ++hwStage)
    {
        if (hwStages & (1 << hwStage))
        {
            hwMappingNode.push_back(m_document->getNode(HwStageNames[hwStage]));
        }
    }
}

// =====================================================================================================================
// Get the MsgPack map node for the specified API shader in the ".shaders" map
msgpack::MapDocNode ConfigBuilderBase::GetApiShaderNode(
    uint32_t apiStage)  // API shader stage
{
    if (m_apiShaderNodes[apiStage].isEmpty())
    {
        m_apiShaderNodes[apiStage] = m_pipelineNode[Util::Abi::PipelineMetadataKey::Shaders]
                                                   .getMap(true)[ApiStageNames[apiStage]].getMap(true);
    }
    return m_apiShaderNodes[apiStage];
}

// =====================================================================================================================
// Get the MsgPack map node for the specified hardware shader in the ".hardware_stages" map
msgpack::MapDocNode ConfigBuilderBase::GetHwShaderNode(
    Util::Abi::HardwareStage hwStage)   // Hardware shader stage
{
    if (m_hwShaderNodes[uint32_t(hwStage)].isEmpty())
    {
        m_hwShaderNodes[uint32_t(hwStage)] = m_pipelineNode[Util::Abi::PipelineMetadataKey::HardwareStages]
                                                .getMap(true)[HwStageNames[uint32_t(hwStage)]].getMap(true);
    }
    return m_hwShaderNodes[uint32_t(hwStage)];
}

// =====================================================================================================================
// Set an API shader's hash in metadata
void ConfigBuilderBase::SetShaderHash(
    ShaderStage   apiStage, // API shader stage
    ShaderHash    hash)     // Its hash
{
    auto hashNode = GetApiShaderNode(uint32_t(apiStage))[Util::Abi::ShaderMetadataKey::ApiShaderHash].getArray(true);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
    // 128-bit hash
    hashNode[0] = hashNode.getDocument()->getNode(hash.lower);
    hashNode[1] = hashNode.getDocument()->getNode(hash.upper);
#else
    // 64-bit hash
    hashNode[0] = hashNode.getDocument()->getNode(hash);
    hashNode[1] = hashNode.getDocument()->getNode(0U);
#endif
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_SGPRS for given hardware shader stage
void ConfigBuilderBase::SetNumAvailSgprs(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    uint32_t value)                   // Number of available SGPRs
{
    auto hwShaderNode = GetHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::SgprLimit] = hwShaderNode.getDocument()->getNode(value);
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_VGPRS for given hardware shader stage
void ConfigBuilderBase::SetNumAvailVgprs(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    uint32_t value)                   // Number of available VGPRs
{
    auto hwShaderNode = GetHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::VgprLimit] = hwShaderNode.getDocument()->getNode(value);
}

// =====================================================================================================================
// Set USES_VIEWPORT_ARRAY_INDEX
void ConfigBuilderBase::SetUsesViewportArrayIndex(
    bool value)   // Value to set
{
    if (value == false)
    {
        return; // Optional
    }

    m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex] = m_document->getNode(value);
}

// =====================================================================================================================
// Set PS_USES_UAVS
void ConfigBuilderBase::SetPsUsesUavs(
    bool value)   // Value to set
{
    if (value == false)
    {
        return; // Optional
    }

    GetHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::UsesUavs] =
        m_document->getNode(value);
}

// =====================================================================================================================
// Set PS_WRITES_UAVS
void ConfigBuilderBase::SetPsWritesUavs(
    bool value)   // Value to set
{
    if (value == false)
    {
        return; // Optional
    }

    GetHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::WritesUavs] =
        m_document->getNode(value);

}

// =====================================================================================================================
// Set PS_WRITES_DEPTH
void ConfigBuilderBase::SetPsWritesDepth(
    bool value)   // Value to set
{
    if (value == false)
    {
        return; // Optional
    }

    GetHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::WritesDepth] =
        m_document->getNode(value);
}

// =====================================================================================================================
// Set ES_GS_LDS_BYTE_SIZE
void ConfigBuilderBase::SetEsGsLdsByteSize(
    uint32_t value)   // Value to set
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::EsGsLdsSize] = m_document->getNode(value);
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Set CALC_WAVE_BREAK_SIZE_AT_DRAW_TIME
void ConfigBuilderBase::SetCalcWaveBreakSizeAtDrawTime(
    bool value)   // Value to set
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::CalcWaveBreakSizeAtDrawTime] = m_document->getNode(value);
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
// =====================================================================================================================
// Set hardware stage wavefront
void ConfigBuilderBase::SetWaveFrontSize(
    Util::Abi::HardwareStage hwStage,   // Hardware shader stage
    uint32_t                 value)     // Value to set
{
    auto hwShaderNode = GetHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::WavefrontSize] = m_document->getNode(value);
}

#endif
#endif

// =====================================================================================================================
// Set API name
void ConfigBuilderBase::SetApiName(
    const char* pValue) // [in] Value to set
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::Api] = m_document->getNode(pValue);
}

// =====================================================================================================================
// Set pipeline type
void ConfigBuilderBase::SetPipelineType(
    Util::Abi::PipelineType value) // Value to set
{
    const char* pValue = "";
    switch (value)
    {
    case Util::Abi::VsPs:
        pValue = "VsPs";
        break;
    case Util::Abi::Gs:
        pValue = "Gs";
        break;
    case Util::Abi::Cs:
        pValue = "Cs";
        break;
    case Util::Abi::Ngg:
        pValue = "Ngg";
        break;
    case Util::Abi::Tess:
        pValue = "Tess";
        break;
    case Util::Abi::GsTess:
        pValue = "GsTess";
        break;
    case Util::Abi::NggTess:
        pValue = "NggTess";
        break;
    default:
        break;
    }
    m_pipelineNode[Util::Abi::PipelineMetadataKey::Type] = m_document->getNode(pValue);
}

// =====================================================================================================================
// Set LDS byte size for given hardware shader stage
void ConfigBuilderBase::SetLdsSizeByteSize(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    uint32_t                 value)   // Value to set
{
    if (value == 0)
    {
        return; // Optional
    }

    auto hwShaderNode = GetHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = hwShaderNode.getDocument()->getNode(value);
}

// =====================================================================================================================
// Set ES-GS LDS byte size
void ConfigBuilderBase::SetEsGsLdsSize(
    uint32_t value) // Value to set
{
    if (value == 0)
    {
        return; // Optional
    }

    m_pipelineNode[Util::Abi::PipelineMetadataKey::EsGsLdsSize] = m_document->getNode(value);
}

// =====================================================================================================================
// Set USER_DATA_LIMIT (called once for the whole pipeline)
void ConfigBuilderBase::SetUserDataLimit()
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::UserDataLimit] = m_document->getNode(m_userDataLimit);
}

// =====================================================================================================================
// Set SPILL_THRESHOLD (called once for the whole pipeline)
void ConfigBuilderBase::SetSpillThreshold()
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::SpillThreshold] = m_document->getNode(m_spillThreshold);
}

// =====================================================================================================================
// Set PIPELINE_HASH (called once for the whole pipeline)
void ConfigBuilderBase::SetPipelineHash()
{
    auto hash64 = m_pContext->GetPiplineHashCode();

    auto pipelineHashNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
    pipelineHashNode[0] = m_document->getNode(hash64);
    pipelineHashNode[1] = m_document->getNode(0U);
}

// =====================================================================================================================
/// Append a single entry to the PAL register metadata.
///
/// @param [in] key The metadata key (usually a register address).
/// @param [in] value The metadata value.
void ConfigBuilderBase::AppendConfig(uint32_t key, uint32_t value)
{
    LLPC_ASSERT(key != InvalidMetadataKey);

    Util::Abi::PalMetadataNoteEntry entry;
    entry.key = key;
    entry.value = value;
    m_config.push_back(entry);
}

// =====================================================================================================================
/// Append an array of entries to the PAL register metadata. Invalid keys are filtered out.
///
/// @param [in] config The array of register metadata entries.
void ConfigBuilderBase::AppendConfig(llvm::ArrayRef<Util::Abi::PalMetadataNoteEntry> config)
{
    uint32_t count = 0;

    for (const auto &entry : config)
    {
        if (entry.key != InvalidMetadataKey)
            ++count;
    }

    uint32_t idx = m_config.size();
    m_config.resize(idx + count);

    for (const auto &entry : config)
    {
        if (entry.key != InvalidMetadataKey)
        {
            m_config[idx++] = entry;
        }
    }
}

// =====================================================================================================================
// Write the config into PAL metadata in the LLVM IR module
void ConfigBuilderBase::WritePalMetadata()
{
    // Set whole-pipeline values.
    SetUserDataLimit();
    SetSpillThreshold();
    SetPipelineHash();

    // Generating MsgPack metadata.
    // Set the pipeline hashes.
    auto pipelineHashNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
    pipelineHashNode[0] = m_document->getNode(m_pContext->GetPiplineHashCode());
    pipelineHashNode[1] = m_document->getNode(m_pContext->GetCacheHashCode());

    // Add the register values to the MsgPack document.
    msgpack::MapDocNode registers = m_pipelineNode[".registers"].getMap(true);
    for (const auto& entry : m_config)
    {
        LLPC_ASSERT(entry.key != InvalidMetadataKey);
        auto key   = m_document->getNode(entry.key);
        auto value = m_document->getNode(entry.value);
        registers[key] = value;

    }

    // Add the metadata version number.
    auto versionNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Version]
                            .getArray(true);
    versionNode[0] = m_document->getNode(Util::Abi::PipelineMetadataMajorVersion);
    versionNode[1] = m_document->getNode(Util::Abi::PipelineMetadataMinorVersion);

    // Write the MsgPack document into an IR metadata node.
    std::string blob;
    m_document->writeToBlob(blob);
    auto pAbiMetaString = MDString::get(m_pModule->getContext(), blob);
    auto pAbiMetaNode = MDNode::get(m_pModule->getContext(), pAbiMetaString);
    auto pNamedMeta = m_pModule->getOrInsertNamedMetadata("amdgpu.pal.metadata.msgpack");
    pNamedMeta->addOperand(pAbiMetaNode);
}

