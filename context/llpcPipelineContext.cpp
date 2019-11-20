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
#include "llpcPipeline.h"

namespace llvm
{

namespace cl
{

extern opt<bool> EnablePipelineDump;

} // cl

} // llvm

using namespace llvm;

// -include-llvm-ir: include LLVM IR as a separate section in the ELF binary
static cl::opt<bool> IncludeLlvmIr("include-llvm-ir",
                                   cl::desc("Include LLVM IR as a separate section in the ELF binary"),
                                   cl::init(false));

// -vgpr-limit: maximum VGPR limit for this shader
static cl::opt<uint32_t> VgprLimit("vgpr-limit", cl::desc("Maximum VGPR limit for this shader"), cl::init(0));

// -sgpr-limit: maximum SGPR limit for this shader
static cl::opt<uint32_t> SgprLimit("sgpr-limit", cl::desc("Maximum SGPR limit for this shader"), cl::init(0));

// -waves-per-eu: the maximum number of waves per EU for this shader
static cl::opt<uint32_t> WavesPerEu("waves-per-eu",
                                    cl::desc("Maximum number of waves per EU for this shader"),
                                    cl::init(0));

// -enable-load-scalarizer: Enable the optimization for load scalarizer.
static cl::opt<bool> EnableScalarLoad("enable-load-scalarizer",
                                      cl::desc("Enable the optimization for load scalarizer."),
                                      cl::init(false));

// -scalar-threshold: Set the vector size threshold for load scalarizer.
static cl::opt<unsigned> ScalarThreshold("scalar-threshold",
                                         cl::desc("The threshold for load scalarizer"),
                                         cl::init(0xFFFFFFFF));

// -enable-si-scheduler: enable target option si-scheduler
static cl::opt<bool> EnableSiScheduler("enable-si-scheduler",
                                       cl::desc("Enable target option si-scheduler"),
                                       cl::init(false));

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
const char* PipelineContext::GetGpuNameString(
    GfxIpVersion gfxIp)   // Graphics IP version info
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
#if LLPC_BUILD_GFX10
        { { 10, 1, 0xFFFF }, "gfx101F" },
        { { 10, 1, 0xFFFE }, "gfx101E" },
        { { 10, 1, 0 }, "gfx1010" },  // [10.1.0] gfx1010
        { { 10, 1, 0xFFFD}, "gfx101D" },
        { { 10, 1, 2 }, "gfx1012" },  // [10.1.2] gfx1012, navi14
#endif
    };

    const GpuNameStringMap* pNameMap = nullptr;
    for (auto& nameMap : GpuNameMap)
    {
        if ((nameMap.gfxIp.major    == gfxIp.major) &&
            (nameMap.gfxIp.minor    == gfxIp.minor) &&
            (nameMap.gfxIp.stepping == gfxIp.stepping))
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
const char* PipelineContext::GetGpuNameAbbreviation(
    GfxIpVersion gfxIp)  // Graphics IP version info
{
    const char* pNameAbbr = nullptr;
    switch (gfxIp.major)
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
    ShaderStage    shaderStage,      // Shader stage
    ResourceUsage* pResUsage)        // [out] Resource usage
{
    memset(&pResUsage->builtInUsage, 0, sizeof(pResUsage->builtInUsage));

    pResUsage->pushConstSizeInBytes = 0;
    pResUsage->resourceWrite = false;
    pResUsage->resourceRead = false;
    pResUsage->perShaderTable = false;

    pResUsage->numSgprsAvailable = UINT32_MAX;
    pResUsage->numVgprsAvailable = UINT32_MAX;

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
    InterfaceData* pIntfData)  // [out] Interface data
{
    pIntfData->userDataCount = 0;
    memset(pIntfData->userDataMap, InterfaceData::UserDataUnmapped, sizeof(pIntfData->userDataMap));

    memset(&pIntfData->pushConst, 0, sizeof(pIntfData->pushConst));
    pIntfData->pushConst.resNodeIdx = InvalidValue;

    memset(&pIntfData->spillTable, 0, sizeof(pIntfData->spillTable));
    pIntfData->spillTable.offsetInDwords = InvalidValue;

    memset(&pIntfData->userDataUsage, 0, sizeof(pIntfData->userDataUsage));

    memset(&pIntfData->entryArgIdxs, 0, sizeof(pIntfData->entryArgIdxs));
    pIntfData->entryArgIdxs.spillTable = InvalidValue;
}

// =====================================================================================================================
// Gets the hash code of input shader with specified shader stage.
ShaderHash PipelineContext::GetShaderHashCode(
    ShaderStage stage       // Shader stage
) const
{
    auto pShaderInfo = GetPipelineShaderInfo(stage);
    LLPC_ASSERT(pShaderInfo != nullptr);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
    if((pShaderInfo->options.clientHash.upper != 0) &&
       (pShaderInfo->options.clientHash.lower != 0))
    {
        return pShaderInfo->options.clientHash;
    }
    else
    {
        ShaderHash hash = {};
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

        if(pModuleData != nullptr)
        {
            hash.lower = MetroHash::Compact64(reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash));
            hash.upper = 0;
        }
        return hash;
    }
#else
    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

    return (pModuleData == nullptr) ? 0 :
        MetroHash::Compact64(reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash));
#endif
}

// =====================================================================================================================
// Set pipeline state in Pipeline object for middle-end
void PipelineContext::SetPipelineState(
    Pipeline*    pPipeline) const   // [in/out] Middle-end pipeline object
{
    // Give the shader stage mask to the middle-end.
    uint32_t stageMask = GetShaderStageMask();
    pPipeline->SetShaderStageMask(stageMask);

    // Give the pipeline options to the middle-end.
    SetOptionsInPipeline(pPipeline);

    // Give the user data nodes to the middle-end.
    SetUserDataInPipeline(pPipeline);
}

// =====================================================================================================================
// Give the pipeline options to the middle-end.
void PipelineContext::SetOptionsInPipeline(
    Pipeline*    pPipeline) const   // [in/out] Middle-end pipeline object
{
    Options options = {};
    options.hash[0] = GetPiplineHashCode();
    options.hash[1] = GetCacheHashCode();

    options.includeDisassembly = (cl::EnablePipelineDump || EnableOuts() || GetPipelineOptions()->includeDisassembly);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
    options.reconfigWorkgroupLayout = GetPipelineOptions()->reconfigWorkgroupLayout;
#endif
    options.includeIr = (IncludeLlvmIr || GetPipelineOptions()->includeIr);

#if LLPC_BUILD_GFX10
    if (IsGraphics() && (GetGfxIpVersion().major >= 10))
    {
        // Only set NGG options for a GFX10+ graphics pipeline.
        auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo());
        const auto& nggState = pPipelineInfo->nggState;
        if (nggState.enableNgg == false)
        {
            options.nggFlags |= NggFlagDisable;
        }
        else
        {
            options.nggFlags =
                  (nggState.enableGsUse ? NggFlagEnableGsUse : 0) |
                  (nggState.forceNonPassthrough ? NggFlagForceNonPassthrough : 0) |
                  (nggState.alwaysUsePrimShaderTable ? 0 : NggFlagDontAlwaysUsePrimShaderTable) |
                  (nggState.compactMode == NggCompactSubgroup ? NggFlagCompactSubgroup : 0) |
                  (nggState.enableFastLaunch ? NggFlagEnableFastLaunch : 0) |
                  (nggState.enableVertexReuse ? NggFlagEnableVertexReuse : 0) |
                  (nggState.enableBackfaceCulling ? NggFlagEnableBackfaceCulling : 0) |
                  (nggState.enableFrustumCulling ? NggFlagEnableFrustumCulling : 0) |
                  (nggState.enableBoxFilterCulling ? NggFlagEnableBoxFilterCulling : 0) |
                  (nggState.enableSphereCulling ? NggFlagEnableSphereCulling : 0) |
                  (nggState.enableSmallPrimFilter ? NggFlagEnableSmallPrimFilter : 0) |
                  (nggState.enableCullDistanceCulling ? NggFlagEnableCullDistanceCulling : 0);
            options.nggBackfaceExponent = nggState.backfaceExponent;
            options.nggSubgroupSizing = nggState.subgroupSizing;
            options.nggVertsPerSubgroup = nggState.vertsPerSubgroup;
            options.nggPrimsPerSubgroup = nggState.primsPerSubgroup;
        }
    }
#endif

    pPipeline->SetOptions(options);

    // Give the shader options (including the hash) to the middle-end.
    uint32_t stageMask = GetShaderStageMask();
    for (uint32_t stage = 0; stage <= ShaderStageCompute; ++stage)
    {
        if (stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage)))
        {
            ShaderOptions shaderOptions = {};

            ShaderHash hash = GetShaderHashCode(static_cast<ShaderStage>(stage));
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            // 128-bit hash
            shaderOptions.hash[0] = hash.lower;
            shaderOptions.hash[1] = hash.upper;
#else
            // 64-bit hash
            shaderOptions.hash[0] = hash;
#endif

            const PipelineShaderInfo* pShaderInfo = GetPipelineShaderInfo(static_cast<ShaderStage>(stage));
            shaderOptions.trapPresent = pShaderInfo->options.trapPresent;
            shaderOptions.debugMode = pShaderInfo->options.debugMode;
            shaderOptions.allowReZ = pShaderInfo->options.allowReZ;

            if ((pShaderInfo->options.vgprLimit != 0) && (pShaderInfo->options.vgprLimit != UINT_MAX))
            {
                shaderOptions.vgprLimit = pShaderInfo->options.vgprLimit;
            }
            else
            {
                shaderOptions.vgprLimit = VgprLimit;
            }

            if ((pShaderInfo->options.sgprLimit != 0) && (pShaderInfo->options.sgprLimit != UINT_MAX))
            {
                shaderOptions.sgprLimit = pShaderInfo->options.sgprLimit;
            }
            else
            {
                shaderOptions.sgprLimit = SgprLimit;
            }

            if (pShaderInfo->options.maxThreadGroupsPerComputeUnit != 0)
            {
                shaderOptions.maxThreadGroupsPerComputeUnit = pShaderInfo->options.maxThreadGroupsPerComputeUnit;
            }
            else
            {
                shaderOptions.maxThreadGroupsPerComputeUnit = WavesPerEu;
            }

#if LLPC_BUILD_GFX10
            shaderOptions.waveBreakSize = pShaderInfo->options.waveBreakSize;
#endif

            bool loadScalarizerEnabled = EnableScalarLoad;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
            loadScalarizerEnabled |= pShaderInfo->options.enableLoadScalarizer;
#endif
            shaderOptions.loadScalarizerThreshold = loadScalarizerEnabled ? ScalarThreshold : 0;

            shaderOptions.useSiScheduler = EnableSiScheduler;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
            shaderOptions.useSiScheduler |= pShaderInfo->options.useSiScheduler;
#endif

            shaderOptions.unrollThreshold = pShaderInfo->options.unrollThreshold;

            pPipeline->SetShaderOptions(static_cast<ShaderStage>(stage), shaderOptions);
        }
    }
}

// =====================================================================================================================
// Give the user data nodes and descriptor range values to the middle-end.
// The user data nodes have been merged so they are the same in each shader stage. Get them from
// the first active stage.
void PipelineContext::SetUserDataInPipeline(
    Pipeline*    pPipeline) const   // [in/out] Middle-end pipeline object
{
    const PipelineShaderInfo* pShaderInfo = nullptr;
    uint32_t stageMask = GetShaderStageMask();
    {
        pShaderInfo = GetPipelineShaderInfo(ShaderStage(countTrailingZeros(stageMask)));
    }
    ArrayRef<ResourceMappingNode> userDataNodes(pShaderInfo->pUserDataNodes,
                                                pShaderInfo->userDataNodeCount);
    ArrayRef<DescriptorRangeValue> descriptorRangeValues(pShaderInfo->pDescriptorRangeValues,
                                                         pShaderInfo->descriptorRangeValueCount);
    pPipeline->SetUserDataNodes(userDataNodes, descriptorRangeValues);
}

} // Llpc
