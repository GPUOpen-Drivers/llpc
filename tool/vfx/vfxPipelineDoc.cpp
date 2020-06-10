/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  vfxPipelineDoc.cpp
* @brief Contains implementation of class PipelineDocument
***********************************************************************************************************************
*/
#include "vfx.h"
#include "vfxSection.h"

#ifndef VFX_DISABLE_SPVGEN
#if VFX_INSIDE_SPVGEN
#define SH_EXPORTING
#endif
#include "spvgen.h"
#endif

#if VFX_SUPPORT_VK_PIPELINE
#include "vfxPipelineDoc.h"
#include "vfxVkSection.h"

using namespace Vkgc;

namespace Vfx {
// =====================================================================================================================
// Gets max section count for PipelineDocument
unsigned PipelineDocument::getMaxSectionCount(SectionType type) {
  unsigned maxSectionCount = 0;
  switch (type) {
  case SectionTypeVersion:
    maxSectionCount = 1;
    break;
  case SectionTypeCompileLog:
    maxSectionCount = 1;
    break;
  case SectionTypeGraphicsState:
    maxSectionCount = 1;
    break;
  case SectionTypeComputeState:
    maxSectionCount = 1;
    break;
  case SectionTypeVertexInputState:
    maxSectionCount = 1;
    break;
  case SectionTypeShader:
    maxSectionCount = UINT32_MAX;
    break;
  case SectionTypeShaderInfo:
    maxSectionCount = UINT32_MAX;
    break;
  default:
    break;
  }
  return maxSectionCount;
};

// =====================================================================================================================
// Checks whether the input version is supportted.
//
// @param ver : Version
bool PipelineDocument::checkVersion(unsigned ver) {
  bool result = true;

  // Report error if input version higher than the version in header file
  if (ver > Version) {
    PARSE_ERROR(m_errorMsg, 0, "Unsupported version: %u (max version = %u)", ver, Version);
    result = false;
  }
  return result;
}

// =====================================================================================================================
// Gets PiplineDocument content
VfxPipelineStatePtr PipelineDocument::getDocument() {
  // Section "Version"
  m_pipelineState.version = Version;

  // Section "GraphicsPipelineState"
  if (m_sections[SectionTypeGraphicsState].size() > 0) {
    GraphicsPipelineState graphicState;
    m_pipelineState.pipelineType = VfxPipelineTypeGraphics;
    reinterpret_cast<SectionGraphicsState *>(m_sections[SectionTypeGraphicsState][0])
        ->getSubState(m_fileName, graphicState, &m_errorMsg);
    auto gfxPipelineInfo = &m_pipelineState.gfxPipelineInfo;
    gfxPipelineInfo->iaState.topology = graphicState.topology;
    gfxPipelineInfo->iaState.patchControlPoints = graphicState.patchControlPoints;
    gfxPipelineInfo->iaState.deviceIndex = graphicState.deviceIndex;
    gfxPipelineInfo->iaState.disableVertexReuse = graphicState.disableVertexReuse != 0;
    gfxPipelineInfo->iaState.switchWinding = graphicState.switchWinding != 0;
    gfxPipelineInfo->iaState.enableMultiView = graphicState.enableMultiView != 0;
    gfxPipelineInfo->vpState.depthClipEnable = graphicState.depthClipEnable != 0;
    gfxPipelineInfo->rsState.rasterizerDiscardEnable = graphicState.rasterizerDiscardEnable != 0;
    gfxPipelineInfo->rsState.perSampleShading = graphicState.perSampleShading != 0;
    gfxPipelineInfo->rsState.numSamples = graphicState.numSamples;
    gfxPipelineInfo->rsState.samplePatternIdx = graphicState.samplePatternIdx;
    gfxPipelineInfo->rsState.usrClipPlaneMask = static_cast<uint8_t>(graphicState.usrClipPlaneMask);
    gfxPipelineInfo->rsState.polygonMode = graphicState.polygonMode;
    gfxPipelineInfo->rsState.cullMode = graphicState.cullMode;
    gfxPipelineInfo->rsState.frontFace = graphicState.frontFace;
    gfxPipelineInfo->rsState.depthBiasEnable = graphicState.depthBiasEnable != 0;

    gfxPipelineInfo->cbState.alphaToCoverageEnable = graphicState.alphaToCoverageEnable != 0;
    gfxPipelineInfo->cbState.dualSourceBlendEnable = graphicState.dualSourceBlendEnable != 0;
    for (unsigned i = 0; i < MaxColorTargets; ++i) {
      gfxPipelineInfo->cbState.target[i].format = graphicState.colorBuffer[i].format;
      gfxPipelineInfo->cbState.target[i].channelWriteMask =
          static_cast<uint8_t>(graphicState.colorBuffer[i].channelWriteMask);
      gfxPipelineInfo->cbState.target[i].blendEnable = graphicState.colorBuffer[i].blendEnable != 0;
      gfxPipelineInfo->cbState.target[i].blendSrcAlphaToColor = graphicState.colorBuffer[i].blendSrcAlphaToColor != 0;
    }

    gfxPipelineInfo->options = graphicState.options;
    gfxPipelineInfo->nggState = graphicState.nggState;
  }

  // Section "ComputePipelineState"
  if (m_sections[SectionTypeComputeState].size() > 0) {
    ComputePipelineState computeState;
    m_pipelineState.pipelineType = VfxPipelineTypeCompute;
    reinterpret_cast<SectionComputeState *>(m_sections[SectionTypeComputeState][0])
        ->getSubState(m_fileName, computeState, &m_errorMsg);
    auto computePipelineInfo = &m_pipelineState.compPipelineInfo;
    computePipelineInfo->deviceIndex = computeState.deviceIndex;
    computePipelineInfo->options = computeState.options;
    computePipelineInfo->cs.entryStage = Vkgc::ShaderStageCompute;
  }

  // Section "VertexInputState"
  if (m_sections[SectionTypeVertexInputState].size() > 0) {
    reinterpret_cast<SectionVertexInput *>(m_sections[SectionTypeVertexInputState][0])->getSubState(m_vertexInputState);
    m_pipelineState.gfxPipelineInfo.pVertexInput = &m_vertexInputState;
  }

  if (m_pipelineState.pipelineType == VfxPipelineTypeGraphics ||
      m_pipelineState.pipelineType == VfxPipelineTypeCompute) {
    PipelineShaderInfo *shaderInfo[NativeShaderStageCount] = {
        &m_pipelineState.gfxPipelineInfo.vs, &m_pipelineState.gfxPipelineInfo.tcs, &m_pipelineState.gfxPipelineInfo.tes,
        &m_pipelineState.gfxPipelineInfo.gs, &m_pipelineState.gfxPipelineInfo.fs,  &m_pipelineState.compPipelineInfo.cs,
    };

    m_shaderSources.resize(NativeShaderStageCount);
    m_pipelineState.numStages = NativeShaderStageCount;
    m_pipelineState.stages = &m_shaderSources[0];
    // Shader section
    for (auto section : m_sections[SectionTypeShader]) {
      auto pShaderSection = reinterpret_cast<SectionShader *>(section);
      auto stage = pShaderSection->getShaderStage();

      pShaderSection->getSubState(m_pipelineState.stages[stage]);
    }

    // Shader info Section "XXInfo"
    for (auto section : m_sections[SectionTypeShaderInfo]) {
      auto pShaderInfoSection = reinterpret_cast<SectionShaderInfo *>(section);
      auto stage = pShaderInfoSection->getShaderStage();
      pShaderInfoSection->getSubState(*(shaderInfo[stage]));
    }
  }

  return &m_pipelineState;
}

// =====================================================================================================================
// Validates whether sections in this document are valid.
bool PipelineDocument::validate() {
  unsigned stageMask = 0;
  for (size_t i = 0; i < m_sectionList.size(); ++i) {
    auto sectionType = m_sectionList[i]->getSectionType();
    if (sectionType == SectionTypeShader) {
      auto stage = reinterpret_cast<SectionShader *>(m_sectionList[i])->getShaderStage();
      stageMask |= (1 << stage);
      if (i == m_sectionList.size()) {
        PARSE_ERROR(m_errorMsg, m_sectionList[i]->getLineNum(), "Fails to find related shader info section!\n");
        return false;
      } else {
        auto nextSectionType = m_sectionList[i + 1]->getSectionType();
        if ((nextSectionType != SectionTypeShaderInfo) ||
            (reinterpret_cast<SectionShaderInfo *>(m_sectionList[i + 1])->getShaderStage() != stage)) {
          PARSE_ERROR(m_errorMsg, m_sectionList[i + 1]->getLineNum(),
                      "Unexpected section type. Shader source and shader info must be in pair!\n");
          return false;
        }
      }
    }
  }
  const unsigned graphicsStageMask =
      ((1 << SpvGenStageVertex) | (1 << SpvGenStageTessControl) | (1 << SpvGenStageTessEvaluation) |
       (1 << SpvGenStageGeometry) | (1 << SpvGenStageFragment));
  const unsigned computeStageMask = (1 << SpvGenStageCompute);

  if (((stageMask & graphicsStageMask) && (stageMask & computeStageMask))
  ) {
    PARSE_ERROR(m_errorMsg, 0, "Stage Conflict! Different pipeline stage can't in same pipeline file.\n");
    return false;
  }

  if (stageMask & graphicsStageMask) {
    if (m_sections[SectionTypeComputeState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeComputeState][0]->getLineNum(),
                  "Section ComputePipelineState conflict with graphic shader stages\n");
      return false;
    }
  }

  if (stageMask & computeStageMask) {
    if (m_sections[SectionTypeGraphicsState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeGraphicsState][0]->getLineNum(),
                  "Section GraphicsPipelineState conflict with compute shader stages\n");
      return false;
    }
  }

  return true;
}

// =====================================================================================================================
// Creates a section object according to section name
//
// @param sectionName : Section name
Section *PipelineDocument::createSection(const char *sectionName) {
  auto it = Section::m_sectionInfo.find(sectionName);

  VFX_ASSERT(it->second.type != SectionTypeUnset);
  Section *section = nullptr;
  switch (it->second.type) {
  case SectionTypeGraphicsState:
    section = new SectionGraphicsState();
    break;
  case SectionTypeComputeState:
    section = new SectionComputeState();
    break;
  case SectionTypeVertexInputState:
    section = new SectionVertexInput();
    break;
  case SectionTypeShaderInfo:
    section = new SectionShaderInfo(it->second);
    break;
  default:
    section = Document::createSection(sectionName);
    break;
  }

  return section;
}

// =====================================================================================================================
// Gets the pointer of sub section according to member name
//
// @param lineNum : Line No.
// @param memberName : Member name
// @param memberType : Member type
// @param isWriteAccess : Whether the sub section will be written
// @param arrayIndex : Array index
// @param [out] ptrOut : Pointer of sub section
// @param [out] errorMsg : Error message
bool PipelineDocument::getPtrOfSubSection(Section *section, unsigned lineNum, const char *memberName,
                                          MemberType memberType, bool isWriteAccess, unsigned arrayIndex,
                                          Section **ptrOut, std::string *errorMsg) {
  bool result = false;

  switch (memberType) {
    CASE_SUBSECTION(MemberTypeResourceMappingNode, SectionResourceMappingNode)
    CASE_SUBSECTION(MemberTypeDescriptorRangeValue, SectionDescriptorRangeValueItem)
    CASE_SUBSECTION(MemberTypePipelineOption, SectionPipelineOption)
    CASE_SUBSECTION(MemberTypeShaderOption, SectionShaderOption)
    CASE_SUBSECTION(MemberTypeNggState, SectionNggState)
    CASE_SUBSECTION(MemberTypeExtendedRobustness, SectionExtendedRobustness)
  default:
    result = Document::getPtrOfSubSection(section, lineNum, memberName, memberType, isWriteAccess, arrayIndex, ptrOut,
                                          errorMsg);
    break;
  }

  return result;
}

// =====================================================================================================================
// Gets pipeline document from document handle
//
// NOTE: The document contents are not accessable after call vfxCloseDoc
//
// @param doc : Document handle
// @param [out] pipelineState : Pointer of struct VfxPipelineState
void VFXAPI vfxGetPipelineDoc(void *doc, VfxPipelineStatePtr *pipelineState) {
  *pipelineState = reinterpret_cast<PipelineDocument *>(doc)->getDocument();
}

} // namespace Vfx

#endif
