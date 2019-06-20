/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPipelineContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PipelineContext.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-pipeline-context"

#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/CommandLine.h"

#include "SPIRVInternal.h"
#include "llpcBuilder.h"
#include "llpcCompiler.h"
#include "llpcPipelineContext.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
PipelineContext::PipelineContext(
    GfxIpVersion           gfxIp,           // Graphics IP version info
    const GpuProperty*     pGpuProp,        // [in] GPU property
    const WorkaroundFlags* pGpuWorkarounds, // [in] GPU workarounds
    MetroHash::Hash*       pPipelineHash,   // [in] Pipeline hash code
    MetroHash::Hash*       pCacheHash)      // [in] Cache hash code
    :
    m_gfxIp(gfxIp),
    m_pipelineHash(*pPipelineHash),
    m_cacheHash(*pCacheHash),
    m_pGpuProperty(pGpuProp),
    m_pGpuWorkarounds(pGpuWorkarounds)
{

}

// =====================================================================================================================
PipelineContext::~PipelineContext()
{
}

// =====================================================================================================================
// Gets the name string of GPU target according to graphics IP version info.
const char* PipelineContext::GetGpuNameString() const
{
    struct GpuNameStringMap
    {
        GfxIpVersion gfxIp;
        const char*  pNameString;
    };

    static const GpuNameStringMap GpuNameMap[] =
    {   // Graphics IP  Target Name   Compatible Target Name
        { { 6, 0, 0 }, "tahiti"   },  // [6.0.0] gfx600, tahiti
        { { 6, 0, 1 }, "pitcairn" },  // [6.0.1] gfx601, pitcairn, verde, oland, hainan
        { { 7, 0, 0 }, "kaveri"  },   // [7.0.0] gfx700, kaveri
        { { 7, 0, 1 }, "hawaii"   },  // [7.0.1] gfx701, hawaii
        { { 7, 0, 2 }, "gfx702"   },  // [7.0.2] gfx702
        { { 7, 0, 3 }, "kabini"   },  // [7.0.3] gfx703, kabini, mullins
        { { 7, 0, 4 }, "bonaire"  },  // [7.0.4] gfx704, bonaire
        { { 8, 0, 0 }, "iceland"  },  // [8.0.0] gfx800, iceland
        { { 8, 0, 1 }, "carrizo"  },  // [8.0.1] gfx801, carrizo
        { { 8, 0, 2 }, "tonga"    },  // [8.0.2] gfx802, tonga
        { { 8, 0, 3 }, "fiji"     },  // [8.0.3] gfx803, fiji, polaris10, polaris11
        { { 8, 0, 4 }, "gfx804"   },  // [8.0.4] gfx804
        { { 8, 1, 0 }, "stoney"   },  // [8.1.0] gfx810, stoney
        { { 9, 0, 0 }, "gfx900"   },  // [9.0.0] gfx900
        { { 9, 0, 1 }, "gfx901"   },  // [9.0.1] gfx901
        { { 9, 0, 2 }, "gfx902"   },  // [9.0.2] gfx902
        { { 9, 0, 3 }, "gfx903"   },  // [9.0.3] gfx903
        { { 9, 0, 4 }, "gfx904"   },  // [9.0.4] gfx904, vega12
        { { 9, 0, 6 }, "gfx906"   },  // [9.0.6] gfx906, vega20
        { { 9, 0, 9 }, "gfx909"   },  // [9.0.9] gfx909, raven2
    };

    const GpuNameStringMap* pNameMap = nullptr;
    for (auto& nameMap : GpuNameMap)
    {
        if ((nameMap.gfxIp.major    == m_gfxIp.major) &&
            (nameMap.gfxIp.minor    == m_gfxIp.minor) &&
            (nameMap.gfxIp.stepping == m_gfxIp.stepping))
        {
            pNameMap = &nameMap;
            break;
        }
    }

    LLPC_ASSERT(pNameMap != nullptr);

    return (pNameMap != nullptr) ? pNameMap->pNameString : "";
}

// =====================================================================================================================
// Gets the name string of the abbreviation for GPU target according to graphics IP version info.
const char* PipelineContext::GetGpuNameAbbreviation() const
{
    const char* pNameAbbr = nullptr;
    switch (m_gfxIp.major)
    {
    case 6:
        pNameAbbr = "SI";
        break;
    case 7:
        pNameAbbr = "CI";
        break;
    case 8:
        pNameAbbr = "VI";
        break;
    case 9:
        pNameAbbr = "GFX9";
        break;
    default:
        pNameAbbr = "UNKNOWN";
        break;
    }

    return pNameAbbr;
}

// =====================================================================================================================
// Initializes resource usage of the specified shader stage.
void PipelineContext::InitShaderResourceUsage(
    ShaderStage shaderStage)    // Shader stage
{
    auto pResUsage = GetShaderResourceUsage(shaderStage);

    memset(&pResUsage->builtInUsage, 0, sizeof(pResUsage->builtInUsage));

    pResUsage->pushConstSizeInBytes = 0;
    pResUsage->resourceWrite = false;
    pResUsage->resourceRead = false;
    pResUsage->perShaderTable = false;
    pResUsage->globalConstant = false;

    pResUsage->numSgprsAvailable = m_pGpuProperty->maxSgprsAvailable;
    pResUsage->numVgprsAvailable = m_pGpuProperty->maxVgprsAvailable;

    pResUsage->inOutUsage.inputMapLocCount = 0;
    pResUsage->inOutUsage.outputMapLocCount = 0;
    memset(pResUsage->inOutUsage.gs.outLocCount, 0, sizeof(pResUsage->inOutUsage.gs.outLocCount));
    pResUsage->inOutUsage.perPatchInputMapLocCount = 0;
    pResUsage->inOutUsage.perPatchOutputMapLocCount = 0;

    pResUsage->inOutUsage.expCount = 0;

    memset(pResUsage->inOutUsage.xfbStrides, 0, sizeof(pResUsage->inOutUsage.xfbStrides));
    pResUsage->inOutUsage.enableXfb = false;

    memset(pResUsage->inOutUsage.streamXfbBuffers, 0, sizeof(pResUsage->inOutUsage.streamXfbBuffers));

    if (shaderStage == ShaderStageVertex)
    {
        // NOTE: For vertex shader, PAL expects base vertex and base instance in user data,
        // even if they are not used in shader.
        pResUsage->builtInUsage.vs.baseVertex = true;
        pResUsage->builtInUsage.vs.baseInstance = true;
    }
    else if (shaderStage == ShaderStageTessControl)
    {
        auto& calcFactor = pResUsage->inOutUsage.tcs.calcFactor;

        calcFactor.inVertexStride           = InvalidValue;
        calcFactor.outVertexStride          = InvalidValue;
        calcFactor.patchCountPerThreadGroup = InvalidValue;
        calcFactor.offChip.outPatchStart    = InvalidValue;
        calcFactor.offChip.patchConstStart  = InvalidValue;
        calcFactor.onChip.outPatchStart     = InvalidValue;
        calcFactor.onChip.patchConstStart   = InvalidValue;
        calcFactor.outPatchSize             = InvalidValue;
        calcFactor.patchConstSize           = InvalidValue;
    }
    else if (shaderStage == ShaderStageGeometry)
    {
        pResUsage->inOutUsage.gs.rasterStream        = 0;

        auto& calcFactor = pResUsage->inOutUsage.gs.calcFactor;
        memset(&calcFactor, 0, sizeof(calcFactor));
    }
    else if (shaderStage == ShaderStageFragment)
    {
        for (uint32_t i = 0; i < MaxColorTargets; ++i)
        {
            pResUsage->inOutUsage.fs.expFmts[i] = EXP_FORMAT_ZERO;
            pResUsage->inOutUsage.fs.outputTypes[i] = BasicType::Unknown;
        }

        pResUsage->inOutUsage.fs.cbShaderMask = 0;
        pResUsage->inOutUsage.fs.dummyExport = true;
    }
}

// =====================================================================================================================
// Initializes interface data of the specified shader stage.
void PipelineContext::InitShaderInterfaceData(
    ShaderStage shaderStage)  // Shader stage
{
    auto pIntfData = GetShaderInterfaceData(shaderStage);

    pIntfData->userDataCount = 0;
    memset(pIntfData->userDataMap, InterfaceData::UserDataUnmapped, sizeof(pIntfData->userDataMap));

    memset(&pIntfData->pushConst, 0, sizeof(pIntfData->pushConst));
    pIntfData->pushConst.resNodeIdx = InvalidValue;

    memset(&pIntfData->spillTable, 0, sizeof(pIntfData->spillTable));
    pIntfData->spillTable.offsetInDwords = InvalidValue;

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
    memset(&pIntfData->vbTable, 0, sizeof(pIntfData->vbTable));
    pIntfData->vbTable.resNodeIdx = InvalidValue;

    memset(&pIntfData->streamOutTable, 0, sizeof(pIntfData->streamOutTable));
    pIntfData->streamOutTable.resNodeIdx = InvalidValue;
#endif

    memset(&pIntfData->userDataUsage, 0, sizeof(pIntfData->userDataUsage));

    memset(&pIntfData->entryArgIdxs, 0, sizeof(pIntfData->entryArgIdxs));
    pIntfData->entryArgIdxs.spillTable = InvalidValue;
}

// =====================================================================================================================
// Gets the hash code of input shader with specified shader stage.
uint64_t PipelineContext::GetShaderHashCode(
    ShaderStage stage       // Shader stage
) const
{
    auto pShaderInfo = GetPipelineShaderInfo(stage);
    LLPC_ASSERT(pShaderInfo != nullptr);

    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

    return (pModuleData == nullptr) ? 0 :
        MetroHash::Compact64(reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash));
}

// =====================================================================================================================
// Set pipeline state in Builder
void PipelineContext::SetBuilderPipelineState(
    Builder*          pBuilder) const   // [in] The builder
{
    // Give the user data nodes and descriptor range values to the Builder.
    // The user data nodes have been merged so they are the same in each shader stage. Get them from
    // the first active stage.
    uint32_t stageMask = GetShaderStageMask();
    auto pShaderInfo = GetPipelineShaderInfo(ShaderStage(countTrailingZeros(stageMask)));
    ArrayRef<ResourceMappingNode> userDataNodes(pShaderInfo->pUserDataNodes,
                                                pShaderInfo->userDataNodeCount);
    ArrayRef<DescriptorRangeValue> descriptorRangeValues(pShaderInfo->pDescriptorRangeValues,
                                                         pShaderInfo->descriptorRangeValueCount);
    pBuilder->SetUserDataNodes(userDataNodes, descriptorRangeValues);
}

} // Llpc
