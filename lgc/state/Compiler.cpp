/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Compiler.cpp
 * @brief LLPC source file: PipelineState methods that do IR linking and compilation
 ***********************************************************************************************************************
 */
#include "continuations/Continuations.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRPrintingPasses.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Timer.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "lgc-compiler"

using namespace lgc;
using namespace llvm;

namespace lgc {

ElfLinker *createElfLinkerImpl(PipelineState *pipelineState, llvm::ArrayRef<llvm::MemoryBufferRef> elfs);

} // namespace lgc

// =====================================================================================================================
// Mark a function as a shader entry-point. This must be done before linking shader modules into a pipeline
// with irLink(). This is a static method in Pipeline, as it does not need a Pipeline object, and can be used
// in the front-end before a shader is associated with a pipeline.
//
// @param func : Shader entry-point function
// @param stage : Shader stage or ShaderStageInvalid
void Pipeline::markShaderEntryPoint(Function *func, ShaderStage stage) {
  // We mark the shader entry-point function by
  // 1. marking it external linkage and DLLExportStorageClass; and
  // 2. adding the shader stage metadata.
  // The shader stage metadata for any other non-inlined functions in the module is added in irLink().
  if (stage != ShaderStageInvalid) {
    func->setLinkage(GlobalValue::ExternalLinkage);
    func->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  } else
    func->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  setShaderStage(func, stage);
}

// =====================================================================================================================
// Get a function's shader stage.
//
// @param func : Function to check
// @returns stage : Shader stage, or ShaderStageInvalid if none
ShaderStage Pipeline::getShaderStage(llvm::Function *func) {
  return lgc::getShaderStage(func);
}

// =====================================================================================================================
// Link shader IR modules into a pipeline module.
//
// @param modules : Array of modules. Modules are freed
// @param pipelineLink : Enum saying whether this is a pipeline, unlinked or part-pipeline compile.
Module *PipelineState::irLink(ArrayRef<Module *> modules, PipelineLink pipelineLink) {
  m_pipelineLink = pipelineLink;

  // Processing for each shader module before linking.
  IRBuilder<> builder(getContext());
  for (Module *module : modules) {
    if (!module)
      continue;

    // Find the shader entry-point (marked with irLink()), and get the shader stage from that.
    ShaderStage stage = ShaderStageInvalid;
    for (Function &func : *module) {
      if (!isShaderEntryPoint(&func))
        continue;
      // We have the entry-point (marked as DLLExportStorageClass).
      stage = getShaderStage(&func);
      m_stageMask |= 1U << stage;

      // Rename the entry-point to ensure there is no clash on linking.
      func.setName(Twine(lgcName::EntryPointPrefix) + getShaderStageAbbreviation(static_cast<ShaderStage>(stage)) +
                   "." + func.getName());
    }

    // Check if this is a compute library with no shader entry-point; if so, mark functions as compute.
    if (stage == ShaderStageInvalid) {
      stage = ShaderStageCompute;
      m_computeLibrary = true;
    }

    // Mark all other function definitions in the module with the same shader stage.
    for (Function &func : *module) {
      if (!func.isDeclaration() && !isShaderEntryPoint(&func))
        setShaderStage(&func, stage);
    }
  }

  // The front-end was using a BuilderRecorder; record pipeline state into IR metadata.
  record(modules[0]);

  // If there is only one shader, just change the name on its module and return it.
  Module *pipelineModule = nullptr;
  if (modules.size() == 1) {
    pipelineModule = modules[0];
    pipelineModule->setModuleIdentifier("lgcPipeline");
  } else {
    // Create an empty module then link each shader module into it.
    bool result = true;
    pipelineModule = new Module("lgcPipeline", getContext());
    TargetMachine *targetMachine = getLgcContext()->getTargetMachine();
    pipelineModule->setTargetTriple(targetMachine->getTargetTriple().getTriple());
    pipelineModule->setDataLayout(modules.front()->getDataLayout());

    Linker linker(*pipelineModule);
    for (Module *module : modules) {
      // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
      // linked into pipeline module.
      if (linker.linkInModule(std::unique_ptr<Module>(module)))
        result = false;
    }

    if (!result) {
      delete pipelineModule;
      pipelineModule = nullptr;
    }
  }
  return pipelineModule;
}

// =====================================================================================================================
// Version of generate() that takes ownership of the Module and deletes it.
bool PipelineState::generate(std::unique_ptr<Module> pipelineModule, raw_pwrite_stream &outStream,
                             Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, ArrayRef<Timer *> timers) {
  return generate(&*pipelineModule, outStream, checkShaderCacheFunc, timers);
}

// =====================================================================================================================
// Generate pipeline module by running patch, middle-end optimization and backend codegen passes.
// The output is normally ELF, but IR assembly if an option is used to stop compilation early,
// or ISA assembly if -filetype=asm is specified.
// Output is written to outStream.
//
// Like other LGC and LLVM library functions, an internal compiler error could cause an assert or report_fatal_error.
//
// @param pipelineModule : IR pipeline module
// @param [in/out] outStream : Stream to write ELF or IR disassembly output
// @param checkShaderCacheFunc : Function to check shader cache in graphics pipeline
// @param timers : Optional timers for 0 or more of:
//                 timers[0]: patch passes
//                 timers[1]: LLVM optimizations
//                 timers[2]: codegen
// @returns : True for success.
//           False if irLink asked for an "unlinked" shader or part-pipeline, and there is some reason why the
//           module cannot be compiled that way.  The client typically then does a whole-pipeline compilation
//           instead. The client can call getLastError() to get a textual representation of the error, for
//           use in logging or in error reporting in a command-line utility.
bool PipelineState::generate(Module *pipelineModule, raw_pwrite_stream &outStream,
                             Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, ArrayRef<Timer *> timers) {
  m_lastError.clear();

  unsigned passIndex = 1000;
  Timer *patchTimer = timers.size() >= 1 ? timers[0] : nullptr;
  Timer *optTimer = timers.size() >= 2 ? timers[1] : nullptr;
  Timer *codeGenTimer = timers.size() >= 3 ? timers[2] : nullptr;

  // Set up "whole pipeline" passes, where we have a single module representing the whole pipeline.
  std::unique_ptr<lgc::PassManager> passMgr(lgc::PassManager::Create(getLgcContext()));
  passMgr->setPassIndex(&passIndex);
  Patch::registerPasses(*passMgr);
  passMgr->registerFunctionAnalysis([&] { return getLgcContext()->getTargetMachine()->getTargetIRAnalysis(); });
  passMgr->registerModuleAnalysis([&] { return PipelineShaders(); });

  // Ensure m_stageMask is set up in this PipelineState, as Patch::addPasses uses it.
  readShaderStageMask(&*pipelineModule);

  // Manually add a PipelineStateWrapper pass.
  // We were using BuilderRecorder, so we do not give our PipelineState to it.
  // (The first time PipelineStateWrapper is used, it allocates its own PipelineState and populates
  // it by reading IR metadata.)
  passMgr->registerModuleAnalysis([&] { return PipelineStateWrapper(getLgcContext()); });

  // continuation transform require this.
  passMgr->registerModuleAnalysis([&] { return DialectContextAnalysis(false); });

  if (m_emitLgc) {
    // -emit-lgc: Just write the module.
    passMgr->addPass(PrintModulePass(outStream));
    // Run the "whole pipeline" passes.
    passMgr->run(*pipelineModule);
  } else {
    // Patching.
    Patch::addPasses(this, *passMgr, patchTimer, optTimer, std::move(checkShaderCacheFunc),
                     static_cast<uint32_t>(getLgcContext()->getOptimizationLevel()));

    // Add pass to clear pipeline state from IR
    passMgr->addPass(PipelineStateClearer());

    // Run the pipeline passes until codegen.
    passMgr->run(*pipelineModule);
    if (passMgr->stopped()) {
      outStream << *pipelineModule;
    } else {
      // Code generation.
      std::unique_ptr<LegacyPassManager> codegenPassMgr(LegacyPassManager::Create());
      unsigned passIndex = 2000;
      codegenPassMgr->setPassIndex(&passIndex);
      getLgcContext()->addTargetPasses(*codegenPassMgr, codeGenTimer, outStream);
      // Get compatible datalayout as what backend require, this is mainly used to remove entries for address space that
      // are only known to the middle-end.
      pipelineModule->setDataLayout(getLgcContext()->getTargetMachine()->createDataLayout());
      codegenPassMgr->run(*pipelineModule);
    }
  }

  // See if there was a recoverable error.
  return getLastError() == "";
}

// =====================================================================================================================
// Create an ELF linker object for linking unlinked shader/part-pipeline ELFs into a pipeline ELF using the pipeline
// state. This needs to be deleted after use.
ElfLinker *PipelineState::createElfLinker(llvm::ArrayRef<llvm::MemoryBufferRef> elfs) {
  return createElfLinkerImpl(this, elfs);
}

// =====================================================================================================================
// Do an early check for ability to use unlinked shader compilation then ELF linking.
// Intended to be used when doing unlinked shader compilation with pipeline state already available.
// It gives an early indication that there is something in the pipeline state (such as compact buffer
// descriptors) that stops ELF linking working. It does not necessarily spot all such conditions, but
// it can be useful in avoiding an unnecessary shader compile before falling back to full-pipeline
// compilation.
//
// @returns : True for success, false if some reason for failure found, in which case getLastError()
//           returns a textual description
bool PipelineState::checkElfLinkable() {
  return true;
}

// =====================================================================================================================
// Set error message to be returned to the client by it calling getLastError
//
// @param message : Error message to store
void PipelineState::setError(const Twine &message) {
  m_lastError = message.str();
}

// =====================================================================================================================
// Get a textual error message for the last recoverable error caused by generate() or one of the ElfLinker
// methods finding something about the shaders or pipeline state that means that shader compilation then
// linking cannot be done. This error message is intended only for logging or command-line error reporting.
//
// @returns : Error message from last such recoverable error; remains valid until next time generate() or
//           one of the ElfLinker methods is called, or the Pipeline object is destroyed
StringRef PipelineState::getLastError() {
  return m_lastError;
}
