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
#include "lgc/Builder.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "llpc-graphics-context"

using namespace llvm;
using namespace SPIRV;

namespace Llpc {

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
      m_pipelineInfo(pipelineInfo), m_stageMask(0), m_preRasterHasGs(false), m_activeStageCount(0) {

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
const PipelineShaderInfo *GraphicsContext::getPipelineShaderInfo(ShaderStage shaderStage) const {
  if (shaderStage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    shaderStage = ShaderStageGeometry;
  }

  assert(shaderStage < ShaderStageGfxCount);

  const PipelineShaderInfo *shaderInfo = nullptr;
  switch (shaderStage) {
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

} // namespace Llpc
