/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcPipelineContext.h"
#include "SPIRVInternal.h"
#include "llpcCompiler.h"
#include "llpcDebug.h"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/Pipeline.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-pipeline-context"

namespace llvm {

namespace cl {

extern opt<bool> EnablePipelineDump;

} // namespace cl

} // namespace llvm

using namespace lgc;
using namespace llvm;

// -include-llvm-ir: include LLVM IR as a separate section in the ELF binary
static cl::opt<bool> IncludeLlvmIr("include-llvm-ir",
                                   cl::desc("Include LLVM IR as a separate section in the ELF binary"),
                                   cl::init(false));

// -vgpr-limit: maximum VGPR limit for this shader
static cl::opt<unsigned> VgprLimit("vgpr-limit", cl::desc("Maximum VGPR limit for this shader"), cl::init(0));

// -sgpr-limit: maximum SGPR limit for this shader
static cl::opt<unsigned> SgprLimit("sgpr-limit", cl::desc("Maximum SGPR limit for this shader"), cl::init(0));

// -waves-per-eu: the maximum number of waves per EU for this shader
static cl::opt<unsigned> WavesPerEu("waves-per-eu", cl::desc("Maximum number of waves per EU for this shader"),
                                    cl::init(0));

// -enable-load-scalarizer: Enable the optimization for load scalarizer.
static cl::opt<bool> EnableScalarLoad("enable-load-scalarizer",
                                      cl::desc("Enable the optimization for load scalarizer."), cl::init(true));

// The max threshold of load scalarizer.
static const unsigned MaxScalarThreshold = 0xFFFFFFFF;

// -scalar-threshold: Set the vector size threshold for load scalarizer.
static cl::opt<unsigned> ScalarThreshold("scalar-threshold", cl::desc("The threshold for load scalarizer"),
                                         cl::init(3));

// -enable-si-scheduler: enable target option si-scheduler
static cl::opt<bool> EnableSiScheduler("enable-si-scheduler", cl::desc("Enable target option si-scheduler"),
                                       cl::init(false));

// -disable-licm: annotate loops with metadata to disable the LLVM LICM pass (this is now an alias
// for -disable-licm-threshold=1 which remains for backwards compatibility)
static cl::opt<bool> DisableLicm("disable-licm", cl::desc("Disable LLVM LICM pass"), cl::init(false));

// -disable-color-export-shader: disable the color export shader when doing unlinked shaders.
static cl::opt<bool> DisableColorExportShader("disable-color-export-shader", cl::desc("Disable color export shaders"),
                                              cl::init(false));

// -subgroup-size: sub-group size exposed via Vulkan API.
static cl::opt<int> SubgroupSize("subgroup-size", cl::desc("Sub-group size exposed via Vulkan API"), cl::init(64));

// -enable-shadow-desc: enable shadow descriptor table
static cl::opt<bool> EnableShadowDescriptorTable("enable-shadow-desc", cl::desc("Enable shadow descriptor table"));

// -shadow-desc-table-ptr-high: high part of VA for shadow descriptor table pointer.
// Default of 2 is for use by standalone amdllpc.
static cl::opt<unsigned> ShadowDescTablePtrHigh("shadow-desc-table-ptr-high",
                                                cl::desc("High part of VA for shadow descriptor table pointer"),
                                                cl::init(2));

// -force-loop-unroll-count: Force to set the loop unroll count.
static cl::opt<int> ForceLoopUnrollCount("force-loop-unroll-count", cl::desc("Force loop unroll count"), cl::init(0));

// -disable-licm-threshold: disable LICM for loops with at least the specified number of blocks
static cl::opt<int>
    DisableLicmThreshold("disable-licm-threshold",
                         cl::desc("Disable LICM for loops with at least the specified number of blocks"), cl::init(20));

// -unroll-hint-threshold: loop unroll threshold to use for loops with Unroll hint
static cl::opt<int> UnrollHintThreshold("unroll-hint-threshold",
                                        cl::desc("loop unroll threshold to use for loops with Unroll hint"),
                                        cl::init(0));

// -dontunroll-hint-threshold: loop unroll threshold to use for loops with DontUnroll hint
static cl::opt<int> DontUnrollHintThreshold("dontunroll-hint-threshold",
                                            cl::desc("loop unroll threshold to use for loops with DontUnroll hint"),
                                            cl::init(0));

namespace Llpc {

// =====================================================================================================================
//
// @param gfxIp : Graphics IP version info
// @param pipelineHash : Pipeline hash code
// @param cacheHash : Cache hash code
PipelineContext::PipelineContext(GfxIpVersion gfxIp, MetroHash::Hash *pipelineHash, MetroHash::Hash *cacheHash)
    : m_gfxIp(gfxIp), m_pipelineHash(*pipelineHash), m_cacheHash(*cacheHash), m_resourceMapping() {
}

// =====================================================================================================================
PipelineContext::~PipelineContext() {
}

// =====================================================================================================================
// Gets the name string of GPU target according to graphics IP version info.
//
// @param gfxIp : Graphics IP version info
// @param [out] gpuName : LLVM GPU name
void PipelineContext::getGpuNameString(GfxIpVersion gfxIp, std::string &gpuName) {
  // A GfxIpVersion from PAL is three decimal numbers for major, minor and stepping. This function
  // converts that to an LLVM target name, whith is "gfx" followed by the three decimal numbers with
  // no separators, e.g. "gfx1010" for 10.1.0. A high stepping number 0xFFFA..0xFFFF denotes an
  // experimental target, and that is represented by the final hexadecimal digit, e.g. "gfx101A"
  // for 10.1.0xFFFA. In gfx9, stepping numbers 10..35 are represented by lower case letter 'a'..'z'.
  gpuName.clear();
  raw_string_ostream gpuNameStream(gpuName);
  gpuNameStream << "gfx" << gfxIp.major << gfxIp.minor;
  if (gfxIp.stepping >= 0xFFFA)
    gpuNameStream << char(gfxIp.stepping - 0xFFFA + 'A');
  else if (gfxIp.major == 9 && gfxIp.stepping >= 10)
    gpuNameStream << char(gfxIp.stepping - 10 + 'a');
  else
    gpuNameStream << gfxIp.stepping;
}

// =====================================================================================================================
// Gets the name string of the abbreviation for GPU target according to graphics IP version info.
//
// @param gfxIp : Graphics IP version info
const char *PipelineContext::getGpuNameAbbreviation(GfxIpVersion gfxIp) {
  const char *nameAbbr = nullptr;
  switch (gfxIp.major) {
  case 6:
    nameAbbr = "SI";
    break;
  case 7:
    nameAbbr = "CI";
    break;
  case 8:
    nameAbbr = "VI";
    break;
  case 9:
    nameAbbr = "GFX9";
    break;
  default:
    nameAbbr = "UNKNOWN";
    break;
  }

  return nameAbbr;
}

// =====================================================================================================================
// Gets the hash code of input shader with specified shader stage.
//
// @param stage : Shader stage
ShaderHash PipelineContext::getShaderHashCode(ShaderStage stage) const {
  auto shaderInfo = getPipelineShaderInfo(stage);
  assert(shaderInfo);

  if (shaderInfo->options.clientHash.upper != 0 && shaderInfo->options.clientHash.lower != 0)
    return shaderInfo->options.clientHash;
  else {
    ShaderHash hash = {};
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);

    if (moduleData) {
      hash.lower = MetroHash::compact64(reinterpret_cast<const MetroHash::Hash *>(&moduleData->hash));
      hash.upper = 0;
    }
    return hash;
  }
}

// =====================================================================================================================
// Set pipeline state in Pipeline object for middle-end
//
// @param [in/out] pipeline : Middle-end pipeline object
// @param unlinked : Do not provide some state to LGC, so offsets are generated as relocs, and a fetch shader
//                   is needed
void PipelineContext::setPipelineState(Pipeline *pipeline, bool unlinked) const {
  // Give the shader stage mask to the middle-end. We need to translate the Vkgc::ShaderStage bit numbers
  // to lgc::ShaderStage bit numbers.
  unsigned stageMask = getShaderStageMask();
  unsigned lgcStageMask = 0;
  for (unsigned stage = 0; stage != ShaderStageCount; ++stage) {
    if (stageMask & (1 << stage))
      lgcStageMask |= 1 << getLgcShaderStage(static_cast<ShaderStage>(stage));
  }
  pipeline->setShaderStageMask(lgcStageMask);

  // Give the pipeline options to the middle-end.
  setOptionsInPipeline(pipeline);

  if (!unlinked) {
    // Give the user data nodes to the middle-end.
    setUserDataInPipeline(pipeline);
  }

  if (isGraphics()) {
    if (!unlinked) {
      // Set vertex input descriptions to the middle-end.
      setVertexInputDescriptions(pipeline);
    }

    if (!unlinked || DisableColorExportShader) {
      // Give the color export state to the middle-end.
      setColorExportState(pipeline);
    }

    // Give the graphics pipeline state to the middle-end.
    setGraphicsStateInPipeline(pipeline);
  } else
    pipeline->setDeviceIndex(static_cast<const ComputePipelineBuildInfo *>(getPipelineBuildInfo())->deviceIndex);
}

// =====================================================================================================================
// Give the pipeline options to the middle-end.
//
// @param [in/out] pipeline : Middle-end pipeline object
void PipelineContext::setOptionsInPipeline(Pipeline *pipeline) const {
  Options options = {};
  options.hash[0] = getPipelineHashCode();
  options.hash[1] = getCacheHashCode();

  options.includeDisassembly = (cl::EnablePipelineDump || EnableOuts() || getPipelineOptions()->includeDisassembly);
  options.reconfigWorkgroupLayout = getPipelineOptions()->reconfigWorkgroupLayout;
  options.includeIr = (IncludeLlvmIr || getPipelineOptions()->includeIr);

  switch (getPipelineOptions()->shadowDescriptorTableUsage) {
  case Vkgc::ShadowDescriptorTableUsage::Auto:
    // Use default of 2 for standalone amdllpc.
    options.shadowDescriptorTable = ShadowDescTablePtrHigh;
    break;
  case Vkgc::ShadowDescriptorTableUsage::Enable:
    options.shadowDescriptorTable = getPipelineOptions()->shadowDescriptorTablePtrHigh;
    break;
  case Vkgc::ShadowDescriptorTableUsage::Disable:
    options.shadowDescriptorTable = ShadowDescriptorTableDisable;
    break;
  }

  // Shadow descriptor command line options override pipeline options.
  if (EnableShadowDescriptorTable.getNumOccurrences() > 0) {
    if (!EnableShadowDescriptorTable)
      options.shadowDescriptorTable = ShadowDescriptorTableDisable;
    else
      options.shadowDescriptorTable = ShadowDescTablePtrHigh;
  }

  if (isGraphics() && getGfxIpVersion().major >= 10) {
    // Only set NGG options for a GFX10+ graphics pipeline.
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo());
    const auto &nggState = pipelineInfo->nggState;
    if (!nggState.enableNgg)
      options.nggFlags |= NggFlagDisable;
    else {
      options.nggFlags = (nggState.enableGsUse ? NggFlagEnableGsUse : 0) |
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 44
                         (nggState.forceNonPassthrough ? NggFlagForceCullingMode : 0) |
#else
                         (nggState.forceCullingMode ? NggFlagForceCullingMode : 0) |
#endif
                         (nggState.alwaysUsePrimShaderTable ? 0 : NggFlagDontAlwaysUsePrimShaderTable) |
                         (nggState.compactMode == NggCompactDisable ? NggFlagCompactDisable : 0) |
                         (nggState.enableVertexReuse ? NggFlagEnableVertexReuse : 0) |
                         (nggState.enableBackfaceCulling ? NggFlagEnableBackfaceCulling : 0) |
                         (nggState.enableFrustumCulling ? NggFlagEnableFrustumCulling : 0) |
                         (nggState.enableBoxFilterCulling ? NggFlagEnableBoxFilterCulling : 0) |
                         (nggState.enableSphereCulling ? NggFlagEnableSphereCulling : 0) |
                         (nggState.enableSmallPrimFilter ? NggFlagEnableSmallPrimFilter : 0) |
                         (nggState.enableCullDistanceCulling ? NggFlagEnableCullDistanceCulling : 0);
      options.nggBackfaceExponent = nggState.backfaceExponent;

      // Use a static cast from Vkgc NggSubgroupSizingType to LGC NggSubgroupSizing, and static assert that
      // that is valid.
      static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::Auto) == NggSubgroupSizing::Auto, "Mismatch");
      static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::MaximumSize) ==
                        NggSubgroupSizing::MaximumSize,
                    "Mismatch");
      static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::HalfSize) == NggSubgroupSizing::HalfSize,
                    "Mismatch");
      static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::OptimizeForVerts) ==
                        NggSubgroupSizing::OptimizeForVerts,
                    "Mismatch");
      static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::OptimizeForPrims) ==
                        NggSubgroupSizing::OptimizeForPrims,
                    "Mismatch");
      static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::Explicit) == NggSubgroupSizing::Explicit,
                    "Mismatch");
      options.nggSubgroupSizing = static_cast<NggSubgroupSizing>(nggState.subgroupSizing);

      options.nggVertsPerSubgroup = nggState.vertsPerSubgroup;
      options.nggPrimsPerSubgroup = nggState.primsPerSubgroup;
    }
  }

  options.allowNullDescriptor = getPipelineOptions()->extendedRobustness.nullDescriptor;
  options.disableImageResourceCheck = getPipelineOptions()->disableImageResourceCheck;

  pipeline->setOptions(options);

  // Give the shader options (including the hash) to the middle-end.
  unsigned stageMask = getShaderStageMask();
  for (unsigned stage = 0; stage <= ShaderStageCompute; ++stage) {
    if (stageMask & shaderStageToMask(static_cast<ShaderStage>(stage))) {
      ShaderOptions shaderOptions = {};

      ShaderHash hash = getShaderHashCode(static_cast<ShaderStage>(stage));
      // 128-bit hash
      shaderOptions.hash[0] = hash.lower;
      shaderOptions.hash[1] = hash.upper;

      const PipelineShaderInfo *shaderInfo = getPipelineShaderInfo(static_cast<ShaderStage>(stage));
      shaderOptions.trapPresent = shaderInfo->options.trapPresent;
      shaderOptions.debugMode = shaderInfo->options.debugMode;
      shaderOptions.allowReZ = shaderInfo->options.allowReZ;

      if (shaderInfo->options.vgprLimit != 0 && shaderInfo->options.vgprLimit != UINT_MAX)
        shaderOptions.vgprLimit = shaderInfo->options.vgprLimit;
      else
        shaderOptions.vgprLimit = VgprLimit;

      if (shaderInfo->options.sgprLimit != 0 && shaderInfo->options.sgprLimit != UINT_MAX)
        shaderOptions.sgprLimit = shaderInfo->options.sgprLimit;
      else
        shaderOptions.sgprLimit = SgprLimit;

      if (shaderInfo->options.maxThreadGroupsPerComputeUnit != 0)
        shaderOptions.maxThreadGroupsPerComputeUnit = shaderInfo->options.maxThreadGroupsPerComputeUnit;
      else
        shaderOptions.maxThreadGroupsPerComputeUnit = WavesPerEu;

      shaderOptions.waveSize = shaderInfo->options.waveSize;
      shaderOptions.wgpMode = shaderInfo->options.wgpMode;
      if (!shaderInfo->options.allowVaryWaveSize) {
        // allowVaryWaveSize is disabled, so use -subgroup-size (default 64) to override the wave
        // size for a shader that uses gl_SubgroupSize.
        shaderOptions.subgroupSize = SubgroupSize;
      }

      // Use a static cast from Vkgc WaveBreakSize to LGC WaveBreak, and static assert that
      // that is valid.
      static_assert(static_cast<WaveBreak>(WaveBreakSize::None) == WaveBreak::None, "Mismatch");
      static_assert(static_cast<WaveBreak>(WaveBreakSize::_8x8) == WaveBreak::_8x8, "Mismatch");
      static_assert(static_cast<WaveBreak>(WaveBreakSize::_16x16) == WaveBreak::_16x16, "Mismatch");
      static_assert(static_cast<WaveBreak>(WaveBreakSize::_32x32) == WaveBreak::_32x32, "Mismatch");
      shaderOptions.waveBreakSize = static_cast<WaveBreak>(shaderInfo->options.waveBreakSize);

      shaderOptions.loadScalarizerThreshold = 0;
      if (EnableScalarLoad)
        shaderOptions.loadScalarizerThreshold = ScalarThreshold;

      if (shaderInfo->options.enableLoadScalarizer) {
        if (shaderInfo->options.scalarThreshold != 0)
          shaderOptions.loadScalarizerThreshold = shaderInfo->options.scalarThreshold;
        else
          shaderOptions.loadScalarizerThreshold = MaxScalarThreshold;
      }

      shaderOptions.useSiScheduler = EnableSiScheduler || shaderInfo->options.useSiScheduler;
      shaderOptions.updateDescInElf = shaderInfo->options.updateDescInElf;
      shaderOptions.unrollThreshold = shaderInfo->options.unrollThreshold;
      // A non-zero command line -force-loop-unroll-count value overrides the shaderInfo option value.
      shaderOptions.forceLoopUnrollCount =
          ForceLoopUnrollCount ? ForceLoopUnrollCount : shaderInfo->options.forceLoopUnrollCount;

      static_assert(static_cast<lgc::DenormalMode>(Vkgc::DenormalMode::Auto) == lgc::DenormalMode::Auto, "Mismatch");
      static_assert(static_cast<lgc::DenormalMode>(Vkgc::DenormalMode::FlushToZero) == lgc::DenormalMode::FlushToZero,
                    "Mismatch");
      static_assert(static_cast<lgc::DenormalMode>(Vkgc::DenormalMode::Preserve) == lgc::DenormalMode::Preserve,
                    "Mismatch");
      shaderOptions.fp32DenormalMode = static_cast<lgc::DenormalMode>(shaderInfo->options.fp32DenormalMode);
      shaderOptions.adjustDepthImportVrs = shaderInfo->options.adjustDepthImportVrs;
      // disableLicmThreshold is set to the first of:
      // - a non-zero value from the corresponding shaderInfo option
      // - a value of 1 if DisableLicm or the disableLicm shaderInfo option is true
      // - the value of DisableLicmThreshold
      // Default is 0, which does not disable LICM
      if (shaderInfo->options.disableLicmThreshold > 0)
        shaderOptions.disableLicmThreshold = shaderInfo->options.disableLicmThreshold;
      else if (DisableLicm || shaderInfo->options.disableLicm)
        shaderOptions.disableLicmThreshold = 1;
      else
        shaderOptions.disableLicmThreshold = DisableLicmThreshold;
      if (shaderInfo->options.unrollHintThreshold > 0)
        shaderOptions.unrollHintThreshold = shaderInfo->options.unrollHintThreshold;
      else
        shaderOptions.unrollHintThreshold = UnrollHintThreshold;
      if (shaderInfo->options.dontUnrollHintThreshold > 0)
        shaderOptions.dontUnrollHintThreshold = shaderInfo->options.dontUnrollHintThreshold;
      else
        shaderOptions.dontUnrollHintThreshold = DontUnrollHintThreshold;
      pipeline->setShaderOptions(getLgcShaderStage(static_cast<ShaderStage>(stage)), shaderOptions);
    }
  }
}

// =====================================================================================================================
// Give the user data nodes and descriptor range values to the middle-end.
// The user data nodes have been merged so they are the same in each shader stage. Get them from
// the first active stage.
//
// @param [in/out] pipeline : Middle-end pipeline object
void PipelineContext::setUserDataInPipeline(Pipeline *pipeline) const {
  auto resourceMapping = getResourceMapping();
  auto allocNodes = std::make_unique<ResourceMappingNode[]>(resourceMapping->userDataNodeCount);

  for (unsigned idx = 0; idx < resourceMapping->userDataNodeCount; ++idx)
      allocNodes[idx] = resourceMapping->pUserDataNodes[idx].node;

  // Translate the resource nodes into the LGC format expected by Pipeline::SetUserDataNodes.
  ArrayRef<ResourceMappingNode> nodes(allocNodes.get(), resourceMapping->userDataNodeCount);
  ArrayRef<StaticDescriptorValue> descriptorRangeValues(resourceMapping->pStaticDescriptorValues,
                                                        resourceMapping->staticDescriptorValueCount);

  // First, create a map of immutable nodes.
  ImmutableNodesMap immutableNodesMap;
  for (auto &rangeValue : descriptorRangeValues)
    immutableNodesMap[{rangeValue.set, rangeValue.binding}] = &rangeValue;

  // Count how many user data nodes we have, and allocate the buffer.
  unsigned nodeCount = nodes.size();
  for (auto &node : nodes) {
    if (node.type == ResourceMappingNodeType::DescriptorTableVaPtr)
      nodeCount += node.tablePtr.nodeCount;
  }
  auto allocUserDataNodes = std::make_unique<ResourceNode[]>(nodeCount);

  // Copy nodes in.
  ResourceNode *destTable = allocUserDataNodes.get();
  ResourceNode *destInnerTable = destTable + nodeCount;
  auto userDataNodes = ArrayRef<ResourceNode>(destTable, nodes.size());
  setUserDataNodesTable(pipeline, nodes, immutableNodesMap, /*isRoot=*/true, destTable, destInnerTable);
  assert(destInnerTable == destTable + nodes.size());

  // Give the table to the LGC Pipeline interface.
  pipeline->setUserDataNodes(userDataNodes);
}

// =====================================================================================================================
// Set one user data table, and its inner tables. Used by SetUserDataInPipeline above, and recursively calls
// itself for an inner table. This translates from a Vkgc ResourceMappingNode to an LGC ResourceNode.
//
// @param context : LLVM context
// @param nodes : The resource mapping nodes
// @param immutableNodesMap : Map of immutable nodes
// @param isRoot : Whether this is the root table
// @param [out] destTable : Where to write nodes
// @param [in/out] destInnerTable : End of space available for inner tables
void PipelineContext::setUserDataNodesTable(Pipeline *pipeline, ArrayRef<ResourceMappingNode> nodes,
                                            const ImmutableNodesMap &immutableNodesMap, bool isRoot,
                                            ResourceNode *destTable, ResourceNode *&destInnerTable) const {
  for (unsigned idx = 0; idx != nodes.size(); ++idx) {
    auto &node = nodes[idx];
    auto &destNode = destTable[idx];

    destNode.sizeInDwords = node.sizeInDwords;
    destNode.offsetInDwords = node.offsetInDwords;

    switch (node.type) {
    case ResourceMappingNodeType::DescriptorTableVaPtr: {
      // Process an inner table.
      destNode.type = ResourceNodeType::DescriptorTableVaPtr;
      destInnerTable -= node.tablePtr.nodeCount;
      destNode.innerTable = ArrayRef<ResourceNode>(destInnerTable, node.tablePtr.nodeCount);
      setUserDataNodesTable(pipeline, ArrayRef<ResourceMappingNode>(node.tablePtr.pNext, node.tablePtr.nodeCount),
                            immutableNodesMap, /*isRoot=*/false, destInnerTable, destInnerTable);
      break;
    }
    case ResourceMappingNodeType::IndirectUserDataVaPtr: {
      // Process an indirect pointer.
      destNode.type = ResourceNodeType::IndirectUserDataVaPtr;
      destNode.indirectSizeInDwords = node.userDataPtr.sizeInDwords;
      break;
    }
    case ResourceMappingNodeType::StreamOutTableVaPtr: {
      // Process an indirect pointer.
      destNode.type = ResourceNodeType::StreamOutTableVaPtr;
      destNode.indirectSizeInDwords = node.userDataPtr.sizeInDwords;
      break;
    }
    default: {
      // Process an SRD. First check that a static_cast works to convert a Vkgc ResourceMappingNodeType
      // to an LGC ResourceNodeType (with the exception of DescriptorCombinedBvhBuffer, whose value
      // accidentally depends on LLPC version).
      static_assert(ResourceNodeType::DescriptorResource ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorResource),
                    "Mismatch");
      static_assert(ResourceNodeType::DescriptorSampler ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorSampler),
                    "Mismatch");
      static_assert(ResourceNodeType::DescriptorCombinedTexture ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorCombinedTexture),
                    "Mismatch");
      static_assert(ResourceNodeType::DescriptorTexelBuffer ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorTexelBuffer),
                    "Mismatch");
      static_assert(ResourceNodeType::DescriptorFmask ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorFmask),
                    "Mismatch");
      static_assert(ResourceNodeType::DescriptorBuffer ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorBuffer),
                    "Mismatch");
      static_assert(ResourceNodeType::PushConst == static_cast<ResourceNodeType>(ResourceMappingNodeType::PushConst),
                    "Mismatch");
      static_assert(ResourceNodeType::DescriptorBufferCompact ==
                        static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorBufferCompact),
                    "Mismatch");
      // A "PushConst" is in fact an InlineBuffer when it appears in a non-root table.
      if (node.type == ResourceMappingNodeType::PushConst && !isRoot)
        destNode.type = ResourceNodeType::InlineBuffer;
      else if (node.type == ResourceMappingNodeType::DescriptorYCbCrSampler)
        destNode.type = ResourceNodeType::DescriptorResource;
      else
        destNode.type = static_cast<ResourceNodeType>(node.type);

      destNode.set = node.srdRange.set;
      destNode.binding = node.srdRange.binding;
      destNode.immutableValue = nullptr;
      switch (node.type) {
      case ResourceMappingNodeType::DescriptorResource:
      case ResourceMappingNodeType::DescriptorFmask:
        destNode.stride = DescriptorSizeResource / sizeof(uint32_t);
        break;
      case ResourceMappingNodeType::DescriptorSampler:
        destNode.stride = DescriptorSizeSampler / sizeof(uint32_t);
        break;
      case ResourceMappingNodeType::DescriptorCombinedTexture:
        destNode.stride = (DescriptorSizeResource + DescriptorSizeSampler) / sizeof(uint32_t);
        break;
      case ResourceMappingNodeType::DescriptorYCbCrSampler:
        // Current node.sizeInDwords = resourceDescSizeInDwords * M * N (M means plane count, N means array count)
        // TODO: Desired destNode.stride = resourceDescSizeInDwords * M
        //
        // Temporary set stride to be node.sizeInDwords, for that the stride varies from different plane
        // counts, and we don't know the real plane count currently.
        // Thus, set stride to sizeInDwords, and just divide array count when it is available in handling immutable
        // sampler descriptor (For YCbCrSampler, immutable sampler is always accessible)
        destNode.stride = node.sizeInDwords;
        break;
      case ResourceMappingNodeType::DescriptorBufferCompact:
        destNode.stride = 2;
        break;
      default:
        destNode.stride = DescriptorSizeBuffer / sizeof(uint32_t);
        break;
      }

      // Only check for an immutable value if the resource is or contains a sampler. This specifically excludes
      // YCbCrSampler; that was handled in the SPIR-V reader.
      if (node.type != ResourceMappingNodeType::DescriptorSampler &&
          node.type != ResourceMappingNodeType::DescriptorCombinedTexture &&
          node.type != ResourceMappingNodeType::DescriptorYCbCrSampler)
        break;

      auto it = immutableNodesMap.find(std::pair<unsigned, unsigned>(destNode.set, destNode.binding));
      if (it != immutableNodesMap.end()) {
        // This set/binding is (or contains) an immutable value. The value can only be a sampler, so we
        // can assume it is four dwords.
        auto &immutableNode = *it->second;

        IRBuilder<> builder(pipeline->getContext());
        SmallVector<Constant *, 8> values;

        if (immutableNode.arraySize != 0) {
          if (node.type == ResourceMappingNodeType::DescriptorYCbCrSampler) {
            // TODO: Remove the statement when destNode.stride is per array size
            // Update destNode.stride = node.sizeInDwords / immutableNode.arraySize
            destNode.stride /= immutableNode.arraySize;
          }

          constexpr unsigned SamplerDescriptorSize = 4;

          for (unsigned compIdx = 0; compIdx < immutableNode.arraySize; ++compIdx) {
            Constant *compValues[SamplerDescriptorSize] = {};
            for (unsigned i = 0; i < SamplerDescriptorSize; ++i)
              compValues[i] = builder.getInt32(immutableNode.pValue[compIdx * SamplerDescriptorSize + i]);
            values.push_back(ConstantVector::get(compValues));
          }
          destNode.immutableValue = ConstantArray::get(ArrayType::get(values[0]->getType(), values.size()), values);
        }
      }
      break;
    }
    }
  }
}

// =====================================================================================================================
// Give the graphics pipeline state to the middle-end.
//
// @param [in/out] pipeline : Middle-end pipeline object
void PipelineContext::setGraphicsStateInPipeline(Pipeline *pipeline) const {
  const auto &inputIaState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->iaState;
  pipeline->setDeviceIndex(inputIaState.deviceIndex);

  InputAssemblyState inputAssemblyState = {};
  // PrimitiveTopology happens to have the same values as the corresponding Vulkan enum.
  inputAssemblyState.topology = static_cast<PrimitiveTopology>(inputIaState.topology);
  inputAssemblyState.patchControlPoints = inputIaState.patchControlPoints;
  inputAssemblyState.disableVertexReuse = inputIaState.disableVertexReuse;
  inputAssemblyState.switchWinding = inputIaState.switchWinding;
  inputAssemblyState.enableMultiView = inputIaState.enableMultiView;

  const auto &inputVpState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->vpState;
  ViewportState viewportState = {};
  viewportState.depthClipEnable = inputVpState.depthClipEnable;

  const auto &inputRsState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->rsState;
  RasterizerState rasterizerState = {};
  rasterizerState.rasterizerDiscardEnable = inputRsState.rasterizerDiscardEnable;
  rasterizerState.innerCoverage = inputRsState.innerCoverage;
  rasterizerState.perSampleShading = inputRsState.perSampleShading;
  rasterizerState.numSamples = inputRsState.numSamples;
  rasterizerState.samplePatternIdx = inputRsState.samplePatternIdx;
  rasterizerState.usrClipPlaneMask = inputRsState.usrClipPlaneMask;
  // PolygonMode and CullModeFlags happen to have the same values as their Vulkan equivalents.
  rasterizerState.polygonMode = static_cast<PolygonMode>(inputRsState.polygonMode);
  rasterizerState.cullMode = static_cast<CullModeFlags>(inputRsState.cullMode);
  rasterizerState.frontFaceClockwise = inputRsState.frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 46
  rasterizerState.depthBiasEnable = inputRsState.depthBiasEnable;
#endif

  pipeline->setGraphicsState(inputAssemblyState, viewportState, rasterizerState);
}

// =====================================================================================================================
// Set vertex input descriptions in middle-end Pipeline object
//
// @param pipeline : Pipeline object
void PipelineContext::setVertexInputDescriptions(Pipeline *pipeline) const {
  auto vertexInput = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->pVertexInput;
  if (!vertexInput)
    return;

  // Gather the bindings.
  SmallVector<VertexInputDescription, 8> bindings;
  for (unsigned i = 0; i < vertexInput->vertexBindingDescriptionCount; ++i) {
    auto binding = &vertexInput->pVertexBindingDescriptions[i];
    unsigned idx = binding->binding;
    if (idx >= bindings.size())
      bindings.resize(idx + 1);
    bindings[idx].binding = binding->binding;
    bindings[idx].stride = binding->stride;
    switch (binding->inputRate) {
    case VK_VERTEX_INPUT_RATE_VERTEX:
      bindings[idx].inputRate = VertexInputRateVertex;
      break;
    case VK_VERTEX_INPUT_RATE_INSTANCE:
      bindings[idx].inputRate = VertexInputRateInstance;
      break;
    default:
      llvm_unreachable("Should never be called!");
    }
  }

  // Check for divisors.
  auto vertexDivisor = findVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, vertexInput->pNext);
  if (vertexDivisor) {
    for (unsigned i = 0; i < vertexDivisor->vertexBindingDivisorCount; ++i) {
      auto divisor = &vertexDivisor->pVertexBindingDivisors[i];
      if (divisor->binding <= bindings.size())
        bindings[divisor->binding].inputRate = divisor->divisor;
    }
  }

  // Gather the vertex inputs.
  SmallVector<VertexInputDescription, 8> descriptions;
  for (unsigned i = 0; i < vertexInput->vertexAttributeDescriptionCount; ++i) {
    auto attrib = &vertexInput->pVertexAttributeDescriptions[i];
    if (attrib->binding >= bindings.size())
      continue;
    auto binding = &bindings[attrib->binding];
    if (binding->binding != attrib->binding)
      continue;

    auto dfmt = BufDataFormatInvalid;
    auto nfmt = BufNumFormatUnorm;
    std::tie(dfmt, nfmt) = mapVkFormat(attrib->format, /*isColorExport=*/false);

    if (dfmt != BufDataFormatInvalid) {
      descriptions.push_back({
          attrib->location,
          attrib->binding,
          attrib->offset,
          binding->stride,
          dfmt,
          nfmt,
          binding->inputRate,
      });
    }
  }

  // Give the vertex input descriptions to the middle-end Pipeline object.
  pipeline->setVertexInputDescriptions(descriptions);
}

// =====================================================================================================================
// Set color export state in middle-end Pipeline object
//
// @param pipeline : Pipeline object
void PipelineContext::setColorExportState(Pipeline *pipeline) const {
  const auto &cbState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->cbState;
  ColorExportState state = {};
  SmallVector<ColorExportFormat, MaxColorTargets> formats;

  state.alphaToCoverageEnable = cbState.alphaToCoverageEnable;
  state.dualSourceBlendEnable = cbState.dualSourceBlendEnable;

  for (unsigned targetIndex = 0; targetIndex < MaxColorTargets; ++targetIndex) {
    if (cbState.target[targetIndex].format != VK_FORMAT_UNDEFINED) {
      auto dfmt = BufDataFormatInvalid;
      auto nfmt = BufNumFormatUnorm;
      std::tie(dfmt, nfmt) = mapVkFormat(cbState.target[targetIndex].format, true);
      formats.resize(targetIndex + 1);
      formats[targetIndex].dfmt = dfmt;
      formats[targetIndex].nfmt = nfmt;
      formats[targetIndex].blendEnable = cbState.target[targetIndex].blendEnable;
      formats[targetIndex].blendSrcAlphaToColor = cbState.target[targetIndex].blendSrcAlphaToColor;
    }
  }

  pipeline->setColorExportState(formats, state);
}

// =====================================================================================================================
// Map a VkFormat to a {BufDataFormat, BufNumFormat}. Returns BufDataFormatInvalid if the
// VkFormat is not supported for vertex input.
//
// @param format : Vulkan API format code
// @param isColorExport : True for looking up color export format, false for vertex input format
std::pair<BufDataFormat, BufNumFormat> PipelineContext::mapVkFormat(VkFormat format, bool isColorExport) {
  struct FormatEntry {
#ifndef NDEBUG
    VkFormat format;
#endif
    BufDataFormat dfmt;
    BufNumFormat nfmt;
    unsigned validVertexFormat : 1;
    unsigned validExportFormat : 1;
  };

  static const FormatEntry FormatTable[] = {
#ifndef NDEBUG
#define INVALID_FORMAT_ENTRY(format)                                                                                   \
  { format, BufDataFormatInvalid, BufNumFormatUnorm, false, false }
#define VERTEX_FORMAT_ENTRY(format, dfmt, nfmt)                                                                        \
  { format, dfmt, nfmt, true, false }
#define COLOR_FORMAT_ENTRY(format, dfmt, nfmt)                                                                         \
  { format, dfmt, nfmt, false, true }
#define BOTH_FORMAT_ENTRY(format, dfmt, nfmt)                                                                          \
  { format, dfmt, nfmt, true, true }
#else
#define INVALID_FORMAT_ENTRY(format)                                                                                   \
  { BufDataFormatInvalid, BufNumFormatUnorm, false, false }
#define VERTEX_FORMAT_ENTRY(format, dfmt, nfmt)                                                                        \
  { dfmt, nfmt, true, false }
#define COLOR_FORMAT_ENTRY(format, dfmt, nfmt)                                                                         \
  { dfmt, nfmt, false, true }
#define BOTH_FORMAT_ENTRY(format, dfmt, nfmt)                                                                          \
  { dfmt, nfmt, true, true }
#endif
      INVALID_FORMAT_ENTRY(VK_FORMAT_UNDEFINED),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R4G4_UNORM_PACK8, BufDataFormat4_4, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R4G4B4A4_UNORM_PACK16, BufDataFormat4_4_4_4, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B4G4R4A4_UNORM_PACK16, BufDataFormat4_4_4_4_Bgra, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R5G6B5_UNORM_PACK16, BufDataFormat5_6_5, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B5G6R5_UNORM_PACK16, BufDataFormat5_6_5_Bgr, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R5G5B5A1_UNORM_PACK16, BufDataFormat5_6_5_1, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B5G5R5A1_UNORM_PACK16, BufDataFormat5_6_5_1_Bgra, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_A1R5G5B5_UNORM_PACK16, BufDataFormat1_5_6_5, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8_UNORM, BufDataFormat8, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8_SNORM, BufDataFormat8, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8_USCALED, BufDataFormat8, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8_SSCALED, BufDataFormat8, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8_UINT, BufDataFormat8, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8_SINT, BufDataFormat8, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8_SRGB, BufDataFormat8, BufNumFormatSrgb),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8_UNORM, BufDataFormat8_8, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8_SNORM, BufDataFormat8_8, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8_USCALED, BufDataFormat8_8, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8_SSCALED, BufDataFormat8_8, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8_UINT, BufDataFormat8_8, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8_SINT, BufDataFormat8_8, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8_SRGB, BufDataFormat8_8, BufNumFormatSrgb),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_UNORM, BufDataFormat8_8_8, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_SNORM, BufDataFormat8_8_8, BufNumFormatSnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_USCALED, BufDataFormat8_8_8, BufNumFormatUscaled),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_SSCALED, BufDataFormat8_8_8, BufNumFormatSscaled),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_UINT, BufDataFormat8_8_8, BufNumFormatUint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_SINT, BufDataFormat8_8_8, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8_SRGB, BufDataFormat8_8_8, BufNumFormatSrgb),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_UNORM, BufDataFormat8_8_8_Bgr, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_SNORM, BufDataFormat8_8_8_Bgr, BufNumFormatSnorm),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_USCALED, BufDataFormat8_8_8_Bgr, BufNumFormatUscaled),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_SSCALED, BufDataFormat8_8_8_Bgr, BufNumFormatSscaled),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_UINT, BufDataFormat8_8_8_Bgr, BufNumFormatUint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_SINT, BufDataFormat8_8_8_Bgr, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8_SRGB, BufDataFormat8_8_8_Bgr, BufNumFormatSrgb),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_UNORM, BufDataFormat8_8_8_8, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_SNORM, BufDataFormat8_8_8_8, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_USCALED, BufDataFormat8_8_8_8, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_SSCALED, BufDataFormat8_8_8_8, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_UINT, BufDataFormat8_8_8_8, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_SINT, BufDataFormat8_8_8_8, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_R8G8B8A8_SRGB, BufDataFormat8_8_8_8, BufNumFormatSrgb),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_UNORM, BufDataFormat8_8_8_8_Bgra, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_SNORM, BufDataFormat8_8_8_8_Bgra, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_USCALED, BufDataFormat8_8_8_8_Bgra, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_SSCALED, BufDataFormat8_8_8_8_Bgra, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_UINT, BufDataFormat8_8_8_8_Bgra, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_SINT, BufDataFormat8_8_8_8_Bgra, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_B8G8R8A8_SRGB, BufDataFormat8_8_8_8_Bgra, BufNumFormatSrgb),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_UNORM_PACK32, BufDataFormat8_8_8_8, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_SNORM_PACK32, BufDataFormat8_8_8_8, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_USCALED_PACK32, BufDataFormat8_8_8_8, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_SSCALED_PACK32, BufDataFormat8_8_8_8, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_UINT_PACK32, BufDataFormat8_8_8_8, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_SINT_PACK32, BufDataFormat8_8_8_8, BufNumFormatSint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_A8B8G8R8_SRGB_PACK32, BufDataFormat8_8_8_8, BufNumFormatSrgb),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2R10G10B10_UNORM_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2R10G10B10_SNORM_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2R10G10B10_USCALED_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2R10G10B10_SSCALED_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2R10G10B10_UINT_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2R10G10B10_SINT_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2B10G10R10_UNORM_PACK32, BufDataFormat2_10_10_10, BufNumFormatUnorm),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_A2B10G10R10_SNORM_PACK32, BufDataFormat2_10_10_10, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2B10G10R10_USCALED_PACK32, BufDataFormat2_10_10_10, BufNumFormatUscaled),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_A2B10G10R10_SSCALED_PACK32, BufDataFormat2_10_10_10, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_A2B10G10R10_UINT_PACK32, BufDataFormat2_10_10_10, BufNumFormatUint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_A2B10G10R10_SINT_PACK32, BufDataFormat2_10_10_10, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_UNORM, BufDataFormat16, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_SNORM, BufDataFormat16, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_USCALED, BufDataFormat16, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_SSCALED, BufDataFormat16, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_UINT, BufDataFormat16, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_SINT, BufDataFormat16, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16_SFLOAT, BufDataFormat16, BufNumFormatFloat),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_UNORM, BufDataFormat16_16, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_SNORM, BufDataFormat16_16, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_USCALED, BufDataFormat16_16, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_SSCALED, BufDataFormat16_16, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_UINT, BufDataFormat16_16, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_SINT, BufDataFormat16_16, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16_SFLOAT, BufDataFormat16_16, BufNumFormatFloat),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_UNORM),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_SNORM),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_USCALED),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_SSCALED),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_UINT),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_SINT),
      INVALID_FORMAT_ENTRY(VK_FORMAT_R16G16B16_SFLOAT),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_UNORM, BufDataFormat16_16_16_16, BufNumFormatUnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_SNORM, BufDataFormat16_16_16_16, BufNumFormatSnorm),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_USCALED, BufDataFormat16_16_16_16, BufNumFormatUscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_SSCALED, BufDataFormat16_16_16_16, BufNumFormatSscaled),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_UINT, BufDataFormat16_16_16_16, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_SINT, BufDataFormat16_16_16_16, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R16G16B16A16_SFLOAT, BufDataFormat16_16_16_16, BufNumFormatFloat),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32_UINT, BufDataFormat32, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32_SINT, BufDataFormat32, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32_SFLOAT, BufDataFormat32, BufNumFormatFloat),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32_UINT, BufDataFormat32_32, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32_SINT, BufDataFormat32_32, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32_SFLOAT, BufDataFormat32_32, BufNumFormatFloat),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32B32_UINT, BufDataFormat32_32_32, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32B32_SINT, BufDataFormat32_32_32, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32B32_SFLOAT, BufDataFormat32_32_32, BufNumFormatFloat),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32B32A32_UINT, BufDataFormat32_32_32_32, BufNumFormatUint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32B32A32_SINT, BufDataFormat32_32_32_32, BufNumFormatSint),
      BOTH_FORMAT_ENTRY(VK_FORMAT_R32G32B32A32_SFLOAT, BufDataFormat32_32_32_32, BufNumFormatFloat),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64_UINT, BufDataFormat64, BufNumFormatUint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64_SINT, BufDataFormat64, BufNumFormatSint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64_SFLOAT, BufDataFormat64, BufNumFormatFloat),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64_UINT, BufDataFormat64_64, BufNumFormatUint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64_SINT, BufDataFormat64_64, BufNumFormatSint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64_SFLOAT, BufDataFormat64_64, BufNumFormatFloat),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64B64_UINT, BufDataFormat64_64_64, BufNumFormatUint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64B64_SINT, BufDataFormat64_64_64, BufNumFormatSint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64B64_SFLOAT, BufDataFormat64_64_64, BufNumFormatFloat),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64B64A64_UINT, BufDataFormat64_64_64_64, BufNumFormatUint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64B64A64_SINT, BufDataFormat64_64_64_64, BufNumFormatSint),
      VERTEX_FORMAT_ENTRY(VK_FORMAT_R64G64B64A64_SFLOAT, BufDataFormat64_64_64_64, BufNumFormatFloat),
      BOTH_FORMAT_ENTRY(VK_FORMAT_B10G11R11_UFLOAT_PACK32, BufDataFormat10_11_11, BufNumFormatFloat),
      COLOR_FORMAT_ENTRY(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, BufDataFormat5_9_9_9, BufNumFormatFloat),
      COLOR_FORMAT_ENTRY(VK_FORMAT_D16_UNORM, BufDataFormat16, BufNumFormatUnorm),
      INVALID_FORMAT_ENTRY(VK_FORMAT_X8_D24_UNORM_PACK32),
      COLOR_FORMAT_ENTRY(VK_FORMAT_D32_SFLOAT, BufDataFormat32, BufNumFormatFloat),
      COLOR_FORMAT_ENTRY(VK_FORMAT_S8_UINT, BufDataFormat8, BufNumFormatUint),
      COLOR_FORMAT_ENTRY(VK_FORMAT_D16_UNORM_S8_UINT, BufDataFormat16, BufNumFormatFloat),
      INVALID_FORMAT_ENTRY(VK_FORMAT_D24_UNORM_S8_UINT),
      COLOR_FORMAT_ENTRY(VK_FORMAT_D32_SFLOAT_S8_UINT, BufDataFormat32, BufNumFormatFloat),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC1_RGB_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC1_RGB_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC2_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC2_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC3_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC3_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC4_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC4_SNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC5_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC5_SNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC6H_UFLOAT_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC6H_SFLOAT_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC7_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_BC7_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_EAC_R11_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_EAC_R11_SNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_EAC_R11G11_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_EAC_R11G11_SNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_4x4_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_4x4_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_5x4_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_5x4_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_5x5_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_5x5_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_6x5_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_6x5_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_6x6_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_6x6_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_8x5_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_8x5_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_8x6_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_8x6_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_8x8_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_8x8_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x5_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x5_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x6_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x6_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x8_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x8_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x10_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_10x10_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_12x10_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_12x10_SRGB_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_12x12_UNORM_BLOCK),
      INVALID_FORMAT_ENTRY(VK_FORMAT_ASTC_12x12_SRGB_BLOCK),
  };

  static const DenseMap<unsigned, FormatEntry> FormatTableExt = {
#ifndef NDEBUG
#define INVALID_FORMAT_ENTRY_EXT(format)                                                                               \
  {                                                                                                                    \
    format, { format, BufDataFormatInvalid, BufNumFormatUnorm, false, false }                                          \
  }
#define VERTEX_FORMAT_ENTRY_EXT(format, dfmt, nfmt)                                                                    \
  {                                                                                                                    \
    format, { format, dfmt, nfmt, true, false }                                                                        \
  }
#define COLOR_FORMAT_ENTRY_EXT(format, dfmt, nfmt)                                                                     \
  {                                                                                                                    \
    format, { format, dfmt, nfmt, false, true }                                                                        \
  }
#define BOTH_FORMAT_ENTRY_EXT(format, dfmt, nfmt)                                                                      \
  {                                                                                                                    \
    format, { format, dfmt, nfmt, true, true }                                                                         \
  }
#else
#define INVALID_FORMAT_ENTRY_EXT(format)                                                                               \
  {                                                                                                                    \
    format, { BufDataFormatInvalid, BufNumFormatUnorm, false, false }                                                  \
  }
#define VERTEX_FORMAT_ENTRY_EXT(format, dfmt, nfmt)                                                                    \
  {                                                                                                                    \
    format, { dfmt, nfmt, true, false }                                                                                \
  }
#define COLOR_FORMAT_ENTRY_EXT(format, dfmt, nfmt)                                                                     \
  {                                                                                                                    \
    format, { dfmt, nfmt, false, true }                                                                                \
  }
#define BOTH_FORMAT_ENTRY_EXT(format, dfmt, nfmt)                                                                      \
  {                                                                                                                    \
    format, { dfmt, nfmt, true, true }                                                                                 \
  }
#endif
      COLOR_FORMAT_ENTRY_EXT(VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT, BufDataFormat4_4_4_4, BufNumFormatUnorm),
      COLOR_FORMAT_ENTRY_EXT(VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT, BufDataFormat4_4_4_4, BufNumFormatUnorm),
  };

  BufDataFormat dfmt = BufDataFormatInvalid;
  BufNumFormat nfmt = BufNumFormatUnorm;
  if (format < ArrayRef<FormatEntry>(FormatTable).size()) {
    assert(format == FormatTable[format].format);
    if ((isColorExport && FormatTable[format].validExportFormat) ||
        (!isColorExport && FormatTable[format].validVertexFormat)) {
      dfmt = FormatTable[format].dfmt;
      nfmt = FormatTable[format].nfmt;
    }
  } else {
    // Formats defined by Vulkan extensions
    if (FormatTableExt.count(format) != 0) {
      auto formatEntry = FormatTableExt.lookup(format);
      if ((isColorExport && formatEntry.validExportFormat) || (!isColorExport && formatEntry.validVertexFormat)) {
        dfmt = formatEntry.dfmt;
        nfmt = formatEntry.nfmt;
      }
    }
  }
  return {dfmt, nfmt};
}

} // namespace Llpc
