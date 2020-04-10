/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC source file: contains implementation of class lgc::ConfigBuilderBase.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#include "llpcConfigBuilderBase.h"
#include "llpcAbiMetadata.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-config-builder-base"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
ConfigBuilderBase::ConfigBuilderBase(
    llvm::Module*   module,        // [in/out] LLVM module
    PipelineState*  pipelineState) // [in] Pipeline state
    :
    m_module(module),
    m_pipelineState(pipelineState),
    m_userDataLimit(0),
    m_spillThreshold(UINT32_MAX)
{
    m_context = &module->getContext();

    m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
    m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
    m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

    m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

    // Only generate MsgPack PAL metadata for PAL client 477 onwards. PAL changed the .note record type
    // from 13 to 32 at that point, and not using MsgPack metadata before that avoids some compatibility
    // problems.
    if (m_pipelineState->getPalAbiVersion() < 477)
        report_fatal_error("PAL ABI version less than 477 not supported");
    m_document = std::make_unique<msgpack::Document>();

    m_pipelineNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines]
                                              .getArray(true)[0].getMap(true);

    setApiName("Vulkan"); // TODO: Client API name should be from ICD.
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
void ConfigBuilderBase::addApiHwShaderMapping(
    ShaderStage apiStage,
    unsigned hwStages)
{
    auto hwMappingNode = getApiShaderNode(apiStage)[Util::Abi::ShaderMetadataKey::HardwareMapping]
                            .getArray(true);
    for (unsigned hwStage = 0; hwStage < unsigned(Util::Abi::HardwareStage::Count); ++hwStage)
    {
        if (hwStages & (1 << hwStage))
            hwMappingNode.push_back(m_document->getNode(HwStageNames[hwStage]));
    }
}

// =====================================================================================================================
// Get the MsgPack map node for the specified API shader in the ".shaders" map
msgpack::MapDocNode ConfigBuilderBase::getApiShaderNode(
    unsigned apiStage)  // API shader stage
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
msgpack::MapDocNode ConfigBuilderBase::getHwShaderNode(
    Util::Abi::HardwareStage hwStage)   // Hardware shader stage
{
    if (m_hwShaderNodes[unsigned(hwStage)].isEmpty())
    {
        m_hwShaderNodes[unsigned(hwStage)] = m_pipelineNode[Util::Abi::PipelineMetadataKey::HardwareStages]
                                                .getMap(true)[HwStageNames[unsigned(hwStage)]].getMap(true);
    }
    return m_hwShaderNodes[unsigned(hwStage)];
}

// =====================================================================================================================
// Set an API shader's hash in metadata. Returns a 32-bit value derived from the hash that is used as
// a shader checksum for performance profiling where applicable.
unsigned ConfigBuilderBase::setShaderHash(
    ShaderStage   apiStage) // API shader stage
{
    const ShaderOptions& shaderOptions = m_pipelineState->getShaderOptions(apiStage);
    auto hashNode = getApiShaderNode(unsigned(apiStage))[Util::Abi::ShaderMetadataKey::ApiShaderHash].getArray(true);
    hashNode[0] = hashNode.getDocument()->getNode(shaderOptions.hash[0]);
    hashNode[1] = hashNode.getDocument()->getNode(shaderOptions.hash[1]);
    return shaderOptions.hash[0] >> 32 ^ shaderOptions.hash[0] ^ shaderOptions.hash[1] >> 32 ^ shaderOptions.hash[1];
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_SGPRS for given hardware shader stage
void ConfigBuilderBase::setNumAvailSgprs(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    unsigned value)                   // Number of available SGPRs
{
    auto hwShaderNode = getHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::SgprLimit] = hwShaderNode.getDocument()->getNode(value);
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_VGPRS for given hardware shader stage
void ConfigBuilderBase::setNumAvailVgprs(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    unsigned value)                   // Number of available VGPRs
{
    auto hwShaderNode = getHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::VgprLimit] = hwShaderNode.getDocument()->getNode(value);
}

// =====================================================================================================================
// Set USES_VIEWPORT_ARRAY_INDEX
void ConfigBuilderBase::setUsesViewportArrayIndex(
    bool value)   // Value to set
{
    if (!value)
        return; // Optional

    m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex] = m_document->getNode(value);
}

// =====================================================================================================================
// Set PS_USES_UAVS
void ConfigBuilderBase::setPsUsesUavs(
    bool value)   // Value to set
{
    if (!value)
        return; // Optional

    getHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::UsesUavs] =
        m_document->getNode(value);
}

// =====================================================================================================================
// Set PS_WRITES_UAVS
void ConfigBuilderBase::setPsWritesUavs(
    bool value)   // Value to set
{
    if (!value)
        return; // Optional

    getHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::WritesUavs] =
        m_document->getNode(value);

}

// =====================================================================================================================
// Set PS_WRITES_DEPTH
void ConfigBuilderBase::setPsWritesDepth(
    bool value)   // Value to set
{
    if (!value)
        return; // Optional

    getHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::WritesDepth] =
        m_document->getNode(value);
}

// =====================================================================================================================
// Set ES_GS_LDS_BYTE_SIZE
void ConfigBuilderBase::setEsGsLdsByteSize(
    unsigned value)   // Value to set
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::EsGsLdsSize] = m_document->getNode(value);
}

// =====================================================================================================================
// Set CALC_WAVE_BREAK_SIZE_AT_DRAW_TIME
void ConfigBuilderBase::setCalcWaveBreakSizeAtDrawTime(
    bool value)   // Value to set
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::CalcWaveBreakSizeAtDrawTime] = m_document->getNode(value);
}

// =====================================================================================================================
// Set hardware stage wavefront
void ConfigBuilderBase::setWaveFrontSize(
    Util::Abi::HardwareStage hwStage,   // Hardware shader stage
    unsigned                 value)     // Value to set
{
    if (m_pipelineState->getPalAbiVersion() >= 495)
    {
        auto hwShaderNode = getHwShaderNode(hwStage);
        hwShaderNode[Util::Abi::HardwareStageMetadataKey::WavefrontSize] = m_document->getNode(value);
    }
}

// =====================================================================================================================
// Set API name
void ConfigBuilderBase::setApiName(
    const char* value) // [in] Value to set
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::Api] = m_document->getNode(value);
}

// =====================================================================================================================
// Set pipeline type
void ConfigBuilderBase::setPipelineType(
    Util::Abi::PipelineType value) // Value to set
{
    const char* typeStr = "";
    switch (value)
    {
    case Util::Abi::VsPs:
        typeStr = "VsPs";
        break;
    case Util::Abi::Gs:
        typeStr = "Gs";
        break;
    case Util::Abi::Cs:
        typeStr = "Cs";
        break;
    case Util::Abi::Ngg:
        typeStr = "Ngg";
        break;
    case Util::Abi::Tess:
        typeStr = "Tess";
        break;
    case Util::Abi::GsTess:
        typeStr = "GsTess";
        break;
    case Util::Abi::NggTess:
        typeStr = "NggTess";
        break;
    default:
        break;
    }
    m_pipelineNode[Util::Abi::PipelineMetadataKey::Type] = m_document->getNode(typeStr);
}

// =====================================================================================================================
// Set LDS byte size for given hardware shader stage
void ConfigBuilderBase::setLdsSizeByteSize(
    Util::Abi::HardwareStage hwStage, // Hardware shader stage
    unsigned                 value)   // Value to set
{
    if (value == 0)
        return; // Optional

    auto hwShaderNode = getHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = hwShaderNode.getDocument()->getNode(value);
}

// =====================================================================================================================
// Set ES-GS LDS byte size
void ConfigBuilderBase::setEsGsLdsSize(
    unsigned value) // Value to set
{
    if (value == 0)
        return; // Optional

    m_pipelineNode[Util::Abi::PipelineMetadataKey::EsGsLdsSize] = m_document->getNode(value);
}

// =====================================================================================================================
// Set USER_DATA_LIMIT (called once for the whole pipeline)
void ConfigBuilderBase::setUserDataLimit()
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::UserDataLimit] = m_document->getNode(m_userDataLimit);
}

// =====================================================================================================================
// Set SPILL_THRESHOLD (called once for the whole pipeline)
void ConfigBuilderBase::setSpillThreshold()
{
    m_pipelineNode[Util::Abi::PipelineMetadataKey::SpillThreshold] = m_document->getNode(m_spillThreshold);
}

// =====================================================================================================================
// Set PIPELINE_HASH (called once for the whole pipeline)
void ConfigBuilderBase::setPipelineHash()
{
    const auto& options = m_pipelineState->getOptions();

    auto pipelineHashNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
    pipelineHashNode[0] = m_document->getNode(options.hash[0]);
    pipelineHashNode[1] = m_document->getNode(options.hash[1]);
}

// =====================================================================================================================
/// Append a single entry to the PAL register metadata.
///
/// @param [in] key The metadata key (usually a register address).
/// @param [in] value The metadata value.
void ConfigBuilderBase::appendConfig(unsigned key, unsigned value)
{
    assert(key != InvalidMetadataKey);

    PalMetadataNoteEntry entry;
    entry.key = key;
    entry.value = value;
    m_config.push_back(entry);
}

// =====================================================================================================================
/// Append an array of entries to the PAL register metadata. Invalid keys are filtered out.
///
/// @param [in] config The array of register metadata entries.
void ConfigBuilderBase::appendConfig(llvm::ArrayRef<PalMetadataNoteEntry> config)
{
    unsigned count = 0;

    for (const auto &entry : config)
    {
        if (entry.key != InvalidMetadataKey)
            ++count;
    }

    unsigned idx = m_config.size();
    m_config.resize(idx + count);

    for (const auto &entry : config)
    {
        if (entry.key != InvalidMetadataKey)
            m_config[idx++] = entry;
    }
}

// =====================================================================================================================
// Write the config into PAL metadata in the LLVM IR module
void ConfigBuilderBase::writePalMetadata()
{
    // Set whole-pipeline values.
    setUserDataLimit();
    setSpillThreshold();
    setPipelineHash();

    // Generating MsgPack metadata.
    // Add the register values to the MsgPack document.
    msgpack::MapDocNode registers = m_pipelineNode[".registers"].getMap(true);
    for (const auto& entry : m_config)
    {
        assert(entry.key != InvalidMetadataKey);
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
    auto abiMetaString = MDString::get(m_module->getContext(), blob);
    auto abiMetaNode = MDNode::get(m_module->getContext(), abiMetaString);
    auto namedMeta = m_module->getOrInsertNamedMetadata("amdgpu.pal.metadata.msgpack");
    namedMeta->addOperand(abiMetaNode);
}

// =====================================================================================================================
// Sets up floating point mode from the specified floating point control flags.
unsigned ConfigBuilderBase::setupFloatingPointMode(
    ShaderStage shaderStage)    // Shader stage
{
    FloatMode floatMode = {};
    floatMode.bits.fp16fp64DenormMode = FP_DENORM_FLUSH_NONE;
    if (shaderStage != ShaderStageCopyShader)
    {
        const auto& shaderMode = m_pipelineState->getShaderModes()->getCommonShaderMode(shaderStage);

        // The HW rounding mode values happen to be one less than the FpRoundMode value, other than
        // FpRoundMode::DontCare, which we map to a default value.
        floatMode.bits.fp16fp64RoundMode = (shaderMode.fp16RoundMode != FpRoundMode::DontCare) ?
                                           static_cast<unsigned>(shaderMode.fp16RoundMode) - 1 :
                                           (shaderMode.fp64RoundMode != FpRoundMode::DontCare) ?
                                           static_cast<unsigned>(shaderMode.fp64RoundMode) - 1 :
                                           FP_ROUND_TO_NEAREST_EVEN;
        floatMode.bits.fp32RoundMode = (shaderMode.fp32RoundMode != FpRoundMode::DontCare) ?
                                       static_cast<unsigned>(shaderMode.fp32RoundMode) - 1 :
                                       FP_ROUND_TO_NEAREST_EVEN;

        // The denorm modes happen to be one less than the FpDenormMode value, other than
        // FpDenormMode::DontCare, which we map to a default value.
        floatMode.bits.fp16fp64DenormMode = (shaderMode.fp16DenormMode != FpDenormMode::DontCare) ?
                                            static_cast<unsigned>(shaderMode.fp16DenormMode) - 1 :
                                            (shaderMode.fp64DenormMode != FpDenormMode::DontCare) ?
                                            static_cast<unsigned>(shaderMode.fp64DenormMode) - 1 :
                                            FP_DENORM_FLUSH_NONE;
        floatMode.bits.fp32DenormMode = (shaderMode.fp32DenormMode != FpDenormMode::DontCare) ?
                                        static_cast<unsigned>(shaderMode.fp32DenormMode) - 1 :
                                        FP_DENORM_FLUSH_IN_OUT;
    }
    return floatMode.u32All;
}

