/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/VersionTuple.h"

#define DEBUG_TYPE "lgc-pal-metadata"

using namespace lgc;
using namespace llvm;

namespace {
// =====================================================================================================================
// Returns an ArrayDocNode associated with the given document that contains the given data.
//
// @param document : The msgpack document the returned array will be associated with.
// @param hash : The hash to place in the array node that will be built.
msgpack::ArrayDocNode buildArrayDocNode(msgpack::Document *document, Hash128 hash) {
  msgpack::ArrayDocNode childNode = document->getArrayNode();
  for (uint32_t i = 0; i < hash.size(); ++i)
    childNode[i] = hash[i];
  return childNode;
}
} // namespace

// =====================================================================================================================
// Construct empty object
PalMetadata::PalMetadata(PipelineState *pipelineState) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  initialize();
}

// =====================================================================================================================
// Constructor given blob of MsgPack PAL metadata
//
// @param pipelineState : PipelineState
// @param blob : MsgPack PAL metadata
PalMetadata::PalMetadata(PipelineState *pipelineState, StringRef blob) : m_pipelineState(pipelineState) {
  m_document = new msgpack::Document;
  bool success = m_document->readFromBlob(blob, /*multi=*/false);
  assert(success && "Bad PAL metadata format");
  ((void)success);
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
  if (m_userDataLimit->isEmpty())
    *m_userDataLimit = 0U;
  m_spillThreshold = &m_pipelineNode[Util::Abi::PipelineMetadataKey::SpillThreshold];
  if (m_spillThreshold->isEmpty())
    *m_spillThreshold = UINT_MAX;
}

// =====================================================================================================================
// Record the PAL metadata into IR metadata in the specified module. This is used both for passing the PAL metadata
// to the AMDGPU back-end, and when compilation stops early due to -stop-before etc.
//
// @param module : Pipeline IR module
void PalMetadata::record(Module *module) {
  // Add the metadata version number.
  auto versionNode = m_document->getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Version].getArray(true);
  versionNode[0] = Util::Abi::PipelineMetadataMajorVersion;
  versionNode[1] = Util::Abi::PipelineMetadataMinorVersion;

  // Write the MsgPack document into an IR metadata node.
  // The IR named metadata node contains an MDTuple containing an MDString containing the msgpack data.
  std::string blob;
  m_document->writeToBlob(blob);
  MDString *abiMetaString = MDString::get(module->getContext(), blob);
  MDNode *abiMetaNode = MDNode::get(module->getContext(), abiMetaString);
  NamedMDNode *namedMeta = module->getOrInsertNamedMetadata(PalMetadataName);

  if (namedMeta->getNumOperands() == 0) {
    namedMeta->addOperand(abiMetaNode);
  } else {
    // If the PAL metadata was already written, then we need to replace the previous value.
    assert(namedMeta->getNumOperands() == 1);
    namedMeta->setOperand(0, abiMetaNode);
  }
}

// =====================================================================================================================
// Read blob as PAL metadata and merge it into existing PAL metadata (if any)
//
// @param blob : MsgPack PAL metadata to merge
// @param isGlueCode : True if the blob is was generated for glue code.
void PalMetadata::mergeFromBlob(llvm::StringRef blob, bool isGlueCode) {
  // Use msgpack::Document::readFromBlob to read the new MsgPack PAL metadata, merging it into the msgpack::Document
  // we already have. We pass it a lambda that determines how to cope with merge conflicts, which returns:
  // -1: failure
  // 0: success; *dest has been set up with the merged node. For an array, 0 means overwrite the existing array
  //    rather than appending.
  bool success = m_document->readFromBlob(
      blob, /*multi=*/false,
      [isGlueCode](msgpack::DocNode *destNode, msgpack::DocNode srcNode, msgpack::DocNode mapKey) {
        // Allow array and map merging.
        if (srcNode.isMap() && destNode->isMap())
          return 0;
        if (srcNode.isArray() && destNode->isArray())
          return 0;
        // Allow string merging as long as the two strings have the same value. If one string has a "_fetchless"
        // suffix, take the other one. This is for the benefit of the linker linking a fetch shader with a
        // fetchless VS.
        if (destNode->isString() && srcNode.isString()) {
          if (destNode->getString() == srcNode.getString())
            return 0;
          if (srcNode.getString().endswith("_fetchless"))
            return 0;
          if (destNode->getString().endswith("_fetchless")) {
            *destNode = srcNode;
            return 0;
          }
          if (srcNode.getString() == "color_export_shader")
            return 0;
          if (destNode->getString() == "color_export_shader") {
            *destNode = srcNode;
            return 0;
          }
        }
        // Disallow merging other than uint.
        if (destNode->getKind() != msgpack::Type::UInt || srcNode.getKind() != msgpack::Type::UInt)
          return -1;
        // Special cases of uint merging.
        if (mapKey.getKind() == msgpack::Type::UInt) {
          switch (mapKey.getUInt()) {
          case mmVGT_SHADER_STAGES_EN:
            // Ignore new value of VGT_SHADER_STAGES_EN from glue shader, as it might accidentally make the VS
            // wave32. (This relies on the glue shader's PAL metadata being merged into the vertex-processing
            // half-pipeline, rather than the other way round.)
            return 0;
          case mmSPI_SHADER_PGM_RSRC1_LS:
          case mmSPI_SHADER_PGM_RSRC1_HS:
          case mmSPI_SHADER_PGM_RSRC1_ES:
          case mmSPI_SHADER_PGM_RSRC1_GS:
          case mmSPI_SHADER_PGM_RSRC1_VS:
          case mmSPI_SHADER_PGM_RSRC1_PS: {
            // For the RSRC1 registers, we need to consider the VGPRS and SGPRS fields separately, and max them.
            // This happens when linking in a glue shader.
            SPI_SHADER_PGM_RSRC1 destRsrc1;
            SPI_SHADER_PGM_RSRC1 srcRsrc1;
            SPI_SHADER_PGM_RSRC1 origRsrc1;
            origRsrc1.u32All = destNode->getUInt();
            srcRsrc1.u32All = srcNode.getUInt();
            destRsrc1.u32All = origRsrc1.u32All | srcRsrc1.u32All;
            destRsrc1.bits.VGPRS = std::max(origRsrc1.bits.VGPRS, srcRsrc1.bits.VGPRS);
            destRsrc1.bits.SGPRS = std::max(origRsrc1.bits.SGPRS, srcRsrc1.bits.SGPRS);
            if (isGlueCode) {
              // The float mode should come from the body of the shader and not the glue code.
              destRsrc1.bits.FLOAT_MODE = origRsrc1.bits.FLOAT_MODE;
            }
            *destNode = srcNode.getDocument()->getNode(destRsrc1.u32All);
            return 0;
          }
          case mmSPI_PS_INPUT_ENA:
          case mmSPI_PS_INPUT_ADDR:
          case mmSPI_PS_IN_CONTROL: {
            if (!isGlueCode) {
              *destNode = srcNode.getUInt();
            }
            return 0;
          }
          }
        } else if (mapKey.isString()) {
          // For .userdatalimit, register counts, and register limits, take the max value.
          if (mapKey.getString() == Util::Abi::PipelineMetadataKey::UserDataLimit ||
              mapKey.getString() == Util::Abi::HardwareStageMetadataKey::SgprCount ||
              mapKey.getString() == Util::Abi::HardwareStageMetadataKey::SgprLimit ||
              mapKey.getString() == Util::Abi::HardwareStageMetadataKey::VgprCount ||
              mapKey.getString() == Util::Abi::HardwareStageMetadataKey::VgprLimit) {
            *destNode = std::max(destNode->getUInt(), srcNode.getUInt());
            return 0;
          }
          // For .spillthreshold, take the min value.
          if (mapKey.getString() == Util::Abi::PipelineMetadataKey::SpillThreshold) {
            *destNode = std::min(destNode->getUInt(), srcNode.getUInt());
            return 0;
          }
        }
        // Default behavior for uint nodes: "or" the values together.
        *destNode = destNode->getUInt() | srcNode.getUInt();
        return 0;
      });
  assert(success && "Bad PAL metadata format");
  ((void)success);
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
  if (userDataValue == static_cast<unsigned>(UserDataMapping::SpillTable))
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
    *m_userDataLimit = userDataValue + dwordCount;

  // Although NumWorkgroupsPtr is a register pair, only the first word has a user data entry.
  if (userDataValue == static_cast<unsigned>(UserDataMapping::Workgroup))
    dwordCount = 1;

  // Write the register(s)
  userDataReg += userDataIndex;
  while (dwordCount--)
    m_registers[userDataReg++] = userDataValue++;
}

// =====================================================================================================================
// Mark that the user data spill table is used at the given offset. The SpillThreshold PAL metadata entry is
// set to the minimum of any call to this function in any shader.
//
// @param dwordOffset : Dword offset that the spill table is used at
void PalMetadata::setUserDataSpillUsage(unsigned dwordOffset) {
  if (dwordOffset < m_spillThreshold->getUInt())
    *m_spillThreshold = dwordOffset;
}

// =====================================================================================================================
// Fix up user data registers. Any user data register that has one of the unlinked UserDataMapping values defined
// in AbiUnlinked.h is fixed up by looking at pipeline state.
void PalMetadata::fixUpRegisters() {
  static const std::pair<unsigned, unsigned> ComputeRegRanges[] = {{mmCOMPUTE_USER_DATA_0, 16}};
  static const std::pair<unsigned, unsigned> Gfx8RegRanges[] = {
      {mmSPI_SHADER_USER_DATA_PS_0, 16}, {mmSPI_SHADER_USER_DATA_VS_0, 16}, {mmSPI_SHADER_USER_DATA_GS_0, 16},
      {mmSPI_SHADER_USER_DATA_ES_0, 16}, {mmSPI_SHADER_USER_DATA_HS_0, 16}, {mmSPI_SHADER_USER_DATA_LS_0, 16}};
  static const std::pair<unsigned, unsigned> Gfx9RegRanges[] = {{mmSPI_SHADER_USER_DATA_PS_0, 32},
                                                                {mmSPI_SHADER_USER_DATA_VS_0, 32},
                                                                {mmSPI_SHADER_USER_DATA_ES_0, 32},
                                                                {mmSPI_SHADER_USER_DATA_HS_0, 32}};
  static const std::pair<unsigned, unsigned> Gfx10RegRanges[] = {{mmSPI_SHADER_USER_DATA_PS_0, 32},
                                                                 {mmSPI_SHADER_USER_DATA_VS_0, 32},
                                                                 {mmSPI_SHADER_USER_DATA_GS_0, 32},
                                                                 {mmSPI_SHADER_USER_DATA_HS_0, 32}};

  ArrayRef<std::pair<unsigned, unsigned>> regRanges = ComputeRegRanges;
  if (m_pipelineState->isGraphics()) {
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 9)
      regRanges = Gfx8RegRanges;
    else if (m_pipelineState->getTargetInfo().getGfxIpVersion().major == 9)
      regRanges = Gfx9RegRanges;
    else
      regRanges = Gfx10RegRanges;
  }

  // First find the descriptor sets and push const nodes.
  SmallVector<const ResourceNode *, 4> descSetNodes;
  const ResourceNode *pushConstNode;
  for (const auto &node : m_pipelineState->getUserDataNodes()) {
    if (node.type == ResourceNodeType::DescriptorTableVaPtr && !node.innerTable.empty()) {
      size_t descSet = node.innerTable[0].set;
      descSetNodes.resize(std::max(descSetNodes.size(), descSet + 1));
      descSetNodes[descSet] = &node;
    } else if (node.type == ResourceNodeType::DescriptorBuffer) {
      size_t descSet = node.set;
      descSetNodes.resize(std::max(descSetNodes.size(), descSet + 1));
      descSetNodes[descSet] = &node;
    } else if (node.type == ResourceNodeType::PushConst) {
      pushConstNode = &node;
    }
  }

  // Scan the PAL metadata registers for user data.
  unsigned userDataLimit = m_userDataLimit->getUInt();
  for (const std::pair<unsigned, unsigned> &regRange : regRanges) {
    unsigned reg = regRange.first;
    unsigned regEnd = reg + regRange.second;
    // Scan registers [reg,regEnd), the user data registers for one shader stage. If register 0 in that range is
    // not set, then the shader stage is not in use, so don't bother to scan the others.
    auto it = m_registers.find(m_document->getNode(reg));
    if (it != m_registers.end()) {
      for (;;) {
        unsigned value = it->second.getUInt();
        unsigned descSet = value - static_cast<unsigned>(UserDataMapping::DescriptorSet0);
        if (descSet <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax) -
                           static_cast<unsigned>(UserDataMapping::DescriptorSet0)) {
          // This entry is a descriptor set pointer. Replace it with the dword offset for that descriptor set.
          if (descSet >= descSetNodes.size() || !descSetNodes[descSet])
            report_fatal_error("Descriptor set " + Twine(descSet) + " not found");
          value = descSetNodes[descSet]->offsetInDwords;
          it->second = value;
          unsigned extent = value + descSetNodes[descSet]->sizeInDwords;
          userDataLimit = std::max(userDataLimit, extent);
        } else {
          unsigned pushConstOffset = value - static_cast<unsigned>(UserDataMapping::PushConst0);
          if (pushConstOffset <= static_cast<unsigned>(UserDataMapping::DescriptorSetMax) -
                                     static_cast<unsigned>(UserDataMapping::DescriptorSet0)) {
            // This entry is a dword in the push constant.
            if (!pushConstNode || pushConstNode->sizeInDwords <= pushConstOffset)
              report_fatal_error("Push constant not found or not big enough");
            value = pushConstNode->offsetInDwords + pushConstOffset;
            it->second = value;
            unsigned extent = pushConstNode->offsetInDwords + pushConstNode->sizeInDwords;
            userDataLimit = std::max(userDataLimit, extent);
          }
        }
        ++it;
        if (it == m_registers.end() || it->first.getUInt() >= regEnd)
          break;
      }
    }
  }
  *m_userDataLimit = userDataLimit;
}

// =====================================================================================================================
// Finalize PAL metadata for pipeline.
// This is called at the end of a full pipeline compilation, or from the ELF link when doing shader/half-pipeline
// compilation.
void PalMetadata::finalizePipeline() {
  assert(!m_pipelineState->isUnlinked());

  // Set pipeline hash.
  auto pipelineHashNode = m_pipelineNode[Util::Abi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  const auto &options = m_pipelineState->getOptions();
  pipelineHashNode[0] = options.hash[0];
  pipelineHashNode[1] = options.hash[1];

  if (m_pipelineState->isGraphics()) {
    // Set PA_CL_CLIP_CNTL from pipeline state settings.
    // DX_CLIP_SPACE_DEF, ZCLIP_NEAR_DISABLE and ZCLIP_FAR_DISABLE are now set internally by PAL (as of
    // version 629), and are no longer part of the PAL ELF ABI.
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
    paClClipCntl.bits.DX_RASTERIZATION_KILL = rasterizerDiscardEnable;
    setRegister(mmPA_CL_CLIP_CNTL, paClClipCntl.u32All);

    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
      DB_SHADER_CONTROL dbShaderControl = {};
      dbShaderControl.u32All = getRegister(mmDB_SHADER_CONTROL);
      dbShaderControl.bitfields.ALPHA_TO_MASK_DISABLE =
          dbShaderControl.bitfields.MASK_EXPORT_ENABLE ||
          m_pipelineState->getColorExportState().alphaToCoverageEnable == false;
      setRegister(mmDB_SHADER_CONTROL, dbShaderControl.u32All);
    }

    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major == 10) {
      auto waveBreakSize = m_pipelineState->getShaderOptions(ShaderStageFragment).waveBreakSize;
      PA_SC_SHADER_CONTROL paScShaderControl = {};
      paScShaderControl.gfx10.WAVE_BREAK_REGION_SIZE = static_cast<unsigned int>(waveBreakSize);
      setRegister(mmPA_SC_SHADER_CONTROL, paScShaderControl.u32All);
    }

    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
      PA_SC_AA_CONFIG paScAaConfig = {};
      if (m_pipelineState->getRasterizerState().innerCoverage) {
        paScAaConfig.bitfields.COVERAGE_TO_SHADER_SELECT = INPUT_INNER_COVERAGE;
      } else {
        paScAaConfig.bitfields.COVERAGE_TO_SHADER_SELECT = INPUT_COVERAGE;
      }
      setRegister(mmPA_SC_AA_CONFIG, paScAaConfig.u32All);
    }
  }

  // If there are root user data nodes but none of them are used, adjust userDataLimit accordingly.
  if (m_userDataLimit->getUInt() == 0 && !m_pipelineState->getUserDataNodes().empty())
    setUserDataLimit();
}

// =====================================================================================================================
// Get a register value in PAL metadata.  Returns 0 if the node does not have an entry.
//
// @param regNum : Register number
unsigned PalMetadata::getRegister(unsigned regNum) {
  auto mapIt = m_registers.find(m_document->getNode(regNum));
  if (mapIt == m_registers.end()) {
    return 0;
  }
  msgpack::DocNode &node = mapIt->second;
  assert(node.getKind() == msgpack::Type::UInt);
  return node.getUInt();
}

// =====================================================================================================================
// Set a register value in PAL metadata.
// NOTE: If the register is already set, this ORs in the value.
//
// @param regNum : Register number
// @param value : Value to OR in
void PalMetadata::setRegister(unsigned regNum, unsigned value) {
  msgpack::DocNode &node = m_registers[regNum];
  if (node.getKind() == msgpack::Type::UInt)
    value |= node.getUInt();
  node = value;
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
  *m_userDataLimit = userDataLimit;
}

// =====================================================================================================================
// Store the vertex fetch information in PAL metadata for a fetchless vertex shader with shader compilation.
//
// @param info : Array of VertexFetchInfo structs
void PalMetadata::addVertexFetchInfo(ArrayRef<VertexFetchInfo> fetches) {
  // Each vertex fetch is an array containing {location,component,type}.
  // .vertexInputs is an array containing the vertex fetches.
  m_vertexInputs = m_pipelineNode[PipelineMetadataKey::VertexInputs].getArray(true);
  for (const VertexFetchInfo &fetch : fetches) {
    msgpack::ArrayDocNode fetchNode = m_document->getArrayNode();
    fetchNode.push_back(m_document->getNode(fetch.location));
    fetchNode.push_back(m_document->getNode(fetch.component));
    fetchNode.push_back(m_document->getNode(getTypeName(fetch.ty), /*copy=*/true));
    m_vertexInputs.push_back(fetchNode);
  }
}

// =====================================================================================================================
// Get the count of vertex fetches for a fetchless vertex shader with shader compilation (or 0 otherwise).
unsigned PalMetadata::getVertexFetchCount() {
  if (m_vertexInputs.isEmpty())
    return 0;
  return m_vertexInputs.size();
}

// =====================================================================================================================
// Get the vertex fetch information out of PAL metadata. Used by the linker to generate the fetch shader.
// Also removes the vertex fetch information, so it does not appear in the final linked ELF.
//
// @param [out] fetches : Vector to store info of each fetch
void PalMetadata::getVertexFetchInfo(SmallVectorImpl<VertexFetchInfo> &fetches) {
  if (m_vertexInputs.isEmpty()) {
    auto it = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::VertexInputs));
    if (it == m_pipelineNode.end() || !it->second.isArray())
      return;
    m_vertexInputs = it->second.getArray();
  }
  for (unsigned i = 0, e = m_vertexInputs.size(); i != e; ++i) {
    msgpack::ArrayDocNode fetchNode = m_vertexInputs[i].getArray();
    unsigned location = fetchNode[0].getUInt();
    unsigned component = fetchNode[1].getUInt();
    StringRef tyName = fetchNode[2].getString();
    Type *ty = getLlvmType(tyName);
    fetches.push_back({location, component, ty});
  }
  m_pipelineNode.erase(m_document->getNode(PipelineMetadataKey::VertexInputs));
}

// =====================================================================================================================
// Store the color export information in PAL metadata for an exportless fragment shader with shader compilation.
//
// @param info : Array of ColorExportInfo structs
void PalMetadata::addColorExportInfo(ArrayRef<ColorExportInfo> exports) {
  // Each color export is an array containing {location, original location,type}.
  // .colorExports is an array containing the color exports.
  auto colorExportArray = m_pipelineNode[PipelineMetadataKey::ColorExports].getArray(true);
  m_colorExports = colorExportArray;
  for (const ColorExportInfo &exportInfo : exports) {
    msgpack::ArrayDocNode fetchNode = m_document->getArrayNode();
    fetchNode.push_back(m_document->getNode(exportInfo.hwColorTarget));
    fetchNode.push_back(m_document->getNode(exportInfo.location));
    fetchNode.push_back(m_document->getNode(exportInfo.isSigned));
    fetchNode.push_back(m_document->getNode(getTypeName(exportInfo.ty), /*copy=*/true));
    colorExportArray.push_back(fetchNode);
  }
}

// =====================================================================================================================
// Get the count of color exports needed by the fragement shader.
unsigned PalMetadata::getColorExportCount() {
  if (m_colorExports.isEmpty())
    return 0;
  return m_colorExports.getArray(false).size();
}

// =====================================================================================================================
// Get the color export information out of PAL metadata. Used by the linker to generate the color export shader, and to
// finalize the PS register information.
//
// @param [out] exports : Vector to store info of each export
void PalMetadata::getColorExportInfo(llvm::SmallVectorImpl<ColorExportInfo> &exports) {
  if (m_colorExports.isEmpty()) {
    auto it = m_pipelineNode.find(m_document->getNode(PipelineMetadataKey::ColorExports));
    if (it == m_pipelineNode.end() || !it->second.isArray())
      return;
    m_colorExports = it->second.getArray();
    m_pipelineNode.erase(m_document->getNode(PipelineMetadataKey::ColorExports));
  }
  assert(m_colorExports.isArray());
  auto colorExportArray = m_colorExports.getArray(false);
  unsigned numColorExports = colorExportArray.size();
  for (unsigned i = 0; i != numColorExports; ++i) {
    msgpack::ArrayDocNode fetchNode = colorExportArray[i].getArray();
    unsigned hwMrt = fetchNode[0].getUInt();
    unsigned location = fetchNode[1].getUInt();
    bool isSigned = fetchNode[2].getBool();
    StringRef tyName = fetchNode[3].getString();
    Type *ty = getLlvmType(tyName);
    exports.push_back({hwMrt, location, isSigned, ty});
  }
}

// =====================================================================================================================
// Get the color export information out of PAL metadata. Used by the linker to generate the color export shader, and to
// finalize the PS register information.
//
void PalMetadata::eraseColorExportInfo() {
  m_colorExports = m_document->getEmptyNode();
  m_pipelineNode.erase(m_document->getNode(PipelineMetadataKey::ColorExports));
}

// =====================================================================================================================
// Get the VS entry register info. Used by the linker to generate the fetch shader.
//
// @param [out] regInfo : Where to store VS entry register info
void PalMetadata::getVsEntryRegInfo(VsEntryRegInfo &regInfo) {
  regInfo = {};

  // Find the first hardware shader stage that has SPI registers set. That must be the API VS, or the merged
  // shader containing it. User data register 0 is always set if a shader stage is active.
  static const std::pair<unsigned, unsigned> shaderTable[] = {{mmSPI_SHADER_USER_DATA_LS_0, CallingConv::AMDGPU_LS},
                                                              {mmSPI_SHADER_USER_DATA_HS_0, CallingConv::AMDGPU_HS},
                                                              {mmSPI_SHADER_USER_DATA_ES_0, CallingConv::AMDGPU_ES},
                                                              {mmSPI_SHADER_USER_DATA_GS_0, CallingConv::AMDGPU_GS},
                                                              {mmSPI_SHADER_USER_DATA_VS_0, CallingConv::AMDGPU_VS}};
  ArrayRef<std::pair<unsigned, unsigned>> shaderEntries = shaderTable;
  auto userDataRegIt = m_registers.begin();
  for (;;) {
    userDataRegIt = m_registers.find(m_document->getNode(shaderEntries[0].first));
    if (userDataRegIt != m_registers.end())
      break;
    shaderEntries = shaderEntries.drop_front();
  }
  unsigned userDataOffset = 0;
  unsigned userDataReg0 = shaderEntries[0].first;
  regInfo.callingConv = shaderEntries[0].second;
  if (userDataReg0 != mmSPI_SHADER_USER_DATA_VS_0 && m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9)
    userDataOffset = 8; // GFX9+ merged shader: extra 8 SGPRs beore user data

  // Scan the user data registers for vertex buffer table, vertex id, instance id.
  unsigned userDataCount = m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;
  for (;;) {
    switch (static_cast<UserDataMapping>(userDataRegIt->second.getUInt())) {
    case UserDataMapping::VertexBufferTable:
      regInfo.vertexBufferTable = userDataRegIt->first.getUInt() - userDataReg0 + userDataOffset;
      break;
    case UserDataMapping::BaseVertex:
      regInfo.baseVertex = userDataRegIt->first.getUInt() - userDataReg0 + userDataOffset;
      break;
    case UserDataMapping::BaseInstance:
      regInfo.baseInstance = userDataRegIt->first.getUInt() - userDataReg0 + userDataOffset;
      break;
    default:
      break;
    }
    if (++userDataRegIt == m_registers.end() || userDataRegIt->first.getUInt() >= userDataReg0 + userDataCount)
      break;
  }

  // Get the number of user data registers in this shader. For GFX9+, we ignore the USER_SGPR_MSB field; we
  // know that there is at least one user SGPR, so if we find that USER_SGPR is 0, it must mean 32.
  SPI_SHADER_PGM_RSRC2 rsrc2;
  rsrc2.u32All =
      m_registers[m_document->getNode(userDataReg0 + mmSPI_SHADER_PGM_RSRC2_VS - mmSPI_SHADER_USER_DATA_VS_0)]
          .getUInt();
  userDataCount = rsrc2.bits.USER_SGPR == 0 ? 32 : rsrc2.bits.USER_SGPR;

  // Conservatively set the total number of input SGPRs. A merged shader with 8 SGPRs before user data
  // does not have any extra ones after; an unmerged shader has up to 10 SGPRs after.
  regInfo.sgprCount = userDataOffset + userDataCount;
  if (userDataOffset == 0)
    regInfo.sgprCount += 10;

  // Set the VGPR numbers for vertex ID and instance ID, and the total number of input VGPRs, depending on the
  // shader stage. We know that instance ID is enabled, because it always is in a fetchless VS.
  // On GFX10, also get the wave32 bit.
  VGT_SHADER_STAGES_EN vgtShaderStagesEn;
  vgtShaderStagesEn.u32All = m_registers[m_document->getNode(mmVGT_SHADER_STAGES_EN)].getUInt();
  switch (regInfo.callingConv) {
  case CallingConv::AMDGPU_LS: // Before-GFX9 unmerged LS
  case CallingConv::AMDGPU_ES: // Before-GFX9 unmerged ES
  case CallingConv::AMDGPU_VS:
    regInfo.vertexId = 0;
    regInfo.instanceId = 3;
    regInfo.vgprCount = 4;
    regInfo.wave32 = vgtShaderStagesEn.gfx10.VS_W32_EN;
    break;
  case CallingConv::AMDGPU_HS: // GFX9+ LS+HS
    regInfo.vertexId = 2;
    regInfo.instanceId = 5;
    regInfo.vgprCount = 6;
    regInfo.wave32 = vgtShaderStagesEn.gfx10.HS_W32_EN;
    break;
  case CallingConv::AMDGPU_GS: // GFX9+ ES+GS
    regInfo.vertexId = 5;
    regInfo.instanceId = 8;
    regInfo.vgprCount = 9;
    regInfo.wave32 = vgtShaderStagesEn.gfx10.GS_W32_EN;
    break;
  default:
    llvm_unreachable("");
    break;
  }
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 10)
    regInfo.wave32 = false;
}

// =====================================================================================================================
// Get the llvm type for that corresponds to tyName.  Returns nullptr if no such type exists.
//
// @param tyName : A type name that is an encoding of a type.
llvm::Type *PalMetadata::getLlvmType(StringRef tyName) const {
  unsigned vecLength = 0;
  if (tyName[0] == 'v') {
    tyName = tyName.drop_front();
    tyName.consumeInteger(10, vecLength);
  }
  Type *ty = nullptr;
  if (tyName == "i8")
    ty = Type::getInt8Ty(m_pipelineState->getContext());
  else if (tyName == "i16")
    ty = Type::getInt16Ty(m_pipelineState->getContext());
  else if (tyName == "i32")
    ty = Type::getInt32Ty(m_pipelineState->getContext());
  else if (tyName == "i64")
    ty = Type::getInt64Ty(m_pipelineState->getContext());
  else if (tyName == "f16")
    ty = Type::getHalfTy(m_pipelineState->getContext());
  else if (tyName == "f32")
    ty = Type::getFloatTy(m_pipelineState->getContext());
  else if (tyName == "f64")
    ty = Type::getDoubleTy(m_pipelineState->getContext());
  if (vecLength != 0 && ty != nullptr)
    ty = FixedVectorType::get(ty, vecLength);
  return ty;
}

// =====================================================================================================================
// Updates the SPI_SHADER_COL_FORMAT entry.
//
void PalMetadata::updateSpiShaderColFormat(ArrayRef<ColorExportInfo> exps, bool hasDepthExpFmtZero, bool killEnabled) {
  unsigned spiShaderColFormat = 0;
  for (auto &exp : exps) {
    if (exp.hwColorTarget == MaxColorTargets)
      continue;
    unsigned expFormat = m_pipelineState->computeExportFormat(exp.ty, exp.location);
    spiShaderColFormat |= (expFormat << (4 * exp.hwColorTarget));
  }

  if (spiShaderColFormat == 0 && hasDepthExpFmtZero) {
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 10 || killEnabled) {
      // NOTE: Hardware requires that fragment shader always exports "something" (color or depth) to the SX.
      // If both SPI_SHADER_Z_FORMAT and SPI_SHADER_COL_FORMAT are zero, we need to override
      // SPI_SHADER_COL_FORMAT to export one channel to MRT0. This dummy export format will be masked
      // off by CB_SHADER_MASK.
      spiShaderColFormat = SPI_SHADER_32_R;
    }
  }
  setRegister(mmSPI_SHADER_COL_FORMAT, spiShaderColFormat);
}

// =====================================================================================================================
// Fills the xglCacheInfo section of the PAL metadata with the given data.
//
// @param finalizedCacheHash : The finalized hash that will be used to look for this pipeline in caches.
// @param version : The version of llpc that generated the hash.
void PalMetadata::setFinalized128BitCacheHash(const Hash128 &finalizedCacheHash, const VersionTuple &version) {
  msgpack::MapDocNode &xglCacheInfo = m_pipelineNode[Util::Abi::PipelineMetadataKey::XglCacheInfo].getMap(true);
  xglCacheInfo[Util::Abi::PipelineMetadataKey::CacheHash128Bits] = buildArrayDocNode(m_document, finalizedCacheHash);
  xglCacheInfo[Util::Abi::PipelineMetadataKey::LlpcVersion] = m_document->getNode(version.getAsString(),
                                                                                  /* copy = */ true);
}
