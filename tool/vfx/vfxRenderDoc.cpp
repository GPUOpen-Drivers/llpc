/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "vfxRenderDoc.h"

namespace Vfx
{
// =====================================================================================================================
// Max section count for RenderDocument
uint32_t RenderDocument::m_MaxSectionCount[SectionTypeNameNum] =
{
    0,                     // SectionTypeUnset
    1,                     // SectionTypeResult
    MaxSectionCount,       // SectionTypeBufferView
    1,                     // SectionTypeVertexState
    1,                     // SectionTypeDrawState
    MaxSectionCount,       // SectionTypeImageView
    MaxSectionCount,       // SectionTypeSampler
    1,                     // SectionTypeVersion
    0,                     // SectionTypeGraphicsState,
    0,                     // SectionTypeComputeState,
    0,                     // SectionTypeVertexInputState,
    0,                     // SectionTypeVertexShaderInfo,
    0,                     // SectionTypeTessControlShaderInfo,
    0,                     // SectionTypeTessEvalShaderInfo,
    0,                     // SectionTypeGeometryShaderInfo,
    0,                     // SectionTypeFragmentShaderInfo,
    0,                     // SectionTypeComputeShaderInfo,
    0,                     // SectionTypeCompileLog
    1,                     // SectionTypeVertexShader
    1,                     // SectionTypeTessControlShader
    1,                     // SectionTypeTessEvalShader
    1,                     // SectionTypeGeometryShader
    1,                     // SectionTypeFragmentShader
    1,                     // SectionTypeComputeShader
};

// =====================================================================================================================
// Gets RenderDocument content
VfxRenderStatePtr RenderDocument::GetDocument()
{
    // Section "Result"
    if (m_sections[SectionTypeResult].size() > 0)
    {
        reinterpret_cast<SectionResult*>(m_sections[SectionTypeResult][0])->GetSubState(m_renderState.result);
    }

    // Section "BufferView"s
    m_renderState.numBufferView = static_cast<uint32_t>(m_sections[SectionTypeBufferView].size());
    for (uint32_t i = 0; i < m_renderState.numBufferView; ++i)
    {
        reinterpret_cast<SectionBufferView*>(m_sections[SectionTypeBufferView][i])->
            GetSubState(m_renderState.bufferView[i]);
    }

    // Section "VertexState"
    if (m_sections[SectionTypeVertexState].size() > 0)
    {
        reinterpret_cast<SectionVertexState*>(m_sections[SectionTypeVertexState][0])->
            GetSubState(m_renderState.vertexState);
    }

    // Section "DrawState"
    if (m_sections[SectionTypeDrawState].size() > 0)
    {
        reinterpret_cast<SectionDrawState*>(m_sections[SectionTypeDrawState][0])->
            GetSubState(m_renderState.drawState);
    }
    else
    {
        SectionDrawState::InitDrawState(m_renderState.drawState);
    }

    // Section "ImageView"s
    m_renderState.numImageView = static_cast<uint32_t>(m_sections[SectionTypeImageView].size());
    for (uint32_t i = 0; i < m_renderState.numImageView; ++i)
    {
        reinterpret_cast<SectionImageView*>(m_sections[SectionTypeImageView][i])->
            GetSubState(m_renderState.imageView[i]);
    }

    // Section "Sampler"s
    m_renderState.numSampler = static_cast<uint32_t>(m_sections[SectionTypeSampler].size());
    for (uint32_t i = 0; i < m_renderState.numSampler; ++i)
    {
        reinterpret_cast<SectionSampler*>(m_sections[SectionTypeSampler][i])->
            GetSubState(m_renderState.sampler[i]);
    }

    // Shader sections
    for (uint32_t i = 0; i < ShaderStageCount; ++i)
    {
        if (m_sections[SectionTypeVertexShader + i].size() > 0)
        {
            reinterpret_cast<SectionShader*>(m_sections[SectionTypeVertexShader + i][0])->
                GetSubState(m_renderState.stages[i]);
        }
    }

    return &m_renderState;
}
}
