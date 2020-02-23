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
 * @file  llpcPipelineDumper.h
 * @brief LLPC header file: contains definitions of LLPC pipline dump utility
 ***********************************************************************************************************************
 */
#pragma once

#include <fstream>
#include <llpc.h>

#include "llpcMetroHash.h"
#if !defined(SINGLE_EXTERNAL_METROHASH)
namespace MetroHash
{
    class MetroHash64;
    struct Hash;
};
#endif

namespace Llpc
{

struct ComputePipelineBuildInfo;
struct GraphicsPipelineBuildInfo;
struct BinaryData;
struct PipelineDumpFile;

// Enumerates which types of pipeline dump are disable
enum PipelineDumpFilters : uint32_t
{
    PipelineDumpFilterNone = 0x00, // Do not disable any pipeline type
    PipelineDumpFilterCs   = 0x01, // Disable pipeline dump for Cs
    PipelineDumpFilterNgg  = 0x02, // Disable pipeline dump for NGG
    PipelineDumpFilterGs   = 0x04, // Disable pipeline dump for Gs
    PipelineDumpFilterTess = 0x08, // Disable pipeline dump for Tess
    PipelineDumpFilterVsPs = 0x10, // Disable pipeline dump for VsPs
};

class PipelineDumper
{
public:
    static void DumpSpirvBinary(const char*                     pDumpDir,
                                const BinaryData*               pSpirvBin,
                                MetroHash::Hash*                pHash);

    static PipelineDumpFile* BeginPipelineDump(const PipelineDumpOptions*       pDumpOptions,
                                               PipelineBuildInfo                pipelineInfo,
                                               const MetroHash::Hash*           pHash);

    static void EndPipelineDump(PipelineDumpFile* pDumpFile);

    static void DumpPipelineBinary(PipelineDumpFile*                pBinaryFile,
                                   GfxIpVersion                     gfxIp,
                                   const BinaryData*                pPipelineBin);

    static void DumpPipelineExtraInfo(PipelineDumpFile*             pBinaryFile,
                                      const std::string*            pStr);

    static MetroHash::Hash GenerateHashForGraphicsPipeline(const GraphicsPipelineBuildInfo* pPipeline,
                                                           bool                             isCacheHash,
                                                           uint32_t                         stage = ShaderStageInvalid);

    static MetroHash::Hash GenerateHashForComputePipeline(const ComputePipelineBuildInfo* pPipeline, bool isCacheHash);

    static std::string GetPipelineInfoFileName(PipelineBuildInfo                pipelineInfo,
                                               const MetroHash::Hash*           pHash);

    static void UpdateHashForPipelineShaderInfo(ShaderStage               stage,
                                                const PipelineShaderInfo* pShaderInfo,
                                                bool                      isCacheHash,
#if defined(SINGLE_EXTERNAL_METROHASH)
                                                Util::MetroHash64*        pHasher);
#else
                                                MetroHash::MetroHash64*   pHasher);
#endif

    static void UpdateHashForVertexInputState(const VkPipelineVertexInputStateCreateInfo* pVertexInput,
#if defined(SINGLE_EXTERNAL_METROHASH)
                                              Util::MetroHash64*                          pHasher);
#else
                                              MetroHash::MetroHash64*                     pHasher);
#endif

    // Update hash for map object
    template <class MapType>
#if defined(SINGLE_EXTERNAL_METROHASH)
    static void UpdateHashForMap(MapType& m, Util::MetroHash64* pHasher)
#else
    static void UpdateHashForMap(MapType& m, MetroHash::MetroHash64* pHasher)
#endif
    {
        pHasher->Update(m.size());
        for (auto mapIt : m)
        {
            pHasher->Update(mapIt.first);
            pHasher->Update(mapIt.second);
        }
    }

    static void UpdateHashForNonFragmentState(
        const GraphicsPipelineBuildInfo* pPipeline,
        bool                             isCacheHash,
#if defined(SINGLE_EXTERNAL_METROHASH)
        Util::MetroHash64*               pHasher);
#else
        MetroHash::MetroHash64*          pHasher);
#endif

    static void UpdateHashForFragmentState(
        const GraphicsPipelineBuildInfo* pPipeline,
#if defined(SINGLE_EXTERNAL_METROHASH)
        Util::MetroHash64*               pHasher);
#else
        MetroHash::MetroHash64*          pHasher);
#endif

    // Get name of register, or "" if not known
    static const char* GetRegisterNameString(uint32_t regNumber);

private:
    static std::string GetSpirvBinaryFileName(const MetroHash::Hash* pHash);

    static void DumpComputePipelineInfo(std::ostream*                   pDumpFile,
                                       const ComputePipelineBuildInfo* pPipelineInfo);
    static void DumpGraphicsPipelineInfo(std::ostream*                    pDumpFile,
                                         const GraphicsPipelineBuildInfo* pPipelineInfo);

    static void DumpVersionInfo(std::ostream&                  dumpFile);
    static void DumpPipelineShaderInfo(const PipelineShaderInfo* pShaderInfo,
                                       std::ostream&             dumpFile);
    static void DumpResourceMappingNode(const ResourceMappingNode* pUserDataNode,
                                        const char*                pPrefix,
                                        std::ostream&              dumpFile);
    static void DumpComputeStateInfo(const ComputePipelineBuildInfo* pPipelineInfo,
                                     std::ostream&                   dumpFile);
    static void DumpGraphicsStateInfo(const GraphicsPipelineBuildInfo* pPipelineInfo,
                                      std::ostream&                    dumpFile);
    static void DumpPipelineOptions(const PipelineOptions*   pOptions,
                                    std::ostream&            dumpFile);

    static void UpdateHashForResourceMappingNode(const ResourceMappingNode* pUserDataNode,
                                                 bool                       isRootNode,
#if defined(SINGLE_EXTERNAL_METROHASH)
                                                 Util::MetroHash64*         pHasher);
#else
                                                 MetroHash::MetroHash64*    pHasher);
#endif
};

} // Llpc
