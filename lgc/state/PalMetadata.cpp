/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PalMetadata.cpp
 * @brief LLPC source file: PalMetadata class for manipulating PAL metadata
 *
 * The PalMetadata object can be retrieved using PipelineState::getPalMetadata(), and is used by various parts
 * of LGC to write information to PAL metadata at the time the information is generated. The PalMetadata object
 * is carried through the middle-end, and serialized to IR metadata at the end of the middle-end (or at the
 * point -stop-before etc stops compilation, if earlier).
 ***********************************************************************************************************************
 */
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "lgc-pal-metadata"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Construct empty object
PalMetadata::PalMetadata(PipelineState *pipelineState) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  initialize();
}

// =====================================================================================================================
// Constructor given pipeline IR module. This reads the already-existing PAL metadata if any.
//
// @param module : Pipeline IR module
PalMetadata::PalMetadata(PipelineState *pipelineState, Module *module) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  NamedMDNode *namedMd = module->getNamedMetadata(PalMetadataName);
  if (namedMd && namedMd->getNumOperands()) {
    // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
    auto mdTuple = dyn_cast<MDTuple>(namedMd->getOperand(0));
    if (mdTuple && mdTuple->getNumOperands()) {
      if (auto mdString = dyn_cast<MDString>(mdTuple->getOperand(0))) {
        bool success = m_document->readFromBlob(mdString->getString(), /*multi=*/false);
        assert(success && "Bad PAL metadata format");
        ((void)success);
      }
    }
  }
  initialize();
}

// =====================================================================================================================
// Destructor
PalMetadata::~PalMetadata() {
  delete m_document;
}

// =====================================================================================================================
// Initialize the PalMetadata object after reading in already-existing PAL metadata if any
void PalMetadata::initialize() {
  // Pre-find (or create) heavily used nodes.
  m_pipelineNode =
      m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines].getArray(true)[0].getMap(true);
  m_registers = m_pipelineNode[".registers"].getMap(true);
  m_userDataLimit = &m_pipelineNode[Util::Abi::PipelineMetadataKey::UserDataLimit];
  if (m_userDataLimit->getKind() == msgpack::Type::Nil)
    *m_userDataLimit = m_document->getNode(0U);
  m_spillThreshold = &m_pipelineNode[Util::Abi::PipelineMetadataKey::SpillThreshold];
  if (m_spillThreshold->getKind() == msgpack::Type::Nil)
    *m_spillThreshold = m_document->getNode(UINT_MAX);
}

// =====================================================================================================================
// Record the PAL metadata into IR metadata in the specified module. This is used both for passing the PAL metadata
// to the AMDGPU back-end, and when compilation stops early due to -stop-before etc.
//
// @param module : Pipeline IR module
void PalMetadata::record(Module *module) {
  // Add the metadata version number.
  auto versionNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Version].getArray(true);
  versionNode[0] = m_document->getNode(Util::Abi::PipelineMetadataMajorVersion);
  versionNode[1] = m_document->getNode(Util::Abi::PipelineMetadataMinorVersion);

  // Write the MsgPack document into an IR metadata node.
  // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
  std::string blob;
  m_document->writeToBlob(blob);
  MDString *abiMetaString = MDString::get(module->getContext(), blob);
  MDNode *abiMetaNode = MDNode::get(module->getContext(), abiMetaString);
  NamedMDNode *namedMeta = module->getOrInsertNamedMetadata(PalMetadataName);
  namedMeta->addOperand(abiMetaNode);
}

// =====================================================================================================================
// Get the first user data register number for the given shader stage, taking into account what shader
// stages are present in the pipeline, and whether NGG is enabled. The first time this is called must be
// after PatchResourceCollect has run.
//
// @param stage : ShaderStage
unsigned PalMetadata::getUserDataReg0(ShaderStage stage) {
  if (m_userDataRegMapping[stage] != 0)
    return m_userDataRegMapping[stage];

  // Mapping not yet initialized.
  // Set up ShaderStage -> user data register mapping.
  m_userDataRegMapping[ShaderStageCompute] = mmCOMPUTE_USER_DATA_0;
  m_userDataRegMapping[ShaderStageFragment] = mmSPI_SHADER_USER_DATA_PS_0;

  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 9) {
    // <=GFX8: No merged shaders.
    m_userDataRegMapping[ShaderStageCopyShader] = mmSPI_SHADER_USER_DATA_VS_0;
    m_userDataRegMapping[ShaderStageGeometry] = mmSPI_SHADER_USER_DATA_GS_0;
    if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      m_userDataRegMapping[ShaderStageTessEval] = mmSPI_SHADER_USER_DATA_ES_0;
    else
      m_userDataRegMapping[ShaderStageTessEval] = mmSPI_SHADER_USER_DATA_VS_0;
    m_userDataRegMapping[ShaderStageTessControl] = mmSPI_SHADER_USER_DATA_HS_0;
    if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
      m_userDataRegMapping[ShaderStageVertex] = mmSPI_SHADER_USER_DATA_LS_0;
    else if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      m_userDataRegMapping[ShaderStageVertex] = mmSPI_SHADER_USER_DATA_ES_0;
    else
      m_userDataRegMapping[ShaderStageVertex] = mmSPI_SHADER_USER_DATA_VS_0;

  } else if (m_pipelineState->getTargetInfo().getGfxIpVersion().major == 9) {
    // GFX9: Merged shaders, and merged ES-GS user data goes into ES registers.
    m_userDataRegMapping[ShaderStageCopyShader] = mmSPI_SHADER_USER_DATA_VS_0;
    m_userDataRegMapping[ShaderStageGeometry] = mmSPI_SHADER_USER_DATA_ES_0;
    if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      m_userDataRegMapping[ShaderStageTessEval] = m_userDataRegMapping[ShaderStageGeometry];
    else
      m_userDataRegMapping[ShaderStageTessEval] = mmSPI_SHADER_USER_DATA_VS_0;
    m_userDataRegMapping[ShaderStageTessControl] = mmSPI_SHADER_USER_DATA_HS_0;
    if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
      m_userDataRegMapping[ShaderStageVertex] = m_userDataRegMapping[ShaderStageTessControl];
    else if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      m_userDataRegMapping[ShaderStageVertex] = m_userDataRegMapping[ShaderStageGeometry];
    else
      m_userDataRegMapping[ShaderStageVertex] = mmSPI_SHADER_USER_DATA_VS_0;

  } else if (!m_pipelineState->getNggControl()->enableNgg) {
    // GFX10+ not NGG: Same as GFX9, except ES-GS user data goes into GS registers.
    m_userDataRegMapping[ShaderStageCopyShader] = mmSPI_SHADER_USER_DATA_VS_0;
    m_userDataRegMapping[ShaderStageGeometry] = mmSPI_SHADER_USER_DATA_GS_0;
    if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      m_userDataRegMapping[ShaderStageTessEval] = m_userDataRegMapping[ShaderStageGeometry];
    else
      m_userDataRegMapping[ShaderStageTessEval] = mmSPI_SHADER_USER_DATA_VS_0;
    m_userDataRegMapping[ShaderStageTessControl] = mmSPI_SHADER_USER_DATA_HS_0;
    if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
      m_userDataRegMapping[ShaderStageVertex] = m_userDataRegMapping[ShaderStageTessControl];
    else if (m_pipelineState->hasShaderStage(ShaderStageGeometry))
      m_userDataRegMapping[ShaderStageVertex] = m_userDataRegMapping[ShaderStageGeometry];
    else
      m_userDataRegMapping[ShaderStageVertex] = mmSPI_SHADER_USER_DATA_VS_0;

  } else {
    // GFX10+ NGG
    m_userDataRegMapping[ShaderStageGeometry] = mmSPI_SHADER_USER_DATA_GS_0;
    m_userDataRegMapping[ShaderStageTessEval] = m_userDataRegMapping[ShaderStageGeometry];
    m_userDataRegMapping[ShaderStageTessControl] = mmSPI_SHADER_USER_DATA_HS_0;
    if (m_pipelineState->hasShaderStage(ShaderStageTessControl))
      m_userDataRegMapping[ShaderStageVertex] = m_userDataRegMapping[ShaderStageTessControl];
    else
      m_userDataRegMapping[ShaderStageVertex] = m_userDataRegMapping[ShaderStageGeometry];
  }

  return m_userDataRegMapping[stage];
}

// =====================================================================================================================
// Set the PAL metadata SPI register for a number of consecutive user data entries
//
// @param stage : ShaderStage
// @param userDataIndex : User data index 0-15 or 0-31 depending on HW and shader stage
// @param userDataValue : Value to store in that entry, one of:
//                        - a 0-based integer for the root user data dword offset
//                        - DescRelocMagic+set_number for an unlinked descriptor set number
//                        - one of the UserDataMapping values, e.g. UserDataMapping::GlobalTable
// @param dwordCount : Number of user data entries to set
void PalMetadata::setUserDataEntry(ShaderStage stage, unsigned userDataIndex, unsigned userDataValue,
                                   unsigned dwordCount) {
  // If this is the spill table pointer, adjust userDataLimit accordingly.
  if (userDataValue == static_cast<unsigned>(Util::Abi::UserDataMapping::SpillTable))
    setUserDataLimit();

  // Get the start register number of SPI user data registers for this shader stage.
  unsigned userDataReg = getUserDataReg0(stage);

  // Assert that the supplied user data index is not too big.
  assert(userDataIndex + dwordCount <= 32 &&
         (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9 || stage != ShaderStageCompute ||
          userDataIndex + dwordCount <= 16) &&
         "Out of range user data index");

  // Update userDataLimit if userData is a 0-based integer for root user data dword offset.
  if (userDataValue < InterfaceData::MaxSpillTableSize && userDataValue + dwordCount > m_userDataLimit->getUInt())
    *m_userDataLimit = m_document->getNode(userDataValue + dwordCount);

  // Although NumWorkgroupsPtr is a register pair, only the first word has a user data entry.
  if (userDataValue == static_cast<unsigned>(Util::Abi::UserDataMapping::Workgroup))
    dwordCount = 1;

  // Write the register(s)
  userDataReg += userDataIndex;
  while (dwordCount--)
    m_registers[m_document->getNode(userDataReg++)] = m_document->getNode(userDataValue++);
}

// =====================================================================================================================
// Mark that the user data spill table is used at the given offset. The SpillThreshold PAL metadata entry is
// set to the minimum of any call to this function in any shader.
//
// @param dwordOffset : Dword offset that the spill table is used at
void PalMetadata::setUserDataSpillUsage(unsigned dwordOffset) {
  if (dwordOffset < m_spillThreshold->getUInt())
    *m_spillThreshold = m_document->getNode(dwordOffset);
}

// =====================================================================================================================
// Finalize PAL metadata for pipeline.
// TODO Shader compilation: The idea is that this will be called at the end of a pipeline compilation, or in
// an ELF link, but not at the end of a shader/half-pipeline compile.
void PalMetadata::finalizePipeline() {
  // Set pipeline hash.
  auto pipelineHashNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  const auto &options = m_pipelineState->getOptions();
  pipelineHashNode[0] = m_document->getNode(options.hash[0]);
  pipelineHashNode[1] = m_document->getNode(options.hash[1]);

  // Set PA_CL_CLIP_CNTL from pipeline state settings.
  bool depthClipDisable = !m_pipelineState->getViewportState().depthClipEnable;
  uint8_t usrClipPlaneMask = m_pipelineState->getRasterizerState().usrClipPlaneMask;
  bool rasterizerDiscardEnable = m_pipelineState->getRasterizerState().rasterizerDiscardEnable;
  PA_CL_CLIP_CNTL paClClipCntl = {};
  paClClipCntl.bits.UCP_ENA_0 = (usrClipPlaneMask >> 0) & 0x1;
  paClClipCntl.bits.UCP_ENA_1 = (usrClipPlaneMask >> 1) & 0x1;
  paClClipCntl.bits.UCP_ENA_2 = (usrClipPlaneMask >> 2) & 0x1;
  paClClipCntl.bits.UCP_ENA_3 = (usrClipPlaneMask >> 3) & 0x1;
  paClClipCntl.bits.UCP_ENA_4 = (usrClipPlaneMask >> 4) & 0x1;
  paClClipCntl.bits.UCP_ENA_5 = (usrClipPlaneMask >> 5) & 0x1;
  paClClipCntl.bits.DX_LINEAR_ATTR_CLIP_ENA = true;
  paClClipCntl.bits.DX_CLIP_SPACE_DEF = true; // DepthRange::ZeroToOne
  paClClipCntl.bits.ZCLIP_NEAR_DISABLE = depthClipDisable;
  paClClipCntl.bits.ZCLIP_FAR_DISABLE = depthClipDisable;
  paClClipCntl.bits.DX_RASTERIZATION_KILL = rasterizerDiscardEnable;
  setRegister(mmPA_CL_CLIP_CNTL, paClClipCntl.u32All);

  // If there are root user data nodes but none of them are used, adjust userDataLimit accordingly.
  if (m_userDataLimit->getUInt() == 0 && !m_pipelineState->getUserDataNodes().empty())
    setUserDataLimit();
}

// =====================================================================================================================
// Set a register value in PAL metadata.
// NOTE: If the register is already set, this ORs in the value.
//
// @param regNum : Register number
// @param value : Value to OR in
void PalMetadata::setRegister(unsigned regNum, unsigned value) {
  msgpack::DocNode &node = m_registers[m_document->getNode(regNum)];
  if (node.getKind() == msgpack::Type::UInt)
    value |= node.getUInt();
  node = m_document->getNode(value);
}

// =====================================================================================================================
// Set userDataLimit to maximum (the size of the root user data table, excluding vertex buffer and streamout).
// This is called if spill is in use, or if there are root user data nodes but none of them are used (PAL does
// not like userDataLimit being 0 if there are any root user data nodes).
void PalMetadata::setUserDataLimit() {
  unsigned userDataLimit = 0;
  for (auto &node : m_pipelineState->getUserDataNodes()) {
    if (node.type != ResourceNodeType::IndirectUserDataVaPtr && node.type != ResourceNodeType::StreamOutTableVaPtr)
      userDataLimit = std::max(userDataLimit, node.offsetInDwords + node.sizeInDwords);
  }
  *m_userDataLimit = m_document->getNode(userDataLimit);
}
