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
 * @file  llpcContext.h
 * @brief LLPC header file: contains declaration of class Llpc::Context.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPipelineContext.h"
#include "spirvExt.h"
#include "lgc/LgcContext.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Target/TargetMachine.h"
#include <unordered_map>
#include <unordered_set>

namespace llvm_dialects {
class DialectContext;
}

namespace Llpc {

// =====================================================================================================================
// Represents LLPC context for pipeline compilation. Derived from the base class llvm::LLVMContext.
class Context : public llvm::LLVMContext {
public:
  Context(GfxIpVersion gfxIp);
  ~Context();

  void reset();

  // Checks whether this context is in use.
  bool isInUse() const { return m_isInUse; }

  // Set context in-use flag.
  void setInUse(bool inUse) {
    if (!m_isInUse && inUse) {
      ++m_useCount;
    }
    m_isInUse = inUse;
  }

  // Get the number of times this context is used.
  unsigned getUseCount() const { return m_useCount; }

  // Attaches pipeline context to LLPC context.
  void attachPipelineContext(PipelineContext *pipelineContext) { m_pipelineContext = pipelineContext; }

  // Gets pipeline context.
  PipelineContext *getPipelineContext() const { return m_pipelineContext; }

  // Set LLPC builder
  void setBuilder(lgc::Builder *builder) { m_builder = builder; }

  // Get LLPC builder
  lgc::Builder *getBuilder() const { return m_builder; }

  // Get (create if necessary) LgcContext
  lgc::LgcContext *getLgcContext();

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 474768
  // Old version of the code
  llvm::CodeGenOpt::Level getOptimizationLevel();
  llvm::CodeGenOpt::Level getLastOptimizationLevel() const;
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  llvm::CodeGenOptLevel getOptimizationLevel();
  llvm::CodeGenOptLevel getLastOptimizationLevel() const;
#endif

  std::unique_ptr<llvm::Module> loadLibrary(const BinaryData *lib);

  // Wrappers of interfaces of pipeline context
  PipelineType getPipelineType() const { return m_pipelineContext->getPipelineType(); }

  const ResourceMappingData *getResourceMapping() const { return m_pipelineContext->getResourceMapping(); }

  const uint64_t getPipelineLayoutApiHash() const { return m_pipelineContext->getPipelineLayoutApiHash(); }

  const void *getPipelineBuildInfo() const { return m_pipelineContext->getPipelineBuildInfo(); }

  unsigned getShaderStageMask() const { return m_pipelineContext->getShaderStageMask(); }

  unsigned getActiveShaderStageCount() const { return m_pipelineContext->getActiveShaderStageCount(); }

  const char *getGpuNameAbbreviation() const { return PipelineContext::getGpuNameAbbreviation(m_gfxIp); }

  GfxIpVersion getGfxIpVersion() const { return m_gfxIp; }

  uint64_t getPipelineHashCode() const { return m_pipelineContext->getPipelineHashCode(); }

  uint64_t get64BitCacheHashCode() const { return m_pipelineContext->get64BitCacheHashCode(); }

  ShaderHash getShaderHashCode(const PipelineShaderInfo &shaderInfo) const {
    return m_pipelineContext->getShaderHashCode(shaderInfo);
  }

  // Sets triple and data layout in specified module from the context's target machine.
  void setModuleTargetMachine(llvm::Module *module);

private:
  Context() = delete;
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  GfxIpVersion m_gfxIp;                                 // Graphics IP version info
  PipelineContext *m_pipelineContext;                   // Pipeline-specific context
  bool m_isInUse = false;                               // Whether this context is in use
  lgc::Builder *m_builder = nullptr;                    // LLPC builder object
  std::unique_ptr<llvm::TargetMachine> m_targetMachine; // Target machine for LGC context
  std::unique_ptr<lgc::LgcContext> m_builderContext;    // LGC context

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 474768
  // Old version of the code
  std::optional<llvm::CodeGenOpt::Level> m_lastOptLevel{}; // What getOptimizationLevel() last returned
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  std::optional<llvm::CodeGenOptLevel> m_lastOptLevel{}; // What getOptimizationLevel() last returned
#endif

  std::unique_ptr<llvm_dialects::DialectContext> m_dialectContext;

  unsigned m_useCount = 0; // Number of times this context is used.
};

} // namespace Llpc
