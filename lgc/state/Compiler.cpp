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
} // namespace lgc

// =====================================================================================================================
// Link shader IR modules into a pipeline module.
//
// @param modules : Array of {module, shaderStage} pairs. Modules are freed
Module *PipelineState::irLink(ArrayRef<std::pair<Module *, ShaderStage>> modules) {
  // Processing for each shader module before linking.
  IRBuilder<> builder(getContext());
  for (auto moduleAndStage : modules) {
    Module *module = moduleAndStage.first;
    ShaderStage stage = moduleAndStage.second;
    if (!module)
      continue;

    // If this is a link of shader modules from earlier separate shader compiles, then the modes are
    // recorded in IR metadata. Read the modes here.
    getShaderModes()->readModesFromShader(module, stage);

    // Add IR metadata for the shader stage to each function in the shader, and rename the entrypoint to
    // ensure there is no clash on linking.
    setShaderStage(module, stage);
    for (Function &func : *module) {
      if (!func.isDeclaration() && func.getLinkage() != GlobalValue::InternalLinkage) {
        func.setName(Twine(lgcName::EntryPointPrefix) + getShaderStageAbbreviation(static_cast<ShaderStage>(stage)) +
                     "." + func.getName());
      }
    }
  }

#ifndef NDEBUG
  // Assert that the front-end's call to setShaderStageMask was correct. (We want the front-end to call it
  // before calling any builder calls in case it is using direct BuilderImpl and one of the builder calls needs
  // the shader stage mask.)
  unsigned shaderStageMask = 0;
  for (auto moduleAndStage : modules)
    shaderStageMask |= 1 << moduleAndStage.second;
  assert(shaderStageMask == getShaderStageMask());
#endif

  // If the front-end was using a BuilderRecorder, record pipeline state into IR metadata.
  if (!m_noReplayer)
    record(modules[0].first);

  // If there is only one shader, just change the name on its module and return it.
  Module *pipelineModule = nullptr;
  if (modules.size() == 1) {
    pipelineModule = modules[0].first;
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
    for (auto moduleAndStage : modules) {
      Module *module = moduleAndStage.first;
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
// The output is normally ELF, but IR disassembly if an option is used to stop compilation early.
// Output is written to outStream.
// Like other Builder methods, on error, this calls report_fatal_error, which you can catch by setting
// a diagnostic handler with LLVMContext::setDiagnosticHandler.
//
// @param pipelineModule : IR pipeline module
// @param [in/out] outStream : Stream to write ELF or IR disassembly output
// @param checkShaderCacheFunc : Function to check shader cache in graphics pipeline
// @param timers : Optional timers for 0 or more of:
//                 timers[0]: patch passes
//                 timers[1]: LLVM optimizations
//                 timers[2]: codegen
void PipelineState::generate(std::unique_ptr<Module> pipelineModule, raw_pwrite_stream &outStream,
                             Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, ArrayRef<Timer *> timers) {
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
}
