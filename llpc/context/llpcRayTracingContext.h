/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcRayTracingContext.h
 * @brief LLPC header file: contains declaration of class Llpc::RayTracingContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPipelineContext.h"
#include <set>

namespace lgc {

class Builder;
} // namespace lgc

namespace Llpc {

// =====================================================================================================================
// Represents LLPC context for ray tracing pipeline compilation. Derived from the base class Llpc::Context.
class RayTracingContext : public PipelineContext {
public:
  RayTracingContext(GfxIpVersion gfxIp, const RayTracingPipelineBuildInfo *pipelineInfo,
                    const PipelineShaderInfo *traceRayShaderInfo, MetroHash::Hash *pipelineHash,
                    MetroHash::Hash *cacheHash, unsigned indirectStageMask);
  virtual ~RayTracingContext() {}

  virtual const PipelineShaderInfo *getPipelineShaderInfo(ShaderStage shaderStage) const override;

  // Gets pipeline build info
  virtual const void *getPipelineBuildInfo() const override { return m_pipelineInfo; }

  // Gets the mask of active shader stages bound to this pipeline
  virtual unsigned getShaderStageMask() const override;

  // Sets the mask of active shader stages bound to this pipeline
  virtual void setShaderStageMask(unsigned mask) override { llvm_unreachable("Should never be called!"); }

  // Gets the count of active shader stages
  virtual unsigned getActiveShaderStageCount() const override;

  // Gets per pipeline options
  virtual const PipelineOptions *getPipelineOptions() const override { return &m_pipelineInfo->options; }

  // Gets subgroup size usage
  virtual unsigned getSubgroupSizeUsage() const override;

  // Checks whether the pipeline is ray tracing
  virtual bool isRayTracing() const override { return true; }

  // Set the raytracing shader stages inline/indirect status
  virtual void setIndirectStage(ShaderStage stage) override { m_indirectStageMask |= shaderStageToMask(stage); }

  virtual void collectPayloadSize(llvm::Type *type, const llvm::DataLayout &dataLayout) override;
  virtual void collectCallableDataSize(llvm::Type *type, const llvm::DataLayout &dataLayout) override;
  virtual void collectAttributeDataSize(llvm::Type *type, const llvm::DataLayout &dataLayout) override;
  virtual void collectBuiltIn(unsigned builtIn) override;

  // Set the Context linked state
  void setLinked(bool linked) { m_linked = linked; }

  // Get the raytracing indirect mask
  const unsigned getIndirectStageMask() const { return m_indirectStageMask; }

  // Get the shader stage module IDs
  void getStageModuleIds(ShaderStage shaderStage, unsigned intersectionShader, std::vector<unsigned> &moduleIds);

  // Get the entry function name
  llvm::StringRef getEntryName() const { return m_entryName; }

  // Set the entry function name to the context
  void setEntryName(llvm::StringRef entryName) { m_entryName = std::string(entryName); }

  static const unsigned InvalidShaderId = static_cast<unsigned>(-1);
  static const unsigned TriangleHitGroup = static_cast<unsigned>(-2);
  llvm::Type *getPayloadType(lgc::Builder *builder);
  llvm::Type *getCallableDataType(lgc::Builder *builder);
  unsigned getAttributeDataSize();
  std::set<unsigned, std::less<unsigned>> &getBuiltIns() { return m_builtIns; }
  bool getHitAttribute() { return m_attributeDataMaxSize > 0; }
  unsigned getPayloadSizeInDword() { return m_payloadMaxSize / 4; }
  bool hasPipelineLibrary() { return m_pipelineInfo->hasPipelineLibrary; }
  unsigned hasLibraryStage(unsigned stageMask) { return m_pipelineInfo->pipelineLibStageMask & stageMask; }
  bool isReplay() { return m_pipelineInfo->isReplay; }

private:
  RayTracingContext() = delete;
  RayTracingContext(const RayTracingContext &) = delete;
  RayTracingContext &operator=(const RayTracingContext &) = delete;
  bool isRayTracingBuiltIn(unsigned builtIn);

  const RayTracingPipelineBuildInfo *m_pipelineInfo;  // Info to build a ray tracing pipeline
  const PipelineShaderInfo *m_traceRayShaderInfo;     // Trace Ray shaderInfo
  bool m_linked;                                      // Whether the context is linked or not
  unsigned m_indirectStageMask;                       // Which stages enable indirect call for ray tracing
  std::string m_entryName;                            // Entry function of the raytracing module
  unsigned m_payloadMaxSize;                          // Payloads maximum size
  unsigned m_callableDataMaxSize;                     // Callable maximum size
  unsigned m_attributeDataMaxSize;                    // Attribute maximum size
  std::set<unsigned, std::less<unsigned>> m_builtIns; // Collected raytracing
};

} // namespace Llpc
