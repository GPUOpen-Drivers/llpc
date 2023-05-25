/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcGraphicsContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::GraphicsContext.
 ***********************************************************************************************************************
 */
#include "llpcGraphicsContext.h"
#include "SPIRVInternal.h"
#include "llpcCompiler.h"
#include "vkgcPipelineDumper.h"
#include "lgc/Builder.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "llpc-graphics-context"

using namespace llvm;
using namespace SPIRV;
using namespace lgc;
using namespace Vkgc;

namespace Llpc {

// -disable-fetch-shader: disable the fetch shader when doing unlinked shaders.
static cl::opt<bool> DisableFetchShader("disable-fetch-shader", cl::desc("Disable fetch shaders"), cl::init(false));

// -disable-color-export-shader: disable the color export shader when doing unlinked shaders.
static cl::opt<bool> DisableColorExportShader("disable-color-export-shader", cl::desc("Disable color export shaders"),
                                              cl::init(false));

// =====================================================================================================================
//
// @param gfxIp : Graphics Ip version info
// @param pipelineInfo : Graphics pipeline build info
// @param pipelineHash : Pipeline hash code
// @param cacheHash : Cache hash code
GraphicsContext::GraphicsContext(GfxIpVersion gfxIp, const GraphicsPipelineBuildInfo *pipelineInfo,
                                 MetroHash::Hash *pipelineHash, MetroHash::Hash *cacheHash)
    : PipelineContext(gfxIp, pipelineHash, cacheHash
#if VKI_RAY_TRACING
                      ,
                      &pipelineInfo->rtState
#endif
                      ),
      m_pipelineInfo(pipelineInfo), m_stageMask(0), m_preRasterHasGs(false), m_useDualSourceBlend(false),
      m_activeStageCount(0) {

  setUnlinked(pipelineInfo->unlinked);
  // clang-format off
  const PipelineShaderInfo *shaderInfo[ShaderStageGfxCount] = {
    &pipelineInfo->task,
    &pipelineInfo->vs,
    &pipelineInfo->tcs,
    &pipelineInfo->tes,
    &pipelineInfo->gs,
    &pipelineInfo->mesh,
    &pipelineInfo->fs,
  };
  // clang-format on
  for (unsigned stage = 0; stage < ShaderStageGfxCount; ++stage) {
    if (shaderInfo[stage]->pModuleData) {
      m_stageMask |= shaderStageToMask(static_cast<ShaderStage>(stage));
      ++m_activeStageCount;

      if (stage == ShaderStageGeometry) {
        m_stageMask |= shaderStageToMask(ShaderStageCopyShader);
        ++m_activeStageCount;
      }
    }
  }

  m_resourceMapping = pipelineInfo->resourceMapping;
  m_pipelineLayoutApiHash = pipelineInfo->pipelineLayoutApiHash;
}

// =====================================================================================================================
GraphicsContext::~GraphicsContext() {
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
//
// @param shaderStage : Shader stage
const PipelineShaderInfo *GraphicsContext::getPipelineShaderInfo(unsigned shaderId) const {
  if (shaderId == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    shaderId = ShaderStageGeometry;
  }

  assert(shaderId < ShaderStageGfxCount);

  const PipelineShaderInfo *shaderInfo = nullptr;
  switch (shaderId) {
  case Llpc::ShaderStageTask:
    shaderInfo = &m_pipelineInfo->task;
    break;
  case Llpc::ShaderStageVertex:
    shaderInfo = &m_pipelineInfo->vs;
    break;
  case Llpc::ShaderStageTessControl:
    shaderInfo = &m_pipelineInfo->tcs;
    break;
  case Llpc::ShaderStageTessEval:
    shaderInfo = &m_pipelineInfo->tes;
    break;
  case Llpc::ShaderStageGeometry:
    shaderInfo = &m_pipelineInfo->gs;
    break;
  case Llpc::ShaderStageMesh:
    shaderInfo = &m_pipelineInfo->mesh;
    break;
  case Llpc::ShaderStageFragment:
    shaderInfo = &m_pipelineInfo->fs;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return shaderInfo;
}

// =====================================================================================================================
// Gets subgroup size usage
//
// @returns : Bitmask per stage, in the same order as defined in `Vkgc::ShaderStage`.
unsigned GraphicsContext::getSubgroupSizeUsage() const {
  // clang-format off
  std::array<const PipelineShaderInfo *, ShaderStageGfxCount> shaderInfos = {
    &m_pipelineInfo->task,
    &m_pipelineInfo->vs,
    &m_pipelineInfo->tcs,
    &m_pipelineInfo->tes,
    &m_pipelineInfo->gs,
    &m_pipelineInfo->mesh,
    &m_pipelineInfo->fs,
  };
  // clang-format on
  unsigned bitmask = 0;
  for (unsigned shaderInfoIdx = 0, e = shaderInfos.size(); shaderInfoIdx != e; ++shaderInfoIdx) {
    auto shaderInfo = shaderInfos[shaderInfoIdx];
    if (!shaderInfo->pModuleData)
      continue;
    auto *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
    if (moduleData->usage.useSubgroupSize)
      bitmask |= (1 << shaderInfoIdx);
  }
  return bitmask;
}

// =====================================================================================================================
// Set pipeline state in Pipeline object for middle-end and/or calculate the hash for the state to be added.
// Doing both these things in the same code ensures that we hash and use the same pipeline state in all situations.
// For graphics, we use the shader stage mask to decide which parts of graphics state to use, omitting
// pre-rasterization state if there are no pre-rasterization shaders, and omitting fragment state if there is
// no FS.
//
// @param [in/out] pipeline : Middle-end pipeline object; nullptr if only hashing pipeline state
// @param [in/out] hasher : Hasher object; nullptr if only setting LGC pipeline state
// @param unlinked : Do not provide some state to LGC, so offsets are generated as relocs, and a fetch shader
//                   is needed
void GraphicsContext::setPipelineState(Pipeline *pipeline, Util::MetroHash64 *hasher, bool unlinked) const {
  PipelineContext::setPipelineState(pipeline, hasher, unlinked);
  const unsigned stageMask = getShaderStageMask();

  if (pipeline) {
    // Give the shader options (including the hash) to the middle-end.
    const auto allStages = maskToShaderStages(stageMask);
    for (ShaderStage stage : make_filter_range(allStages, isNativeStage)) {
      const PipelineShaderInfo *shaderInfo = getPipelineShaderInfo(stage);

      assert(shaderInfo);

      pipeline->setShaderOptions(getLgcShaderStage(static_cast<ShaderStage>(stage)), computeShaderOptions(*shaderInfo));
    }
  }

  if ((stageMask & ~shaderStageToMask(ShaderStageFragment)) && (!unlinked || DisableFetchShader)) {
    // Set vertex input descriptions to the middle-end.
    setVertexInputDescriptions(pipeline, hasher);
  }

  if (isShaderStageInMask(ShaderStageFragment, stageMask) && (!unlinked || DisableColorExportShader)) {
    // Give the color export state to the middle-end.
    setColorExportState(pipeline, hasher);
  }

  // Give the graphics pipeline state to the middle-end.
  setGraphicsStateInPipeline(pipeline, hasher, stageMask);
}

// =====================================================================================================================
// For TCS, set inputVertices from patchControlPoints in the pipeline state.
void GraphicsContext::setTcsInputVertices(Module *tcsModule) {
  const auto &inputIaState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->iaState;
  if (inputIaState.patchControlPoints == 0)
    return;
  TessellationMode tessellationMode = lgc::Pipeline::getTessellationMode(*tcsModule, lgc::ShaderStageTessControl);
  tessellationMode.inputVertices = inputIaState.patchControlPoints;
  lgc::Pipeline::setTessellationMode(*tcsModule, lgc::ShaderStageTessControl, tessellationMode);
}

// =====================================================================================================================
// Give the pipeline options to the middle-end, and/or hash them.
Options GraphicsContext::computePipelineOptions() const {
  Options options = PipelineContext::computePipelineOptions();

  options.enableUberFetchShader =
      reinterpret_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->enableUberFetchShader;
  if (getGfxIpVersion().major >= 10) {
    // Only set NGG options for a GFX10+ graphics pipeline.
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo());
    const auto &nggState = pipelineInfo->nggState;
#if VKI_BUILD_GFX11
    if (!nggState.enableNgg && getGfxIpVersion().major < 11) // GFX11+ must enable NGG
#else
    if (!nggState.enableNgg)
#endif
      options.nggFlags |= NggFlagDisable;
    else {
      options.nggFlags = (nggState.enableGsUse ? NggFlagEnableGsUse : 0) |
                         (nggState.forceCullingMode ? NggFlagForceCullingMode : 0) |
                         (nggState.compactVertex ? NggFlagCompactVertex : 0) |
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
  return options;
}

// =====================================================================================================================
// Set color export state in middle-end Pipeline object, and/or hash it.
//
// @param [in/out] pipeline : Middle-end pipeline object; nullptr if only hashing
// @param [in/out] hasher : Hasher object; nullptr if only setting LGC pipeline state
void GraphicsContext::setColorExportState(Pipeline *pipeline, Util::MetroHash64 *hasher) const {
  auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo());
  const auto &cbState = pipelineInfo->cbState;

  if (hasher)
    hasher->Update(cbState);
  if (!pipeline)
    return; // Only hashing.

  ColorExportState state = {};
  SmallVector<ColorExportFormat, MaxColorTargets> formats;

  state.alphaToCoverageEnable = cbState.alphaToCoverageEnable;
  state.dualSourceBlendEnable =
      cbState.dualSourceBlendEnable || (pipelineInfo->cbState.dualSourceBlendDynamic && getUseDualSourceBlend());

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

  if (state.alphaToCoverageEnable && formats.empty()) {
    // NOTE: We must export alpha channel for alpha to coverage, if there is no color export,
    // we force a dummy color export.
    formats.push_back({BufDataFormat32, BufNumFormatFloat});
  }

  pipeline->setColorExportState(formats, state);
}

// =====================================================================================================================
// Set vertex input descriptions in middle-end Pipeline object, or hash them.
//
// @param [in/out] pipeline : Middle-end pipeline object; nullptr if only hashing
// @param [in/out] hasher : Hasher object; nullptr if only setting LGC pipeline state
void GraphicsContext::setVertexInputDescriptions(Pipeline *pipeline, Util::MetroHash64 *hasher) const {
  auto vertexInput = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->pVertexInput;
  if (!vertexInput)
    return;

  if (hasher) {
    PipelineDumper::updateHashForVertexInputState(
        vertexInput, static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->dynamicVertexStride,
        hasher);
  }
  if (!pipeline)
    return; // Only hashing.

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
          (static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->dynamicVertexStride
               ? 0
               : binding->stride),
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
// Give the graphics pipeline state to the middle-end, and/or hash it. If stageMask has no pre-rasterization shader
// stages, do not consider pre-rasterization pipeline state. If stageMask has no FS, do not consider FS state.
//
// @param [in/out] pipeline : Middle-end pipeline object; nullptr if only hashing
// @param [in/out] hasher : Hasher object; nullptr if only setting LGC pipeline state
// @param stageMask : Bitmap of shader stages
void GraphicsContext::setGraphicsStateInPipeline(Pipeline *pipeline, Util::MetroHash64 *hasher,
                                                 unsigned stageMask) const {
  const auto &inputIaState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->iaState;
  if (pipeline)
    pipeline->setDeviceIndex(inputIaState.deviceIndex);
  if (hasher)
    hasher->Update(inputIaState.deviceIndex);
  const auto &inputRsState = static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->rsState;

  InputAssemblyState inputAssemblyState = {};
  inputAssemblyState.enableMultiView = inputIaState.enableMultiView;
  RasterizerState rasterizerState = {};

  if (stageMask & ~shaderStageToMask(ShaderStageFragment)) {
    // Pre-rasterization shader stages are present.
    switch (inputIaState.topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      inputAssemblyState.primitiveType = PrimitiveType::Point;
      break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      inputAssemblyState.primitiveType = PrimitiveType::LineList;
      break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      inputAssemblyState.primitiveType = PrimitiveType::LineStrip;
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      inputAssemblyState.primitiveType = PrimitiveType::TriangleList;
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      inputAssemblyState.primitiveType = PrimitiveType::TriangleStrip;
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      inputAssemblyState.primitiveType = PrimitiveType::TriangleFan;
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
      inputAssemblyState.primitiveType = PrimitiveType::TriangleListAdjacency;
      break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
      inputAssemblyState.primitiveType = PrimitiveType::TriangleStripAdjacency;
      break;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      inputAssemblyState.primitiveType = PrimitiveType::Patch;
      break;
    default:
      llvm_unreachable("");
    }

    inputAssemblyState.disableVertexReuse = inputIaState.disableVertexReuse;
    inputAssemblyState.switchWinding = inputIaState.switchWinding;

    if (hasher) {
      // We need to hash patchControlPoints here, even though it is used separately in setTcsInputVertices as
      // LGC needs it in the TCS shader mode.
      hasher->Update(inputIaState.patchControlPoints);
    }

    rasterizerState.rasterizerDiscardEnable = inputRsState.rasterizerDiscardEnable;
    rasterizerState.usrClipPlaneMask = inputRsState.usrClipPlaneMask;
    rasterizerState.provokingVertexMode = static_cast<ProvokingVertexMode>(inputRsState.provokingVertexMode);
  }

  if (isShaderStageInMask(ShaderStageFragment, stageMask)) {
    rasterizerState.innerCoverage = inputRsState.innerCoverage;
    rasterizerState.perSampleShading = inputRsState.perSampleShading;
    rasterizerState.numSamples = inputRsState.numSamples;
    rasterizerState.samplePatternIdx = inputRsState.samplePatternIdx;
  }

  if (pipeline)
    pipeline->setGraphicsState(inputAssemblyState, rasterizerState);
  if (hasher) {
    hasher->Update(inputAssemblyState);
    hasher->Update(rasterizerState);
  }

  if (isShaderStageInMask(ShaderStageFragment, stageMask)) {
    // Fragment shader is present.
    const VkPipelineDepthStencilStateCreateInfo &inputDsState =
        static_cast<const GraphicsPipelineBuildInfo *>(getPipelineBuildInfo())->dsState;
    DepthStencilState depthStencilState = {};
    if (inputDsState.depthTestEnable) {
      depthStencilState.depthTestEnable = inputDsState.depthTestEnable;
      depthStencilState.depthCompareOp = inputDsState.depthCompareOp;
    }
    if (inputDsState.stencilTestEnable) {
      depthStencilState.stencilTestEnable = inputDsState.stencilTestEnable;
      depthStencilState.stencilCompareOpFront = inputDsState.front.compareOp;
      depthStencilState.stencilCompareOpBack = inputDsState.back.compareOp;
    }

    if (pipeline)
      pipeline->setDepthStencilState(depthStencilState);
    if (hasher)
      hasher->Update(depthStencilState);
  }
}

// =====================================================================================================================
// Gets client-defined metadata
StringRef GraphicsContext::getClientMetadata() const {
  return StringRef(static_cast<const char *>(m_pipelineInfo->pClientMetadata), m_pipelineInfo->clientMetadataSize);
}

} // namespace Llpc
