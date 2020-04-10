/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderContext.cpp
 * @brief LLPC source file: implementation of llpc::BuilderContext class for creating and using lgc::Builder
 ***********************************************************************************************************************
 */
#include "lgc/llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"
#include "llpcPatch.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"
#include "lgc/llpcBuilder.h"
#include "lgc/llpcPassManager.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"

using namespace lgc;
using namespace llvm;

static codegen::RegisterCodeGenFlags CGF;

#ifndef NDEBUG
static bool Initialized;
#endif

raw_ostream *BuilderContext::m_llpcOuts;

// -emit-llvm: emit LLVM assembly instead of ISA
static cl::opt<bool> EmitLlvm("emit-llvm", cl::desc("Emit LLVM assembly instead of AMD GPU ISA"), cl::init(false));

// -emit-llvm-bc: emit LLVM bitcode instead of ISA
static cl::opt<bool> EmitLlvmBc("emit-llvm-bc", cl::desc("Emit LLVM bitcode instead of AMD GPU ISA"), cl::init(false));

// =====================================================================================================================
// Initialize the middle-end. This must be called before the first BuilderContext::Create, although you are
// allowed to call it again after that. It must also be called before LLVM command-line processing, so
// that you can use a pass name in an option such as -print-after. If multiple concurrent compiles are
// possible, this should be called in a thread-safe way.
void BuilderContext::initialize() {
#ifndef NDEBUG
  Initialized = true;
#endif

  auto &passRegistry = *PassRegistry::getPassRegistry();

  // Initialize LLVM target: AMDGPU
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUAsmPrinter();
  LLVMInitializeAMDGPUAsmParser();
  LLVMInitializeAMDGPUDisassembler();

  // Initialize core LLVM passes so they can be referenced by -stop-before etc.
  initializeCore(passRegistry);
  initializeTransformUtils(passRegistry);
  initializeScalarOpts(passRegistry);
  initializeVectorization(passRegistry);
  initializeInstCombine(passRegistry);
  initializeAggressiveInstCombine(passRegistry);
  initializeIPO(passRegistry);
  initializeCodeGen(passRegistry);
  initializeShadowStackGCLoweringPass(passRegistry);
  initializeExpandReductionsPass(passRegistry);
  initializeRewriteSymbolsLegacyPassPass(passRegistry);

  // Initialize LGC passes so they can be referenced by -stop-before etc.
  initializeUtilPasses(passRegistry);
  initializeBuilderReplayerPass(passRegistry);
  initializePatchPasses(passRegistry);
}

// =====================================================================================================================
// Create the BuilderContext. Returns nullptr on failure to recognize the AMDGPU target whose name is specified
//
// @param context : LLVM context to give each Builder
// @param gpuName : LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
// @param palAbiVersion : PAL pipeline ABI version to compile for
BuilderContext *BuilderContext::Create(LLVMContext &context, StringRef gpuName, unsigned palAbiVersion) {
  assert(Initialized && "Must call BuilderContext::Initialize before BuilderContext::Create");

  BuilderContext *builderContext = new BuilderContext(context, palAbiVersion);

  std::string mcpuName = codegen::getMCPU(); // -mcpu setting from llvm/CodeGen/CommandFlags.h
  if (gpuName == "")
    gpuName = mcpuName;

  builderContext->m_targetInfo = new TargetInfo;
  if (!builderContext->m_targetInfo->setTargetInfo(gpuName)) {
    delete builderContext;
    return nullptr;
  }

  // Get the LLVM target and create the target machine. This should not fail, as we determined above
  // that we support the requested target.
  const std::string triple = "amdgcn--amdpal";
  std::string errMsg;
  const Target *target = TargetRegistry::lookupTarget(triple, errMsg);
  // Allow no signed zeros - this enables omod modifiers (div:2, mul:2)
  TargetOptions targetOpts;
  targetOpts.NoSignedZerosFPMath = true;

  builderContext->m_targetMachine =
      target->createTargetMachine(triple, gpuName, "", targetOpts, Optional<Reloc::Model>());
  assert(builderContext->m_targetMachine);
  return builderContext;
}

// =====================================================================================================================
//
// @param context : LLVM context to give each Builder
// @param palAbiVersion : PAL pipeline ABI version to compile for
BuilderContext::BuilderContext(LLVMContext &context, unsigned palAbiVersion) : m_context(context) {}

// =====================================================================================================================
BuilderContext::~BuilderContext() {
  delete m_targetMachine;
  delete m_targetInfo;
}

// =====================================================================================================================
// Create a Pipeline object for a pipeline compile.
// This actually creates a PipelineState, but returns the Pipeline superclass that is visible to
// the front-end.
Pipeline *BuilderContext::createPipeline() { return new PipelineState(this); }

// =====================================================================================================================
// Create a Builder object. For a shader compile (pPipeline is nullptr), useBuilderRecorder is ignored
// because it always uses BuilderRecorder.
//
// @param pipeline : Pipeline object for pipeline compile, nullptr for shader compile
// @param useBuilderRecorder : true to use BuilderRecorder, false to use BuilderImpl
Builder *BuilderContext::createBuilder(Pipeline *pipeline, bool useBuilderRecorder) {
  if (!pipeline || useBuilderRecorder)
    return new BuilderRecorder(this, pipeline);
  return new BuilderImpl(this, pipeline);
}

// =====================================================================================================================
// Prepare a pass manager. This manually adds a target-aware TLI pass, so middle-end optimizations do not think that
// we have library functions.
//
// @param [in/out] passMgr : Pass manager
void BuilderContext::preparePassManager(legacy::PassManager *passMgr) {
  TargetLibraryInfoImpl targetLibInfo(getTargetMachine()->getTargetTriple());

  // Adjust it to allow memcpy and memset.
  // TODO: Investigate why the latter is necessary. I found that
  // test/shaderdb/ObjStorageBlock_TestMemCpyInt32.comp
  // got unrolled far too much, and at too late a stage for the descriptor loads to be commoned up. It might
  // be an unfortunate interaction between LoopIdiomRecognize and fat pointer laundering.
  targetLibInfo.setAvailable(LibFunc_memcpy);
  targetLibInfo.setAvailable(LibFunc_memset);

  // Also disallow tan functions.
  // TODO: This can be removed once we have LLVM fix D67406.
  targetLibInfo.setUnavailable(LibFunc_tan);
  targetLibInfo.setUnavailable(LibFunc_tanf);
  targetLibInfo.setUnavailable(LibFunc_tanl);

  auto targetLibInfoPass = new TargetLibraryInfoWrapperPass(targetLibInfo);
  passMgr->add(targetLibInfoPass);
}

// =====================================================================================================================
// Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
//
// @param [in/out] passMgr : pass manager to add passes to
// @param codeGenTimer : Timer to time target passes with, nullptr if not timing
// @param [out] outStream : Output stream
void BuilderContext::addTargetPasses(lgc::PassManager &passMgr, Timer *codeGenTimer, raw_pwrite_stream &outStream) {
  // Start timer for codegen passes.
  if (codeGenTimer)
    passMgr.add(createStartStopTimer(codeGenTimer, true));

  // Dump the module just before codegen.
  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.add(
        createPrintModulePass(*outs, "===============================================================================\n"
                                     "// LLPC final pipeline module info\n"));
  }

  if (EmitLlvm && EmitLlvmBc)
    report_fatal_error("-emit-llvm conflicts with -emit-llvm-bc");

  if (EmitLlvm) {
    // For -emit-llvm, add a pass to output the LLVM IR, then tell the pass manager to stop adding
    // passes. We do it this way to ensure that we still get the immutable passes from
    // TargetMachine::addPassesToEmitFile, as they can affect LLVM middle-end optimizations.
    passMgr.add(createPrintModulePass(outStream));
    passMgr.stop();
  }

  if (EmitLlvmBc) {
    // For -emit-llvm-bc, add a pass to output the LLVM IR, then tell the pass manager to stop adding
    // passes. We do it this way to ensure that we still get the immutable passes from
    // TargetMachine::addPassesToEmitFile, as they can affect LLVM middle-end optimizations.
    passMgr.add(createBitcodeWriterPass(outStream));
    passMgr.stop();
  }

  // TODO: We should probably be using InitTargetOptionsFromCodeGenFlags() here.
  // Currently we are not, and it would give an "unused function" warning when compiled with
  // CLANG. So we avoid the warning by referencing it here.
  (void(&codegen::InitTargetOptionsFromCodeGenFlags)); // unused

  if (getTargetMachine()->addPassesToEmitFile(passMgr, outStream, nullptr, codegen::getFileType()))
    report_fatal_error("Target machine cannot emit a file of this type");

  // Stop timer for codegen passes.
  if (codeGenTimer)
    passMgr.add(createStartStopTimer(codeGenTimer, false));
}
