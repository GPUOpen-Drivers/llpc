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
#include "vfxPipelineDoc.h"
#if VFX_INSIDE_SPVGEN
#define SH_EXPORTING
#endif
#include "spvgen.h"

using namespace Vkgc;

namespace Vfx
{
// =====================================================================================================================
// Max section count for PipelineDocument
uint32_t PipelineDocument::m_MaxSectionCount[SectionTypeNameNum] =
{
    0,                     // SectionTypeUnset
    0,                     // SectionTypeResult
    0,                     // SectionTypeBufferView
    0,                     // SectionTypeVertexState
    0,                     // SectionTypeDrawState
    0,                     // SectionTypeImageView
    0,                     // SectionTypeSampler
    1,                     // SectionTypeVersion
    1,                     // SectionTypeGraphicsState,
    1,                     // SectionTypeComputeState,
    1,                     // SectionTypeVertexInputState,
    1,                     // SectionTypeVertexShaderInfo,
    1,                     // SectionTypeTessControlShaderInfo,
    1,                     // SectionTypeTessEvalShaderInfo,
    1,                     // SectionTypeGeometryShaderInfo,
    1,                     // SectionTypeFragmentShaderInfo,
    1,                     // SectionTypeComputeShaderInfo,
    1,                     // SectionTypeCompileLog
    1,                     // SectionTypeVertexShader
    1,                     // SectionTypeTessControlShader
    1,                     // SectionTypeTessEvalShader
    1,                     // SectionTypeGeometryShader
    1,                     // SectionTypeFragmentShader
    1,                     // SectionTypeComputeShader
};

// =====================================================================================================================
// Checks whether the input version is supportted.
bool PipelineDocument::CheckVersion(
    uint32_t ver)        // Version
{
    bool result = true;

    // Report error if input version higher than the version in header file
    if (ver > Version)
    {
        PARSE_ERROR(m_errorMsg, 0, "Unsupported version: %u (max version = %u)", ver, Version);
        result = false;
    }
    return result;
}

// =====================================================================================================================
// Gets PiplineDocument content
VfxPipelineStatePtr PipelineDocument::GetDocument()
{
    // Section "Version"
    m_pipelineState.version = Version;

    // Section "GraphicsPipelineState"
    if (m_sections[SectionTypeGraphicsState].size() > 0)
    {
        GraphicsPipelineState graphicState;
        m_pipelineState.pipelineType = VfxPipelineTypeGraphics;
        reinterpret_cast<SectionGraphicsState*>(m_sections[SectionTypeGraphicsState][0])->
            GetSubState(m_fileName, graphicState, &m_errorMsg);
        auto pGfxPipelineInfo = &m_pipelineState.gfxPipelineInfo;
        pGfxPipelineInfo->iaState.topology                = graphicState.topology;
        pGfxPipelineInfo->iaState.patchControlPoints      = graphicState.patchControlPoints;
        pGfxPipelineInfo->iaState.deviceIndex             = graphicState.deviceIndex;
        pGfxPipelineInfo->iaState.disableVertexReuse      = graphicState.disableVertexReuse ? true : false;
        pGfxPipelineInfo->iaState.switchWinding           = graphicState.switchWinding ? true : false;
        pGfxPipelineInfo->iaState.enableMultiView         = graphicState.enableMultiView ? true : false;
        pGfxPipelineInfo->vpState.depthClipEnable         = graphicState.depthClipEnable ? true : false;
        pGfxPipelineInfo->rsState.rasterizerDiscardEnable = graphicState.rasterizerDiscardEnable ? true : false;
        pGfxPipelineInfo->rsState.perSampleShading        = graphicState.perSampleShading ? true : false;
        pGfxPipelineInfo->rsState.numSamples              = graphicState.numSamples;
        pGfxPipelineInfo->rsState.samplePatternIdx        = graphicState.samplePatternIdx;
        pGfxPipelineInfo->rsState.usrClipPlaneMask        = static_cast<uint8_t>(graphicState.usrClipPlaneMask);
        pGfxPipelineInfo->rsState.polygonMode             = graphicState.polygonMode;
        pGfxPipelineInfo->rsState.cullMode                = graphicState.cullMode;
        pGfxPipelineInfo->rsState.frontFace               = graphicState.frontFace;
        pGfxPipelineInfo->rsState.depthBiasEnable         = graphicState.depthBiasEnable ? true : false;

        pGfxPipelineInfo->cbState.alphaToCoverageEnable   = graphicState.alphaToCoverageEnable ? true : false;
        pGfxPipelineInfo->cbState.dualSourceBlendEnable   = graphicState.dualSourceBlendEnable ? true : false;
        for (uint32_t i = 0; i < MaxColorTargets; ++i)
        {
            pGfxPipelineInfo->cbState.target[i].format = graphicState.colorBuffer[i].format;
            pGfxPipelineInfo->cbState.target[i].channelWriteMask =
                static_cast<uint8_t>(graphicState.colorBuffer[i].channelWriteMask);
            pGfxPipelineInfo->cbState.target[i].blendEnable = graphicState.colorBuffer[i].blendEnable ? true : false;
            pGfxPipelineInfo->cbState.target[i].blendSrcAlphaToColor =
                graphicState.colorBuffer[i].blendSrcAlphaToColor ? true : false;
        }

        pGfxPipelineInfo->options = graphicState.options;
        pGfxPipelineInfo->nggState = graphicState.nggState;
    }

    // Section "ComputePipelineState"
    if (m_sections[SectionTypeComputeState].size() > 0)
    {
        ComputePipelineState computeState;
        m_pipelineState.pipelineType = VfxPipelineTypeCompute;
        reinterpret_cast<SectionComputeState*>(m_sections[SectionTypeComputeState][0])->
            GetSubState(m_fileName, computeState, &m_errorMsg);
        auto pComputePipelineInfo = &m_pipelineState.compPipelineInfo;
        pComputePipelineInfo->deviceIndex = computeState.deviceIndex;
        pComputePipelineInfo->options     = computeState.options;
        pComputePipelineInfo->cs.entryStage = Vkgc::ShaderStageCompute;
    }

    // Section "VertexInputState"
    if (m_sections[SectionTypeVertexInputState].size() > 0)
    {
        reinterpret_cast<SectionVertexInput*>(m_sections[SectionTypeVertexInputState][0])->
            GetSubState(m_vertexInputState);
        m_pipelineState.gfxPipelineInfo.pVertexInput = &m_vertexInputState;
    }

    if (m_pipelineState.pipelineType == VfxPipelineTypeGraphics ||
        m_pipelineState.pipelineType == VfxPipelineTypeCompute)
    {
        PipelineShaderInfo* shaderInfo[NativeShaderStageCount] =
        {
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
        for (uint32_t i = 0; i < NativeShaderStageCount; ++i)
        {
            // shader section
            if (m_sections[SectionTypeVertexShader + i].size() > 0)
            {
                reinterpret_cast<SectionShader*>(m_sections[SectionTypeVertexShader + i][0])->
                    GetSubState(m_pipelineState.stages[i]);
            }

            // shader info Section "XXInfo"
            if (m_sections[SectionTypeVertexShaderInfo + i].size() > 0)
            {
                reinterpret_cast<SectionShaderInfo*>(m_sections[SectionTypeVertexShaderInfo + i][0])->
                    GetSubState(*(shaderInfo[i]));
                shaderInfo[i]->entryStage = m_pipelineState.stages[i].stage;
            }
        }
    }

    return &m_pipelineState;
}

// =====================================================================================================================
// Validates whether sections in this document are valid.
bool PipelineDocument::Validate()
{
    uint32_t stageMask = 0;
    for (size_t i = 0; i < m_sectionList.size(); ++i)
    {
        auto sectionType = m_sectionList[i]->GetSectionType();
        if ((sectionType >= SectionTypeVertexShader) &&
            (sectionType < (SectionTypeVertexShader + ShaderStageCount)))
        {
            auto stage = sectionType - SectionTypeVertexShader;
            stageMask |= (1 << stage);
            if (i == m_sectionList.size())
            {
                PARSE_ERROR(m_errorMsg, m_sectionList[i]->GetLineNum(), "Fails to find related shader info section!\n");
                return false;
            }
            else
            {
                auto nextSectionType = m_sectionList[i + 1]->GetSectionType();
                if (nextSectionType != (SectionTypeVertexShaderInfo + stage))
                {
                    PARSE_ERROR(m_errorMsg,
                                m_sectionList[i + 1]->GetLineNum(),
                                "Unexpected section type. Shader source and shader info must be in pair!\n");
                    return false;
                }
            }
        }
    }
    const uint32_t GraphicsStageMask = (
        (1 << SpvGenStageVertex) |
        (1 << SpvGenStageTessControl) |
        (1 << SpvGenStageTessEvaluation) |
        (1 << SpvGenStageGeometry) |
        (1 << SpvGenStageFragment)
        );
    const uint32_t ComputeStageMask = (1 << SpvGenStageCompute);

    if (((stageMask & GraphicsStageMask) && (stageMask & ComputeStageMask))
        )
    {
        PARSE_ERROR(m_errorMsg,
            0,
            "Stage Conflict! Different pipeline stage can't in same pipeline file.\n");
        return false;
    }

    if (stageMask & GraphicsStageMask)
    {
        if (m_sections[SectionTypeComputeState].size() != 0)
        {
            PARSE_ERROR(m_errorMsg,
                m_sections[SectionTypeComputeState][0]->GetLineNum(),
                "Section ComputePipelineState conflict with graphic shader stages\n");
            return false;
        }
    }

    if (stageMask & ComputeStageMask)
    {
        if (m_sections[SectionTypeGraphicsState].size() != 0)
        {
            PARSE_ERROR(m_errorMsg,
                m_sections[SectionTypeGraphicsState][0]->GetLineNum(),
                "Section GraphicsPipelineState conflict with compute shader stages\n");
            return false;
        }
    }

    return true;
}

}

