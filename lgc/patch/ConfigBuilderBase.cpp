/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ConfigBuilderBase.cpp
 * @brief LLPC source file: contains implementation of class lgc::ConfigBuilderBase.
 ***********************************************************************************************************************
 */
#include "ConfigBuilderBase.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Constants.h"

#define DEBUG_TYPE "lgc-config-builder-base"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
//
// @param [in/out] module : LLVM module
// @param pipelineState : Pipeline state
ConfigBuilderBase::ConfigBuilderBase(Module *module, PipelineState *pipelineState)
    : m_module(module), m_pipelineState(pipelineState) {
  m_context = &module->getContext();

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
  m_hasTask = m_pipelineState->hasShaderStage(ShaderStageTask);
  m_hasMesh = m_pipelineState->hasShaderStage(ShaderStageMesh);

  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  // Only generate MsgPack PAL metadata for PAL client 477 onwards. PAL changed the .note record type
  // from 13 to 32 at that point, and not using MsgPack metadata before that avoids some compatibility
  // problems.
  if (m_pipelineState->getPalAbiVersion() < 477)
    report_fatal_error("PAL ABI version less than 477 not supported");
  m_document = m_pipelineState->getPalMetadata()->getDocument();

  m_pipelineNode =
      m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0].getMap(true);

  setApiName(pipelineState->getClient());
}

// =====================================================================================================================
ConfigBuilderBase::~ConfigBuilderBase() {
}

// =====================================================================================================================
/// Adds the .shaders.$(apiStage).hardware_mapping node to the PAL metadata.
///
/// @param [in] apiStage : The API shader stage
/// @param [in] hwStages : The HW stage(s) that the API shader is mapped to, as a combination of
///                      @ref Util::Abi::HardwareStageFlagBits.
void ConfigBuilderBase::addApiHwShaderMapping(ShaderStage apiStage, unsigned hwStages) {
  auto hwMappingNode = getApiShaderNode(apiStage)[Util::Abi::ShaderMetadataKey::HardwareMapping].getArray(true);
  for (unsigned hwStage = 0; hwStage < unsigned(Util::Abi::HardwareStage::Count); ++hwStage) {
    if (hwStages & (1 << hwStage))
      hwMappingNode.push_back(m_document->getNode(HwStageNames[hwStage]));
  }
}

// =====================================================================================================================
// Get the MsgPack map node for the specified API shader in the ".shaders" map
//
// @param apiStage : API shader stage
msgpack::MapDocNode ConfigBuilderBase::getApiShaderNode(unsigned apiStage) {
  if (m_apiShaderNodes[apiStage].isEmpty()) {
    m_apiShaderNodes[apiStage] =
        m_pipelineNode[Util::Abi::PipelineMetadataKey::Shaders].getMap(true)[ApiStageNames[apiStage]].getMap(true);
  }
  return m_apiShaderNodes[apiStage];
}

// =====================================================================================================================
// Get the MsgPack map node for the specified hardware shader in the ".hardware_stages" map
//
// @param hwStage : Hardware shader stage
msgpack::MapDocNode ConfigBuilderBase::getHwShaderNode(Util::Abi::HardwareStage hwStage) {
  if (m_hwShaderNodes[unsigned(hwStage)].isEmpty()) {
    m_hwShaderNodes[unsigned(hwStage)] = m_pipelineNode[Util::Abi::PipelineMetadataKey::HardwareStages]
                                             .getMap(true)[HwStageNames[unsigned(hwStage)]]
                                             .getMap(true);
  }
  return m_hwShaderNodes[unsigned(hwStage)];
}

// =====================================================================================================================
// Set an API shader's hash in metadata. Returns a 32-bit value derived from the hash that is used as
// a shader checksum for performance profiling where applicable.
//
// @param apiStage : API shader stage
unsigned ConfigBuilderBase::setShaderHash(ShaderStage apiStage) {
  const ShaderOptions &shaderOptions = m_pipelineState->getShaderOptions(apiStage);
  auto hashNode = getApiShaderNode(unsigned(apiStage))[Util::Abi::ShaderMetadataKey::ApiShaderHash].getArray(true);
  hashNode[0] = shaderOptions.hash[0];
  hashNode[1] = shaderOptions.hash[1];
  return shaderOptions.hash[0] >> 32 ^ shaderOptions.hash[0] ^ shaderOptions.hash[1] >> 32 ^ shaderOptions.hash[1];
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_SGPRS for given hardware shader stage
//
// @param hwStage : Hardware shader stage
// @param value : Number of available SGPRs
void ConfigBuilderBase::setNumAvailSgprs(Util::Abi::HardwareStage hwStage, unsigned value) {
  auto hwShaderNode = getHwShaderNode(hwStage);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::SgprLimit] = value;
}

// =====================================================================================================================
// Set *S_NUM_AVAIL_VGPRS for given hardware shader stage
//
// @param hwStage : Hardware shader stage
// @param value : Number of available VGPRs
void ConfigBuilderBase::setNumAvailVgprs(Util::Abi::HardwareStage hwStage, unsigned value) {
  auto hwShaderNode = getHwShaderNode(hwStage);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::VgprLimit] = value;
}

// =====================================================================================================================
// Set USES_VIEWPORT_ARRAY_INDEX
//
// @param value : Value to set
void ConfigBuilderBase::setUsesViewportArrayIndex(bool value) {
  if (!value)
    return; // Optional

  m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex] = value;
}

// =====================================================================================================================
// Set PS_USES_UAVS
//
// @param value : Value to set
void ConfigBuilderBase::setPsUsesUavs(bool value) {
  if (!value)
    return; // Optional

  getHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::UsesUavs] = value;
}

// =====================================================================================================================
// Set PS_WRITES_UAVS
//
// @param value : Value to set
void ConfigBuilderBase::setPsWritesUavs(bool value) {
  if (!value)
    return; // Optional

  getHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::WritesUavs] = value;
}

// =====================================================================================================================
// Set PS_WRITES_DEPTH
//
// @param value : Value to set
void ConfigBuilderBase::setPsWritesDepth(bool value) {
  if (!value)
    return; // Optional

  getHwShaderNode(Util::Abi::HardwareStage::Ps)[Util::Abi::HardwareStageMetadataKey::WritesDepth] = value;
}

// =====================================================================================================================
// Set SampleMask
//
// @param value : Value to set
void ConfigBuilderBase::setPsSampleMask(bool value) {
  if (!value)
    return; // Optional

  m_pipelineNode[Util::Abi::PipelineMetadataKey::PsSampleMask] = value;
}

// =====================================================================================================================
// Set ES_GS_LDS_BYTE_SIZE
//
// @param value : Value to set
void ConfigBuilderBase::setEsGsLdsByteSize(unsigned value) {
  m_pipelineNode[Util::Abi::PipelineMetadataKey::EsGsLdsSize] = value;
}

// =====================================================================================================================
// Set hardware stage wavefront
//
// @param hwStage : Hardware shader stage
// @param value : Value to set
void ConfigBuilderBase::setWaveFrontSize(Util::Abi::HardwareStage hwStage, unsigned value) {
  if (m_pipelineState->getPalAbiVersion() >= 495) {
    auto hwShaderNode = getHwShaderNode(hwStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::WavefrontSize] = value;
  }
}

// =====================================================================================================================
// Set API name
//
// @param value : Value to set
void ConfigBuilderBase::setApiName(const char *value) {
  m_pipelineNode[Util::Abi::PipelineMetadataKey::Api] = value;
}

// =====================================================================================================================
// Set pipeline type
//
// @param value : Value to set
void ConfigBuilderBase::setPipelineType(Util::Abi::PipelineType value) {
  const char *typeStr = "";
  switch (value) {
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
  case Util::Abi::Mesh:
    typeStr = "Mesh";
    break;
  case Util::Abi::TaskMesh:
    typeStr = "TaskMesh";
    break;
  default:
    break;
  }
  m_pipelineNode[Util::Abi::PipelineMetadataKey::Type] = typeStr;
}

// =====================================================================================================================
// Set LDS byte size for given hardware shader stage
//
// @param hwStage : Hardware shader stage
// @param value : Value to set
void ConfigBuilderBase::setLdsSizeByteSize(Util::Abi::HardwareStage hwStage, unsigned value) {
  if (value == 0)
    return; // Optional

  auto hwShaderNode = getHwShaderNode(hwStage);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = value;
}

// =====================================================================================================================
// Set ES-GS LDS byte size
//
// @param value : Value to set
void ConfigBuilderBase::setEsGsLdsSize(unsigned value) {
  if (value == 0)
    return; // Optional

  m_pipelineNode[Util::Abi::PipelineMetadataKey::EsGsLdsSize] = value;
}

// =====================================================================================================================
// Set NGG sub-group size
//
// @param value : Value to set
void ConfigBuilderBase::setNggSubgroupSize(unsigned value) {
  assert(value != 0);
  m_pipelineNode[Util::Abi::PipelineMetadataKey::NggSubgroupSize] = value;
}

// =====================================================================================================================
// Set thread group dimensions
//
// @param values : Values to set
void ConfigBuilderBase::setThreadgroupDimensions(llvm::ArrayRef<unsigned> values) {
  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Cs);
  auto &arrayNode = hwShaderNode[Util::Abi::HardwareStageMetadataKey::ThreadgroupDimensions].getArray(true);
  for (unsigned i = 0; i < values.size(); ++i)
    arrayNode[i] = values[i];
}

// =====================================================================================================================
/// Append a single entry to the PAL register metadata.
///
/// @param [in] key : The metadata key (usually a register address).
/// @param [in] value : The metadata value.
void ConfigBuilderBase::appendConfig(unsigned key, unsigned value) {
  assert(key != InvalidMetadataKey);

  PalMetadataNoteEntry entry;
  entry.key = key;
  entry.value = value;
  m_config.push_back(entry);
}

// =====================================================================================================================
/// Append an array of entries to the PAL register metadata. Invalid keys are filtered out.
///
/// @param [in] config : The array of register metadata entries.
void ConfigBuilderBase::appendConfig(ArrayRef<PalMetadataNoteEntry> config) {
  unsigned count = 0;

  for (const auto &entry : config) {
    if (entry.key != InvalidMetadataKey)
      ++count;
  }

  unsigned idx = m_config.size();
  m_config.resize(idx + count);

  for (const auto &entry : config) {
    if (entry.key != InvalidMetadataKey)
      m_config[idx++] = entry;
  }
}

// =====================================================================================================================
// Whether USES_VIEWPORT_ARRAY_INDEX is set
bool ConfigBuilderBase::usesViewportArrayIndex() {
  if (m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex].isEmpty())
    m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex] = false;

  return m_pipelineNode[Util::Abi::PipelineMetadataKey::UsesViewportArrayIndex].getBool();
}

// =====================================================================================================================
// Finish ConfigBuilder processing by writing into the PalMetadata document
void ConfigBuilderBase::writePalMetadata() {
  // Generating MsgPack metadata.
  // Add the register values to the MsgPack document. The value is ORed in because an earlier pass may have
  // already set some bits in the same register.
  msgpack::MapDocNode registers = m_pipelineNode[".registers"].getMap(true);
  for (const auto &entry : m_config) {
    assert(entry.key != InvalidMetadataKey);
    auto key = entry.key;
    auto &regEntry = registers[key];
    unsigned oredValue = entry.value;
    if (regEntry.getKind() == msgpack::Type::UInt)
      oredValue = regEntry.getUInt();
    regEntry = oredValue;
  }
}

// =====================================================================================================================
// Sets up floating point mode from the specified floating point control flags.
//
// @param shaderStage : Shader stage
unsigned ConfigBuilderBase::setupFloatingPointMode(ShaderStage shaderStage) {
  FloatMode floatMode = {};
  floatMode.bits.fp16fp64DenormMode = FP_DENORM_FLUSH_NONE;
  if (shaderStage != ShaderStageCopyShader) {
    const auto &shaderMode = m_pipelineState->getShaderModes()->getCommonShaderMode(shaderStage);

    // The HW rounding mode values happen to be one less than the FpRoundMode value, other than
    // FpRoundMode::DontCare, which we map to a default value.
    floatMode.bits.fp16fp64RoundMode = shaderMode.fp16RoundMode != FpRoundMode::DontCare
                                           ? static_cast<unsigned>(shaderMode.fp16RoundMode) - 1
                                           : shaderMode.fp64RoundMode != FpRoundMode::DontCare
                                                 ? static_cast<unsigned>(shaderMode.fp64RoundMode) - 1
                                                 : FP_ROUND_TO_NEAREST_EVEN;
    floatMode.bits.fp32RoundMode = shaderMode.fp32RoundMode != FpRoundMode::DontCare
                                       ? static_cast<unsigned>(shaderMode.fp32RoundMode) - 1
                                       : FP_ROUND_TO_NEAREST_EVEN;

    // The denorm modes happen to be one less than the FpDenormMode value, other than
    // FpDenormMode::DontCare, which we map to a default value.
    floatMode.bits.fp16fp64DenormMode = shaderMode.fp16DenormMode != FpDenormMode::DontCare
                                            ? static_cast<unsigned>(shaderMode.fp16DenormMode) - 1
                                            : shaderMode.fp64DenormMode != FpDenormMode::DontCare
                                                  ? static_cast<unsigned>(shaderMode.fp64DenormMode) - 1
                                                  : FP_DENORM_FLUSH_NONE;
    floatMode.bits.fp32DenormMode = shaderMode.fp32DenormMode != FpDenormMode::DontCare
                                        ? static_cast<unsigned>(shaderMode.fp32DenormMode) - 1
                                        : FP_DENORM_FLUSH_IN_OUT;
  }
  return floatMode.u32All;
}
