/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Timer.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "lgc-compiler"

using namespace lgc;
using namespace llvm;

namespace lgc {
// Create BuilderReplayer pass
ModulePass *createBuilderReplayer(Pipeline *pipeline);
ElfLinker *createElfLinkerImpl(PipelineState *pipelineState, llvm::ArrayRef<llvm::MemoryBufferRef> elfs);

} // namespace lgc

// =====================================================================================================================
// Mark a function as a shader entry-point. This must be done before linking shader modules into a pipeline
// with irLink(). This is a static method in Pipeline, as it does not need a Pipeline object, and can be used
// in the front-end before a shader is associated with a pipeline.
//
// @param func : Shader entry-point function
// @param stage : Shader stage
void Pipeline::markShaderEntryPoint(Function *func, ShaderStage stage) {
  // We mark the shader entry-point function by
  // 1. marking it external linkage and DLLExportStorageClass; and
  // 2. adding the shader stage metadata.
  // The shader stage metadata for any other non-inlined functions in the module is added in irLink().
  func->setLinkage(GlobalValue::ExternalLinkage);
  func->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(func, stage);
}

// =====================================================================================================================
// Link shader IR modules into a pipeline module.
//
// @param modules : Array of modules. Modules are freed
// @param unlinked : True if generating an "unlinked" half-pipeline ELF that then needs further linking to
//                   generate a pipeline ELF
Module *PipelineState::irLink(ArrayRef<Module *> modules, bool unlinked) {
  m_unlinked = unlinked;
#ifndef NDEBUG
  unsigned shaderStageMask = 0;
#endif

  // Processing for each shader module before linking.
  IRBuilder<> builder(getContext());
  for (Module *module : modules) {
    if (!module)
      continue;

    // Find the shader entry-point (marked with irLink()), and get the shader stage from that.
    // Default to compute to handle the case of a compute library, which does not have a shader entry-point.
    ShaderStage stage = ShaderStageCompute;
    for (Function &func : *module) {
      if (!isShaderEntryPoint(&func))
        continue;
      // We have the entry-point (marked as DLLExportStorageClass).
      stage = getShaderStage(&func);
#ifndef NDEBUG
      assert((shaderStageMask & (1 << stage)) == 0);
      shaderStageMask |= 1 << stage;
#endif
      // Rename the entry-point to ensure there is no clash on linking.
      func.setName(Twine(lgcName::EntryPointPrefix) + getShaderStageAbbreviation(static_cast<ShaderStage>(stage)) +
                   "." + func.getName());
    }

    // Mark all other function definitions in the module with the same shader stage.
    for (Function &func : *module) {
      if (!func.isDeclaration() && !isShaderEntryPoint(&func))
        setShaderStage(&func, stage);
    }
  }

#ifndef NDEBUG
  // Assert that the front-end's call to setShaderStageMask was correct. (We want the front-end to call it
  // before calling any builder calls in case it is using direct BuilderImpl and one of the builder calls needs
  // the shader stage mask.)
  assert(shaderStageMask == getShaderStageMask());
#endif

  // If the front-end was using a BuilderRecorder, record pipeline state into IR metadata.
  if (!m_noReplayer)
    record(modules[0]);

  // If there is only one shader, just change the name on its module and return it.
  Module *pipelineModule = nullptr;
  if (modules.size() == 1) {
    pipelineModule = modules[0];
    pipelineModule->setModuleIdentifier("lgcPipeline");
  } else {
    // Create an empty module then link each shader module into it. We record pipeline state into IR
    // metadata before the link, to avoid problems with a Constant for an immutable descriptor value
    // disappearing when modules are deleted.
    bool result = true;
    pipelineModule = new Module("lgcPipeline", getContext());
    TargetMachine *targetMachine = getLgcContext()->getTargetMachine();
    pipelineModule->setTargetTriple(targetMachine->getTargetTriple().getTriple());
    pipelineModule->setDataLayout(targetMachine->createDataLayout());

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
// @param otherElf : Optional ELF for the other half-pipeline when compiling an unlinked half-pipeline ELF.
//                   Supplying this could allow more optimal code for writing/reading attribute values between
//                   the two pipeline halves
// @returns : True for success.
//           False if irLink asked for an "unlinked" shader or half-pipeline, and there is some reason why the
//           module cannot be compiled that way.  The client typically then does a whole-pipeline compilation
//           instead. The client can call getLastError() to get a textual representation of the error, for
//           use in logging or in error reporting in a command-line utility.
bool PipelineState::generate(std::unique_ptr<Module> pipelineModule, raw_pwrite_stream &outStream,
                             Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, ArrayRef<Timer *> timers,
                             MemoryBufferRef otherElf) {
  assert(otherElf.getBuffer().empty() && "otherElf not supported yet");

  m_lastError.clear();
  unsigned passIndex = 1000;
  Timer *patchTimer = timers.size() >= 1 ? timers[0] : nullptr;
  Timer *optTimer = timers.size() >= 2 ? timers[1] : nullptr;
  Timer *codeGenTimer = timers.size() >= 3 ? timers[2] : nullptr;

  // Set up "whole pipeline" passes, where we have a single module representing the whole pipeline.
  std::unique_ptr<PassManager> passMgr(PassManager::Create());
  passMgr->setPassIndex(&passIndex);
  passMgr->add(createTargetTransformInfoWrapperPass(getLgcContext()->getTargetMachine()->getTargetIRAnalysis()));

  // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
  getLgcContext()->preparePassManager(&*passMgr);

  // Manually add a PipelineStateWrapper pass.
  // If we were not using BuilderRecorder, give our PipelineState to it. (In the BuilderRecorder case,
  // the first time PipelineStateWrapper is used, it allocates its own PipelineState and populates
  // it by reading IR metadata.)
  PipelineStateWrapper *pipelineStateWrapper = new PipelineStateWrapper(getLgcContext());
  passMgr->add(pipelineStateWrapper);
  if (m_noReplayer)
    pipelineStateWrapper->setPipelineState(this);

  if (m_emitLgc) {
    // -emit-lgc: Just write the module.
    passMgr->add(createPrintModulePass(outStream));
    passMgr->stop();
  }

  // Get a BuilderReplayer pass if needed.
  ModulePass *replayerPass = nullptr;
  if (!m_noReplayer)
    replayerPass = createBuilderReplayer(this);

  // Patching.
  Patch::addPasses(this, *passMgr, replayerPass, patchTimer, optTimer, checkShaderCacheFunc);

  // Add pass to clear pipeline state from IR
  passMgr->add(createPipelineStateClearer());

  // Code generation.
  getLgcContext()->addTargetPasses(*passMgr, codeGenTimer, outStream);

  // Run the "whole pipeline" passes.
  passMgr->run(*pipelineModule);

  // See if there was a recoverable error.
  if (getLastError() != "")
    return false;

  return true;
}

// =====================================================================================================================
// Create an ELF linker object for linking unlinked half-pipeline ELFs into a pipeline ELF using the pipeline state.
// This needs to be deleted after use.
ElfLinker *PipelineState::createElfLinker(llvm::ArrayRef<llvm::MemoryBufferRef> elfs) {
  return createElfLinkerImpl(this, elfs);
}

// =====================================================================================================================
// Do an early check for ability to use shader/half-pipeline compilation then ELF linking.
// Intended to be used when doing shader/half-pipeline compilation with pipeline state already available.
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
