/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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
  case SectionTypeRayTracingState:
    maxSectionCount = 1;
    break;
  case SectionTypeRayTracingLibrarySummary:
    maxSectionCount = UINT32_MAX;
    break;
  case SectionTypeVertexInputState:
    maxSectionCount = 1;
    break;
  case SectionTypeResourceMapping:
    maxSectionCount = 1;
    break;
  case SectionTypeGraphicsLibrary:
    maxSectionCount = 1;
  case SectionTypeShader:
    maxSectionCount = UINT32_MAX;
    break;
  case SectionTypeShaderInfo:
    maxSectionCount = UINT32_MAX;
    break;
  case SectionTypFsOutput:
    maxSectionCount = 1;
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

  // Section "GraphicsLibrary"
  if (m_sections[SectionTypeGraphicsLibrary].size() > 0) {
    m_pipelineState.pipelineType = VfxPipelineTypeGraphicsLibrary;
    reinterpret_cast<SectionGraphicsLibrary *>(m_sections[SectionTypeGraphicsLibrary][0])
        ->getSubState(m_fileName, m_pipelineState.graphicsLibFileName);

    // If a pipeline contains this section, we will compile these libraries separately.
    return &m_pipelineState;
  }

  // Section "GraphicsPipelineState"
  if (m_sections[SectionTypeGraphicsState].size() > 0) {
    m_pipelineState.pipelineType = VfxPipelineTypeGraphics;
    reinterpret_cast<SectionGraphicsState *>(m_sections[SectionTypeGraphicsState][0])
        ->getSubState(m_fileName, m_pipelineState.gfxPipelineInfo, &m_errorMsg);
  }
  // Section "ComputePipelineState"
  if (m_sections[SectionTypeComputeState].size() > 0) {
    m_pipelineState.pipelineType = VfxPipelineTypeCompute;
    reinterpret_cast<SectionComputeState *>(m_sections[SectionTypeComputeState][0])
        ->getSubState(m_fileName, m_pipelineState.compPipelineInfo, &m_errorMsg);
  }

  // Section "RayTracingPipelineState"
  if (m_sections[SectionTypeRayTracingState].size() > 0) {
    m_pipelineState.pipelineType = VfxPipelineTypeRayTracing;
    reinterpret_cast<SectionRayTracingState *>(m_sections[SectionTypeRayTracingState][0])
        ->getSubState(m_fileName, m_pipelineState.rayPipelineInfo, &m_errorMsg);
  }

  // Section "VertexInputState"
  if (m_sections[SectionTypeVertexInputState].size() > 0) {
    reinterpret_cast<SectionVertexInput *>(m_sections[SectionTypeVertexInputState][0])->getSubState(m_vertexInputState);
    m_pipelineState.gfxPipelineInfo.pVertexInput = &m_vertexInputState;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 71
    reinterpret_cast<SectionVertexInput *>(m_sections[SectionTypeVertexInputState][0])
        ->getvbAddressLowBits(m_pipelineState.gfxPipelineInfo.vbAddressLowBits);
#else
    reinterpret_cast<SectionVertexInput *>(m_sections[SectionTypeVertexInputState][0])
        ->getvbAddressLowBits(m_pipelineState.gfxPipelineInfo.glState.vbAddressLowBits);
#endif
  }

  if (m_pipelineState.pipelineType == VfxPipelineTypeGraphics ||
      m_pipelineState.pipelineType == VfxPipelineTypeCompute) {
    PipelineShaderInfo *shaderInfo[NativeShaderStageCount] = {
        &m_pipelineState.gfxPipelineInfo.task, &m_pipelineState.gfxPipelineInfo.vs,
        &m_pipelineState.gfxPipelineInfo.tcs,  &m_pipelineState.gfxPipelineInfo.tes,
        &m_pipelineState.gfxPipelineInfo.gs,   &m_pipelineState.gfxPipelineInfo.mesh,
        &m_pipelineState.gfxPipelineInfo.fs,   &m_pipelineState.compPipelineInfo.cs,
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
  } else if (m_pipelineState.pipelineType == VfxPipelineTypeRayTracing) {
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

    for (Section *section : m_sections[SectionTypeRayTracingLibrarySummary]) {
      auto *summarySection = static_cast<SectionRayTracingLibrarySummary *>(section);
      m_librarySummaries.push_back(summarySection->getSubState());
    }

    m_pipelineState.rayPipelineInfo.libraryCount = m_librarySummaries.size();
    m_pipelineState.rayPipelineInfo.pLibrarySummaries = m_librarySummaries.data();
  } else {
    VFX_NEVER_CALLED();
  }

  ResourceMappingData *resourceMapping = nullptr;
  switch (m_pipelineState.pipelineType) {
  case VfxPipelineTypeGraphics:
    resourceMapping = &m_pipelineState.gfxPipelineInfo.resourceMapping;
    break;
  case VfxPipelineTypeCompute:
    resourceMapping = &m_pipelineState.compPipelineInfo.resourceMapping;
    break;
  case VfxPipelineTypeRayTracing:
    resourceMapping = &m_pipelineState.rayPipelineInfo.resourceMapping;
    break;
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

  if (m_sections[SectionTypFsOutput].size() > 0) {
    auto section = reinterpret_cast<SectionFsOutput *>(m_sections[SectionTypFsOutput][0]);
    section->getSubState(m_pipelineState.fsOutputs);
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

  const unsigned graphicsStageMask = ShaderStageBit::ShaderStageAllGraphicsBit;
  const unsigned computeStageMask = ShaderStageBit::ShaderStageComputeBit;
  const unsigned rayTracingStageMask = ShaderStageAllRayTracingBit;

  if (((stageMask & graphicsStageMask) && (stageMask & computeStageMask)) ||
      ((stageMask & graphicsStageMask) && (stageMask & rayTracingStageMask)) ||
      ((stageMask & computeStageMask) && (stageMask & rayTracingStageMask))) {
    PARSE_ERROR(m_errorMsg, 0, "Stage Conflict! Different pipeline stage can't in same pipeline file.\n");
    return false;
  }

  if (stageMask & graphicsStageMask) {
    if (m_sections[SectionTypeComputeState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeComputeState][0]->getLineNum(),
                  "Section ComputePipelineState conflict with graphic shader stages\n");
      return false;
    }

    if (m_sections[SectionTypeRayTracingState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeRayTracingState][0]->getLineNum(),
                  "Section RayTracingPipelineState conflict with graphic shader stages\n");
      return false;
    }
  }

  if (stageMask & computeStageMask) {
    if (m_sections[SectionTypeGraphicsState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeGraphicsState][0]->getLineNum(),
                  "Section GraphicsPipelineState conflict with compute shader stages\n");
      return false;
    }

    if (m_sections[SectionTypeRayTracingState].size() != 0) {
      PARSE_ERROR(m_errorMsg, m_sections[SectionTypeRayTracingState][0]->getLineNum(),
                  "Section RayTracingPipelineState conflict with compute shader stages\n");
      return false;
    }
  }

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
  case SectionTypeRayTracingState:
    section = new SectionRayTracingState();
    break;
  case SectionTypeRtState:
    section = new SectionRtState();
    break;
  case SectionTypeRayTracingLibrarySummary:
    section = new SectionRayTracingLibrarySummary;
    break;
  case SectionTypeVertexInputState:
    section = new SectionVertexInput();
    break;
  case SectionTypeShaderInfo:
    section = new SectionShaderInfo(it->second);
    break;
  case SectionTypeResourceMapping:
    section = new SectionResourceMapping();
    break;
  case SectionTypeGraphicsLibrary:
    section = new SectionGraphicsLibrary();
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
    CASE_SUBSECTION(MemberTypeUniformConstantMap, SectionUniformConstantMap)
    CASE_SUBSECTION(MemberTypeUniformConstantMapEntry, SectionUniformConstantMapEntry)
    CASE_SUBSECTION(MemberTypeXfbOutInfo, SectionXfbOutInfo)
    CASE_SUBSECTION(MemberTypeShaderGroup, SectionShaderGroup)
    CASE_SUBSECTION(MemberTypeRtState, SectionRtState)
    CASE_SUBSECTION(MemberTypeRayTracingShaderExportConfig, SectionRayTracingShaderExportConfig)
    CASE_SUBSECTION(MemberTypeIndirectCalleeSavedRegs, SectionIndirectCalleeSavedRegs)
    CASE_SUBSECTION(MemberTypeGpurtOption, SectionGpurtOption)
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
    CASE_SUBSECTION(MemberTypeGpurtFuncTable, SectionGpurtFuncTable)
#endif
    CASE_SUBSECTION(MemberTypeExtendedRobustness, SectionExtendedRobustness)
    CASE_SUBSECTION(MemberTypeAdvancedBlendInfo, SectionAdvancedBlendInfo)
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 73
    CASE_SUBSECTION(MemberTypeGlState, SectionGlState)
#endif
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
      (void)result;
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
