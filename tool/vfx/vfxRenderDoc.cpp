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
* @brief Contains implementation of class RenderDocument
***********************************************************************************************************************
*/
#include "vfx.h"
#include "vfxSection.h"

#if VFX_SUPPORT_RENDER_DOCOUMENT
#include "vfxRenderDoc.h"
#include "vfxRenderSection.h"

namespace Vfx {

// =====================================================================================================================
// Gets max section count for RenderDocument
unsigned RenderDocument::getMaxSectionCount(SectionType type) {
  unsigned maxSectionCount = 0;
  switch (type) {
  case SectionTypeVersion:
    maxSectionCount = 1;
    break;
  case SectionTypeCompileLog:
    maxSectionCount = 1;
    break;
  case SectionTypeResult:
    maxSectionCount = 1;
    break;
  case SectionTypeVertexState:
    maxSectionCount = 1;
    break;
  case SectionTypeDrawState:
    maxSectionCount = 1;
    break;
  case SectionTypeShader:
    maxSectionCount = ShaderStage::ShaderStageCount;
    break;
  case SectionTypeBufferView:
    maxSectionCount = Vfx::MaxRenderSectionCount;
    break;
  case SectionTypeImageView:
    maxSectionCount = Vfx::MaxRenderSectionCount;
    break;
  case SectionTypeSampler:
    maxSectionCount = Vfx::MaxRenderSectionCount;
    break;
  default:
    break;
  }
  return maxSectionCount;
}

// =====================================================================================================================
// Gets RenderDocument content
VfxRenderStatePtr RenderDocument::getDocument() {
  // Section "Result"
  if (m_sections[SectionTypeResult].size() > 0)
    reinterpret_cast<SectionResult *>(m_sections[SectionTypeResult][0])->getSubState(m_renderState.result);

  // Section "BufferView"s
  m_renderState.numBufferView = static_cast<unsigned>(m_sections[SectionTypeBufferView].size());
  for (unsigned i = 0; i < m_renderState.numBufferView; ++i) {
    reinterpret_cast<SectionBufferView *>(m_sections[SectionTypeBufferView][i])
        ->getSubState(m_renderState.bufferView[i]);
  }

  // Section "VertexState"
  if (m_sections[SectionTypeVertexState].size() > 0) {
    reinterpret_cast<SectionVertexState *>(m_sections[SectionTypeVertexState][0])
        ->getSubState(m_renderState.vertexState);
  }

  // Section "DrawState"
  if (m_sections[SectionTypeDrawState].size() > 0) {
    reinterpret_cast<SectionDrawState *>(m_sections[SectionTypeDrawState][0])->getSubState(m_renderState.drawState);
  } else
    SectionDrawState::initDrawState(m_renderState.drawState);

  // Section "ImageView"s
  m_renderState.numImageView = static_cast<unsigned>(m_sections[SectionTypeImageView].size());
  for (unsigned i = 0; i < m_renderState.numImageView; ++i) {
    reinterpret_cast<SectionImageView *>(m_sections[SectionTypeImageView][i])->getSubState(m_renderState.imageView[i]);
  }

  // Section "Sampler"s
  m_renderState.numSampler = static_cast<unsigned>(m_sections[SectionTypeSampler].size());
  for (unsigned i = 0; i < m_renderState.numSampler; ++i) {
    reinterpret_cast<SectionSampler *>(m_sections[SectionTypeSampler][i])->getSubState(m_renderState.sampler[i]);
  }

  // Shader sections
  for (auto &section : m_sections[SectionTypeShader]) {
    auto shaderSection = reinterpret_cast<SectionShader *>(&section);
    auto shaderStage = shaderSection->getShaderStage();
    shaderSection->getSubState(m_renderState.stages[shaderStage]);
  }

  return &m_renderState;
}

// =====================================================================================================================
// Creates a section object according to section name
//
// @param sectionName : Section name
Section *RenderDocument::createSection(const char *sectionName) {
  auto it = Section::m_sectionInfo.find(sectionName);

  VFX_ASSERT(it->second.type != SectionTypeUnset);

  Section *section = nullptr;
  switch (it->second.type) {
  case SectionTypeResult:
    section = new SectionResult();
    break;
  case SectionTypeBufferView:
    section = new SectionBufferView();
    break;
  case SectionTypeVertexState:
    section = new SectionVertexState();
    break;
  case SectionTypeDrawState:
    section = new SectionDrawState();
    break;
  case SectionTypeImageView:
    section = new SectionImageView();
    break;
  case SectionTypeSampler:
    section = new SectionSampler();
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
bool RenderDocument::getPtrOfSubSection(Section *section, unsigned lineNum, const char *memberName,
                                        MemberType memberType, bool isWriteAccess, unsigned arrayIndex,
                                        Section **ptrOut, std::string *errorMsg) {
  bool result = false;

  switch (memberType) {
    CASE_SUBSECTION(MemberTypeResultItem, SectionResultItem)
    CASE_SUBSECTION(MemberTypeVertexBufferBindingItem, SectionVertexBufferBinding)
    CASE_SUBSECTION(MemberTypeVertexAttributeItem, SectionVertexAttribute)
    CASE_SUBSECTION(MemberTypePushConstRange, SectionPushConstRange)
  default:
    result = Document::getPtrOfSubSection(section, lineNum, memberName, memberType, isWriteAccess, arrayIndex, ptrOut,
                                          errorMsg);
    break;
  }

  return result;
}

// =====================================================================================================================
// Gets render document from document handle
//
// NOTE: The document contents are not accessible after call vfxCloseDoc
//
// @param doc : Document handle
// @param [out] renderState : Pointer of struct VfxRenderState
void VFXAPI vfxGetRenderDoc(void *doc, VfxRenderStatePtr *renderState) {
  *renderState = reinterpret_cast<RenderDocument *>(doc)->getDocument();
}

} // namespace Vfx

#endif
