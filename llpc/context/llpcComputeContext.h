/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcComputeContext.h
 * @brief LLPC header file: contains declaration of class Llpc::ComputeContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPipelineContext.h"

namespace Llpc {

// =====================================================================================================================
// Represents LLPC context for compute pipeline compilation. Derived from the base class Llpc::Context.
class ComputeContext : public PipelineContext {
public:
  ComputeContext(GfxIpVersion gfxIp, const ComputePipelineBuildInfo *pipelineInfo, MetroHash::Hash *pipelineHash,
                 MetroHash::Hash *cacheHash);
  virtual ~ComputeContext() {}

  virtual const PipelineShaderInfo *getPipelineShaderInfo(ShaderStage shaderStage) const;

  // Checks whether the pipeline is graphics or compute
  virtual bool isGraphics() const { return false; }

  // Gets pipeline build info
  virtual const void *getPipelineBuildInfo() const { return m_pipelineInfo; }

  // Gets the mask of active shader stages bound to this pipeline
  virtual unsigned getShaderStageMask() const { return shaderStageToMask(ShaderStageCompute); }

  // Sets the mask of active shader stages bound to this pipeline
  void setShaderStageMask(unsigned mask) { assert(mask == getShaderStageMask()); }

  // Gets the count of active shader stages
  virtual unsigned getActiveShaderStageCount() const { return 1; }

  // Check whether the pipeline uses features relevant to subgroup size
  virtual bool usesSubgroupSize() const;

  // Gets per pipeline options
  virtual const PipelineOptions *getPipelineOptions() const { return &m_pipelineInfo->options; }

private:
  ComputeContext() = delete;
  ComputeContext(const ComputeContext &) = delete;
  ComputeContext &operator=(const ComputeContext &) = delete;

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 41
  void mergeResourceMappingData();
#endif

  const ComputePipelineBuildInfo *m_pipelineInfo; // Info to build a compute pipeline
};

} // namespace Llpc
