/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#if VKI_RAY_TRACING
  case SectionTypeRayTracingState:
    maxSectionCount = 1;
    break;
#endif
  case SectionTypeVertexInputState:
    maxSectionCount = 1;
    break;
  case SectionTypeResourceMapping:
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
// Checks whether the input version is supported.
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
// Gets PipelineDocument content
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
    gfxPipelineInfo->rsState.provokingVertexMode = graphicState.provokingVertexMode;
    gfxPipelineInfo->iaState.patchControlPoints = graphicState.patchControlPoints;
    gfxPipelineInfo->iaState.deviceIndex = graphicState.deviceIndex;
    gfxPipelineInfo->iaState.disableVertexReuse = graphicState.disableVertexReuse != 0;
    gfxPipelineInfo->iaState.switchWinding = graphicState.switchWinding != 0;
    gfxPipelineInfo->iaState.enableMultiView = graphicState.enableMultiView != 0;
    gfxPipelineInfo->vpState.depthClipEnable = graphicState.depthClipEnable != 0;
    gfxPipelineInfo->rsState.rasterizerDiscardEnable = graphicState.rasterizerDiscardEnable != 0;
    gfxPipelineInfo->rsState.perSampleShading = graphicState.perSampleShading != 0;
    gfxPipelineInfo->rsState.numSamples = graphicState.numSamples;
    gfxPipelineInfo->rsState.pixelShaderSamples = graphicState.pixelShaderSamples;
    gfxPipelineInfo->rsState.samplePatternIdx = graphicState.samplePatternIdx;
    gfxPipelineInfo->rsState.usrClipPlaneMask = static_cast<uint8_t>(graphicState.usrClipPlaneMask);

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
    gfxPipelineInfo->dynamicVertexStride = graphicState.dynamicVertexStride;
    gfxPipelineInfo->enableUberFetchShader = graphicState.enableUberFetchShader;
    gfxPipelineInfo->enableEarlyCompile = graphicState.enableEarlyCompile;
#if VKI_RAY_TRACING
    gfxPipelineInfo->shaderLibrary = graphicState.shaderLibrary;
    gfxPipelineInfo->rtState = graphicState.rtState;
#endif
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
#if VKI_RAY_TRACING
    computePipelineInfo->shaderLibrary = computeState.shaderLibrary;
    computePipelineInfo->rtState = computeState.rtState;
#endif
  }

#if VKI_RAY_TRACING
  // Section "RayTracingPipelineState"
  if (m_sections[SectionTypeRayTracingState].size() > 0) {
    RayTracingPipelineState rayTracingState;
    m_pipelineState.pipelineType = VfxPipelineTypeRayTracing;
    reinterpret_cast<SectionRayTracingState *>(m_sections[SectionTypeRayTracingState][0])
        ->getSubState(m_fileName, rayTracingState, &m_errorMsg);
    auto rayTracingPipelineInfo = &m_pipelineState.rayPipelineInfo;
    rayTracingPipelineInfo->deviceIndex = rayTracingState.deviceIndex;
    rayTracingPipelineInfo->options = rayTracingState.options;
    rayTracingPipelineInfo->shaderGroupCount = rayTracingState.shaderGroupCount;
    rayTracingPipelineInfo->pShaderGroups = rayTracingState.pShaderGroups;
    rayTracingPipelineInfo->shaderTraceRay = rayTracingState.shaderTraceRay;
    rayTracingPipelineInfo->maxRecursionDepth = rayTracingState.maxRecursionDepth;
    rayTracingPipelineInfo->indirectStageMask = rayTracingState.indirectStageMask;
    rayTracingPipelineInfo->rtState = rayTracingState.rtState;
    rayTracingPipelineInfo->payloadSizeMaxInLib = rayTracingState.payloadSizeMaxInLib;
    rayTracingPipelineInfo->attributeSizeMaxInLib = rayTracingState.attributeSizeMaxInLib;
    rayTracingPipelineInfo->hasPipelineLibrary = rayTracingState.hasPipelineLibrary;
    rayTracingPipelineInfo->pipelineLibStageMask = rayTracingState.pipelineLibStageMask;
  }
#endif

  // Section "VertexInputState"
  if (m_sections[SectionTypeVertexInputState].size() > 0) {
    reinterpret_cast<SectionVertexInput *>(m_sections[SectionTypeVertexInputState][0])->getSubState(m_vertexInputState);
    m_pipelineState.gfxPipelineInfo.pVertexInput = &m_vertexInputState;
  }
  // clang-format off
  if (m_pipelineState.pipelineType == VfxPipelineTypeGraphics ||
      m_pipelineState.pipelineType == VfxPipelineTypeCompute) {
    PipelineShaderInfo *shaderInfo[NativeShaderStageCount] = {
      &m_pipelineState.gfxPipelineInfo.vs,
      &m_pipelineState.gfxPipelineInfo.tcs,
      &m_pipelineState.gfxPipelineInfo.tes,
      &m_pipelineState.gfxPipelineInfo.gs,
      &m_pipelineState.gfxPipelineInfo.fs,
      &m_pipelineState.compPipelineInfo.cs,
    };

    m_shaderSources.resize(NativeShaderStageCount);
    m_pipelineState.numStages = NativeShaderStageCount;
    m_pipelineState.stages = &m_shaderSources[0];
    // Shader section
    for (auto section : m_sections[SectionTypeShader]) {
      auto shaderSection = reinterpret_cast<SectionShader *>(section);
      auto stage = shaderSection->getShaderStage();

      // In case the .pipe file did not contain a ComputePipelineState
      if (stage == Vkgc::ShaderStageCompute)
        m_pipelineState.pipelineType = VfxPipelineTypeCompute;

      shaderSection->getSubState(m_pipelineState.stages[stage]);
    }

    // Shader info Section "XXInfo"
    for (auto section : m_sections[SectionTypeShaderInfo]) {
      auto shaderInfoSection = reinterpret_cast<SectionShaderInfo *>(section);
      auto stage = shaderInfoSection->getShaderStage();
      shaderInfoSection->getSubState(*(shaderInfo[stage]));
      shaderInfoSection->getSubState(m_resourceMappingNodes);
      shaderInfoSection->getSubState(m_descriptorRangeValues);
    }
  }
#if VKI_RAY_TRACING
  else if (m_pipelineState.pipelineType == VfxPipelineTypeRayTracing) {
    m_pipelineState.numStages = static_cast<unsigned>(m_sections[SectionTypeShader].size());
    m_shaderSources.resize(m_pipelineState.numStages);
    m_shaderInfos.resize(m_pipelineState.numStages);
    m_pipelineState.stages = &m_shaderSources[0];
    m_pipelineState.rayPipelineInfo.shaderCount = m_pipelineState.numStages;
    m_pipelineState.rayPipelineInfo.pShaders = &m_shaderInfos[0];

    unsigned stageIndex = 0;
    std::map<unsigned, std::pair<SectionShader *, SectionShaderInfo *>> shaderSections;
    VFX_ASSERT(m_sections[SectionTypeShader].size() == m_sections[SectionTypeShaderInfo].size());

    for (size_t i = 0; i < m_sections[SectionTypeShader].size(); ++i) {
      auto shaderSection = reinterpret_cast<SectionShader *>(m_sections[SectionTypeShader][i]);
      auto shaderInfoSection = reinterpret_cast<SectionShaderInfo *>(m_sections[SectionTypeShaderInfo][i]);
      VFX_ASSERT(shaderSection->getShaderStage() == shaderInfoSection->getShaderStage());
      auto stage = shaderSection->getShaderStage();

      shaderSections[m_sections[SectionTypeShader][i]->getLineNum()] =
          std::pair<SectionShader *, SectionShaderInfo *>(shaderSection, shaderInfoSection);
    }

    for (auto mapIt : shaderSections) {
      mapIt.second.first->getSubState(m_pipelineState.stages[stageIndex]);
      // Shader info Section "XXInfo"
      mapIt.second.second->getSubState(m_shaderInfos[stageIndex]);
      mapIt.second.second->getSubState(m_resourceMappingNodes);
      mapIt.second.second->getSubState(m_descriptorRangeValues);
      stageIndex++;
    }
  }
#endif
  else {
    VFX_NEVER_CALLED();
  }
  // clang-format on
  ResourceMappingData *resourceMapping = nullptr;
  switch (m_pipelineState.pipelineType) {
  case VfxPipelineTypeGraphics:
    resourceMapping = &m_pipelineState.gfxPipelineInfo.resourceMapping;
    break;
  case VfxPipelineTypeCompute:
    resourceMapping = &m_pipelineState.compPipelineInfo.resourceMapping;
    break;
#if VKI_RAY_TRACING
  case VfxPipelineTypeRayTracing:
    resourceMapping = &m_pipelineState.rayPipelineInfo.resourceMapping;
    break;
#endif
  default:
    VFX_NEVER_CALLED();
    break;
  }

  // Section "ResourceMapping"
  if (m_sections[SectionTypeResourceMapping].size() > 0) {
    auto section = reinterpret_cast<SectionResourceMapping *>(m_sections[SectionTypeResourceMapping][0]);
    section->getSubState(*resourceMapping);
  } else {
    // If no ResourceMapping section was found, this must be an older .pipe file where the resource mapping
    // was embedded in the pipeline shader infos.
    DeduplicateResourceMappingData(resourceMapping);
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
      }
      auto nextSectionType = m_sectionList[i + 1]->getSectionType();
      if ((nextSectionType != SectionTypeShaderInfo) ||
          (reinterpret_cast<SectionShaderInfo *>(m_sectionList[i + 1])->getShaderStage() != stage)) {
        PARSE_ERROR(m_errorMsg, m_sectionList[i + 1]->getLineNum(),
                    "Unexpected section type. Shader source and shader info must be in pair!\n");
        return false;
      }
    }
  }

  if (stageMask == 0) {
    PARSE_ERROR(m_errorMsg, 0, "No Shader source section in pipeline!\n");
    return false;
  }

  const unsigned graphicsStageMask = ShaderStageBit::ShaderStageAllGraphicsBit;
  const unsigned computeStageMask = ShaderStageBit::ShaderStageComputeBit;
#if VKI_RAY_TRACING
  const unsigned rayTracingStageMask = ShaderStageAllRayTracingBit;
#endif

  if (((stageMask & graphicsStageMask) && (stageMask & computeStageMask))
#if VKI_RAY_TRACING
      || ((stageMask & graphicsStageMask) && (stageMask & rayTracingStageMask)) ||
      ((stageMask & computeStageMask) && (stageMask & rayTracingStageMask))
#endif
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
#if VKI_RAY_TRACING
    if (m_sections[SectionTypeRayTracingState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeRayTracingState][0]->getLineNum(),
                  "Section RayTracingPipelineState conflict with graphic shader stages\n");
      return false;
    }
#endif
  }

  if (stageMask & computeStageMask) {
    if (m_sections[SectionTypeGraphicsState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeGraphicsState][0]->getLineNum(),
                  "Section GraphicsPipelineState conflict with compute shader stages\n");
      return false;
    }
#if VKI_RAY_TRACING
    if (m_sections[SectionTypeRayTracingState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeRayTracingState][0]->getLineNum(),
                  "Section RayTracingPipelineState conflict with compute shader stages\n");
      return false;
    }
#endif
  }

#if VKI_RAY_TRACING
  if (stageMask & rayTracingStageMask) {
    if (m_sections[SectionTypeComputeState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeComputeState][0]->getLineNum(),
                  "Section ComputePipelineState conflict with ray tracing shader stages\n");
      return false;
    }

    if (m_sections[SectionTypeGraphicsState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeGraphicsState][0]->getLineNum(),
                  "Section GraphicsPipelineState conflict with ray tracing shader stages\n");
      return false;
    }
  }
#endif

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
#if VKI_RAY_TRACING
  case SectionTypeRayTracingState:
    section = new SectionRayTracingState();
    break;
  case SectionTypeRtState:
    section = new SectionRtState();
    break;
#endif
  case SectionTypeVertexInputState:
    section = new SectionVertexInput();
    break;
  case SectionTypeShaderInfo:
    section = new SectionShaderInfo(it->second);
    break;
  case SectionTypeResourceMapping:
    section = new SectionResourceMapping();
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
#if VKI_RAY_TRACING
    CASE_SUBSECTION(MemberTypeShaderGroup, SectionShaderGroup)
    CASE_SUBSECTION(MemberTypeRtState, SectionRtState)
    CASE_SUBSECTION(MemberTypeRayTracingShaderExportConfig, SectionRayTracingShaderExportConfig)
    CASE_SUBSECTION(MemberTypeIndirectCalleeSavedRegs, SectionIndirectCalleeSavedRegs)
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
    CASE_SUBSECTION(MemberTypeGpurtFuncTable, SectionGpurtFuncTable)
#endif
#endif
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
// NOTE: The document contents are not accessible after call vfxCloseDoc
//
// @param doc : Document handle
// @param [out] pipelineState : Pointer of struct VfxPipelineState
void VFXAPI vfxGetPipelineDoc(void *doc, VfxPipelineStatePtr *pipelineState) {
  *pipelineState = reinterpret_cast<PipelineDocument *>(doc)->getDocument();
}

// =====================================================================================================================
// Deduplicates any resource mapping data extracted out of shader info sections and populates a ResourceMappingData
// struct to be used at the pipeline level. Used for backward compatibility with Version 1 .pipe files.
//
// @param [out] resourceMapping : Pointer of struct Vkgc::ResourceMappingData
void PipelineDocument::DeduplicateResourceMappingData(Vkgc::ResourceMappingData *resourceMapping) {
  struct RootNodeWrapper {
    Vkgc::ResourceMappingRootNode rootNode;
    std::map<unsigned, Vkgc::ResourceMappingNode> resourceNodes;
  };

  std::map<unsigned, RootNodeWrapper> rootNodeMap;
  std::map<unsigned long long, Vkgc::StaticDescriptorValue> staticMap;
  size_t maxSubNodeCount = 0;

  for (const auto &userDataNode : m_resourceMappingNodes) {
    auto iter = rootNodeMap.find(userDataNode.node.offsetInDwords);
    if (iter == rootNodeMap.end()) {
      auto result = rootNodeMap.insert({userDataNode.node.offsetInDwords, {userDataNode}});
      iter = result.first;
      VFX_ASSERT(result.second);
    } else {
      iter->second.rootNode.visibility |= userDataNode.visibility;
    }

    if (iter->second.rootNode.node.type == Vkgc::ResourceMappingNodeType::DescriptorTableVaPtr) {
      for (unsigned k = 0; k < iter->second.rootNode.node.tablePtr.nodeCount; ++k) {
        const auto &resourceNode = iter->second.rootNode.node.tablePtr.pNext[k];
        iter->second.resourceNodes.insert({resourceNode.offsetInDwords, resourceNode});
        maxSubNodeCount++;
      }
    }
  }

  for (const auto &descriptorRangeValue : m_descriptorRangeValues) {
    unsigned long long key = static_cast<unsigned long long>(descriptorRangeValue.set) |
                             (static_cast<unsigned long long>(descriptorRangeValue.binding) << 32);

    auto iter = staticMap.find(key);
    if (iter == staticMap.end()) {
      auto result = staticMap.insert({key, descriptorRangeValue});
      VFX_ASSERT(result.second);
    } else {
      iter->second.visibility |= descriptorRangeValue.visibility;
    }
  }

  m_resourceMappingNodes.clear();
  m_descriptorRangeValues.clear();
  m_resourceMappingSubNodes.clear();

  m_resourceMappingNodes.reserve(rootNodeMap.size());
  m_descriptorRangeValues.reserve(rootNodeMap.size());
  m_resourceMappingSubNodes.reserve(maxSubNodeCount);

  for (auto &rootNodeIter : rootNodeMap) {
    auto &rootNode = rootNodeIter.second.rootNode;
    auto &subNodeMap = rootNodeIter.second.resourceNodes;

    if (subNodeMap.size() > 0) {
      size_t idxOffset = m_resourceMappingSubNodes.size();
      for (auto &subNode : subNodeMap)
        m_resourceMappingSubNodes.push_back(subNode.second);

      rootNode.node.tablePtr.pNext = &(m_resourceMappingSubNodes.data()[idxOffset]);
      rootNode.node.tablePtr.nodeCount = static_cast<uint32_t>(m_resourceMappingSubNodes.size() - idxOffset);
    }

    m_resourceMappingNodes.push_back(rootNode);
  }

  for (auto &staticIter : staticMap)
    m_descriptorRangeValues.push_back(staticIter.second);

  resourceMapping->pUserDataNodes = m_resourceMappingNodes.data();
  resourceMapping->userDataNodeCount = m_resourceMappingNodes.size();
  resourceMapping->pStaticDescriptorValues = m_descriptorRangeValues.data();
  resourceMapping->staticDescriptorValueCount = m_descriptorRangeValues.size();
}

} // namespace Vfx

#endif
