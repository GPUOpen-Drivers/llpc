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
 * @file  llpcGraphicsContext.h
 * @brief LLPC header file: contains declaration of class Llpc::GraphicsContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPipelineContext.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace Llpc {

// =====================================================================================================================
// Represents LLPC context for graphics pipeline compilation. Derived from the base class Llpc::Context.
class GraphicsContext : public PipelineContext {
public:
  GraphicsContext(GfxIpVersion gfxIp, const GraphicsPipelineBuildInfo *pipelineInfo, MetroHash::Hash *pipelineHash,
                  MetroHash::Hash *cacheHash);
  virtual ~GraphicsContext();

  virtual bool isGraphics() const override { return true; }

  // Gets pipeline shader info of the specified shader stage
  virtual const PipelineShaderInfo *getPipelineShaderInfo(ShaderStage shaderStage) const override;

  // Gets pipeline build info
  virtual const void *getPipelineBuildInfo() const override { return m_pipelineInfo; }

  // Gets the mask of active shader stages bound to this pipeline
  virtual unsigned getShaderStageMask() const override { return m_stageMask; }

  // Sets the mask of active shader stages bound to this pipeline
  virtual void setShaderStageMask(unsigned mask) override { m_stageMask = mask; }

  // Sets whether pre-rasterization part has a geometry shader
  virtual void setPreRasterHasGs(bool preRasterHasGs) override { m_preRasterHasGs = preRasterHasGs; }

  // Gets whether pre-rasterization part has a geometry shader
  virtual bool getPreRasterHasGs() const override { return m_preRasterHasGs; };

  // Gets the count of active shader stages
  virtual unsigned getActiveShaderStageCount() const override { return m_activeStageCount; }

  // Gets per pipeline options
  virtual const PipelineOptions *getPipelineOptions() const override { return &m_pipelineInfo->options; }

  // Gets subgroup size usage
  virtual unsigned getSubgroupSizeUsage() const override;

#if VKI_RAY_TRACING
  virtual bool hasRayQuery() const override { return (m_pipelineInfo->shaderLibrary.codeSize > 0); }
#endif

private:
  GraphicsContext() = delete;
  GraphicsContext(const GraphicsContext &) = delete;
  GraphicsContext &operator=(const GraphicsContext &) = delete;

  const GraphicsPipelineBuildInfo *m_pipelineInfo; // Info to build a graphics pipeline

  unsigned m_stageMask;        // Mask of active shader stages bound to this graphics pipeline
  bool m_preRasterHasGs;       // Whether pre-rasterization part has a geometry shader
  unsigned m_activeStageCount; // Count of active shader stages
};

} // namespace Llpc
