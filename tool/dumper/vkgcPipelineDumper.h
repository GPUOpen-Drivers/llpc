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
 * @file  vkgcPipelineDumper.h
 * @brief VKGC header file: contains definitions of VKGC pipline dump utility
 ***********************************************************************************************************************
 */
#pragma once

#include <fstream>
#include "vkgcDefs.h"

#include "vkgcMetroHash.h"
#if !defined(SINGLE_EXTERNAL_METROHASH)
namespace MetroHash
{
    class MetroHash64;
    struct Hash;
};
#endif

namespace Vkgc
{

struct ComputePipelineBuildInfo;
struct GraphicsPipelineBuildInfo;
struct BinaryData;
struct PipelineDumpFile;

// Enumerates which types of pipeline dump are disable
enum PipelineDumpFilters : unsigned
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
#if defined(SINGLE_EXTERNAL_METROHASH)
    typedef Util::MetroHash64 MetroHash64;
#else
    typedef MetroHash::MetroHash64 MetroHash64;
#endif

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
                                                           unsigned                         stage = ShaderStageInvalid);

    static MetroHash::Hash GenerateHashForComputePipeline(const ComputePipelineBuildInfo* pPipeline, bool isCacheHash);

    static std::string GetPipelineInfoFileName(PipelineBuildInfo                pipelineInfo,
                                               const MetroHash::Hash*           pHash);

    static void UpdateHashForPipelineShaderInfo(ShaderStage               stage,
                                                const PipelineShaderInfo* pShaderInfo,
                                                bool                      isCacheHash,
                                                MetroHash64*              pHasher);

    static void UpdateHashForVertexInputState(const VkPipelineVertexInputStateCreateInfo* pVertexInput,
                                              MetroHash64*                          pHasher);

    // Update hash for map object
    template <class MapType>
    static void UpdateHashForMap(MapType& m, MetroHash64* pHasher)
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
        MetroHash64*               pHasher);

    static void UpdateHashForFragmentState(
        const GraphicsPipelineBuildInfo* pPipeline,
        MetroHash64*               pHasher);

    // Get name of register, or "" if not known
    static const char* getRegisterNameString(unsigned regNumber);

private:
    static std::string GetSpirvBinaryFileName(const MetroHash::Hash* pHash);

    static void DumpComputePipelineInfo(std::ostream*                   pDumpFile,
                                        const char*                     pDumpDir,
                                        const ComputePipelineBuildInfo* pPipelineInfo);
    static void DumpGraphicsPipelineInfo(std::ostream*                    pDumpFile,
                                         const char*                      pDumpDir,
                                         const GraphicsPipelineBuildInfo* pPipelineInfo);

    static void DumpVersionInfo(std::ostream&                  dumpFile);
    static void DumpPipelineShaderInfo(const PipelineShaderInfo* pShaderInfo,
                                       std::ostream&             dumpFile);
    static void DumpResourceMappingNode(const ResourceMappingNode* pUserDataNode,
                                        const char*                pPrefix,
                                        std::ostream&              dumpFile);
    static void DumpComputeStateInfo(const ComputePipelineBuildInfo* pPipelineInfo,
                                     const char*                     pDumpDir,
                                     std::ostream&                   dumpFile);
    static void DumpGraphicsStateInfo(const GraphicsPipelineBuildInfo* pPipelineInfo,
                                      const char*                      pDumpDir,
                                      std::ostream&                    dumpFile);
    static void DumpPipelineOptions(const PipelineOptions*   pOptions,
                                    std::ostream&            dumpFile);

    static void UpdateHashForResourceMappingNode(const ResourceMappingNode* pUserDataNode,
                                                 bool                       isRootNode,
                                                 MetroHash64*         pHasher);
};

} // Vkgc
