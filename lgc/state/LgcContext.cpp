/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  LgcContext.cpp
 * @brief LLPC source file: implementation of llpc::LgcContext class for creating and using lgc::Builder
 ***********************************************************************************************************************
 */
#include "lgc/LgcContext.h"
#include "lgc/Builder.h"
#include "lgc/LgcDialect.h"
#include "lgc/PassManager.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PassManagerCache.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Debug.h"
#include "lgc/util/Internal.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#define DEBUG_TYPE "lgc-context"

using namespace lgc;
using namespace llvm;

static codegen::RegisterCodeGenFlags CGF;

#ifndef NDEBUG
static bool Initialized;
#endif

thread_local raw_ostream *LgcContext::m_llpcOuts;

// -emit-llvm: emit LLVM assembly instead of ISA
static cl::opt<bool> EmitLlvm("emit-llvm", cl::desc("Emit LLVM assembly instead of AMD GPU ISA"), cl::init(false));

// -emit-llvm-bc: emit LLVM bitcode instead of ISA
static cl::opt<bool> EmitLlvmBc("emit-llvm-bc", cl::desc("Emit LLVM bitcode instead of AMD GPU ISA"), cl::init(false));

// -emit-lgc: emit LLVM assembly suitable for input to LGC (middle-end compiler)
static cl::opt<bool> EmitLgc("emit-lgc", cl::desc("Emit LLVM assembly suitable for input to LGC (middle-end compiler)"),
                             cl::init(false));

// -show-encoding: show the instruction encoding when emitting assembler. This mirrors llvm-mc behaviour
static cl::opt<bool> ShowEncoding("show-encoding", cl::desc("Show instruction encodings"), cl::init(false));

// -opt: Override the optimization level passed in to LGC with the given one.
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 474768
// Old version of the code
static cl::opt<CodeGenOpt::Level> OptLevel("opt", cl::desc("Set the optimization level for LGC:"),
                                           cl::init(CodeGenOpt::Default),
                                           values(clEnumValN(CodeGenOpt::None, "none", "no optimizations"),
                                                  clEnumValN(CodeGenOpt::Less, "quick", "quick compilation time"),
                                                  clEnumValN(CodeGenOpt::Default, "default", "default optimizations"),
                                                  clEnumValN(CodeGenOpt::Aggressive, "fast", "fast execution time")));
#else
// New version of the code (also handles unknown version, which we treat as latest)
static cl::opt<CodeGenOptLevel>
    OptLevel("opt", cl::desc("Set the optimization level for LGC:"), cl::init(CodeGenOptLevel::Default),
             values(clEnumValN(CodeGenOptLevel::None, "none", "no optimizations"),
                    clEnumValN(CodeGenOptLevel::Less, "quick", "quick compilation time"),
                    clEnumValN(CodeGenOptLevel::Default, "default", "default optimizations"),
                    clEnumValN(CodeGenOptLevel::Aggressive, "fast", "fast execution time")));
#endif

// =====================================================================================================================
// Set default for a command-line option, but only if command-line processing has not happened yet, or did not see
// an occurrence of this option.
//
// @param name : Option name
// @param value : Default option value
static void setOptionDefault(const char *name, StringRef value) {
  auto optIterator = cl::getRegisteredOptions().find(name);
  assert(optIterator != cl::getRegisteredOptions().end() && "Failed to find option to set default");
  cl::Option *opt = optIterator->second;
  if (opt->getNumOccurrences())
    return;
  // Setting MultiArg means that addOccurrence will not increment the option's occurrence count, so the user
  // can still specify it to override our default here.
  bool setFailed = opt->addOccurrence(0, opt->ArgStr, value, /*MultiArg=*/true);
  assert(!setFailed && "Failed to set default for option");
  ((void)setFailed);
}

// =====================================================================================================================
// Initialize the middle-end. This must be called before the first LgcContext::createTargetMachine or
// LgcContext::create, although you are allowed to call it again after that. It must also be called before
// LLVM command-line processing, so that you can use a pass name in an option such as -print-after. If multiple
// concurrent compiles are possible, this should be called in a thread-safe way.
void LgcContext::initialize() {
#ifndef NDEBUG
  Initialized = true;
#endif

  // Initialize LLVM target: AMDGPU
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTarget();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUAsmPrinter();
  LLVMInitializeAMDGPUAsmParser();
  LLVMInitializeAMDGPUDisassembler();

  auto &passRegistry = *PassRegistry::getPassRegistry();

  // Initialize core LLVM passes so they can be referenced by -stop-before etc.
  initializeCore(passRegistry);
  initializeTransformUtils(passRegistry);
  initializeScalarOpts(passRegistry);
  initializeVectorization(passRegistry);
  initializeInstCombine(passRegistry);
  initializeIPO(passRegistry);
  initializeCodeGen(passRegistry);
  initializeShadowStackGCLoweringPass(passRegistry);
  initializeExpandReductionsPass(passRegistry);

  // Initialize LGC passes so they can be referenced by -stop-before etc.
  initializeUtilPasses(passRegistry);

  // Initialize some command-line option defaults.
  setOptionDefault("filetype", "obj");
  setOptionDefault("amdgpu-unroll-max-block-to-analyze", "20");
  setOptionDefault("unroll-max-percent-threshold-boost", "1000");
  setOptionDefault("unroll-allow-partial", "1");
  // TODO: phi-of-ops optimization in NewGVN has some problems, we temporarily
  // disable this to avoid miscompilation, see (https://github.com/GPUOpen-Drivers/llpc/issues/1206).
  setOptionDefault("enable-phi-of-ops", "0");
  setOptionDefault("amdgpu-vgpr-index-mode", "1"); // force VGPR indexing on GFX8
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 464446
  // Old version of the code
  setOptionDefault("amdgpu-atomic-optimizations", "1");
#else
  // New version of the code (also handles unknown version, which we treat as latest)
#endif
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 463788
  // Old version of the code
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  setOptionDefault("amdgpu-atomic-optimizer-strategy", "DPP");
#endif
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  setOptionDefault("use-gpu-divergence-analysis", "1");
#else
  // New version of the code (also handles unknown version, which we treat as latest)
#endif
  setOptionDefault("structurizecfg-skip-uniform-regions", "1");
  setOptionDefault("spec-exec-max-speculation-cost", "10");
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Conditional discard transformations not supported
#else
  setOptionDefault("amdgpu-conditional-discard-transformations", "1");
#endif
}

// =====================================================================================================================
// Gets the name string of GPU target according to graphics IP version info.
//
// @param major : major version
// @param minor : minor version
// @param stepping : stepping info
// @returns : LLVM GPU name as a std::string
std::string LgcContext::getGpuNameString(unsigned major, unsigned minor, unsigned stepping) {
  // A GfxIpVersion from PAL is three decimal numbers for major, minor and stepping. This function
  // converts that to an LLVM target name, with is "gfx" followed by the three decimal numbers with
  // no separators, e.g. "gfx1010" for 10.1.0. A high stepping number 0xFFFA..0xFFFF denotes an
  // experimental target, and that is represented by the final hexadecimal digit, e.g. "gfx101A"
  // for 10.1.0xFFFA. In gfx9, stepping numbers 10..35 are represented by lower case letter 'a'..'z'.
  std::string gpuName;
  raw_string_ostream gpuNameStream(gpuName);
  gpuNameStream << "gfx" << major << minor;
  if (stepping >= 0xFFFA)
    gpuNameStream << char(stepping - 0xFFFA + 'A');
  else
    gpuNameStream << stepping;

  return gpuNameStream.str();
}

// =====================================================================================================================
// Validate gpuName as a valid gpu
//
// @param gpuName : LLVM GPU name (e.g. "gfx900")
// @returns : true if gpu name is valid, false otherwise
bool LgcContext::isGpuNameValid(llvm::StringRef gpuName) {
  TargetInfo targetInfo;

  return targetInfo.setTargetInfo(gpuName);
}

// =====================================================================================================================
// Create TargetMachine. Returns nullptr on failure to recognize the AMDGPU target whose name is specified.
//
// @param gpuName : LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
// @param optLevel : LLVM optimization level used to initialize target machine
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 474768
// Old version of the code
std::unique_ptr<TargetMachine> LgcContext::createTargetMachine(StringRef gpuName, CodeGenOpt::Level optLevel)
#else
// New version of the code (also handles unknown version, which we treat as latest)
std::unique_ptr<TargetMachine> LgcContext::createTargetMachine(StringRef gpuName, CodeGenOptLevel optLevel)
#endif
{
  assert(Initialized && "Must call LgcContext::initialize before LgcContext::createTargetMachine");

  std::string mcpuName = codegen::getMCPU(); // -mcpu setting from llvm/CodeGen/CommandFlags.h
  if (gpuName == "")
    gpuName = mcpuName;
  if (!isGpuNameValid(gpuName))
    return nullptr;

  // Get the LLVM target and create the target machine. This should not fail, as we determined above
  // that we support the requested target.
  const std::string triple = "amdgcn--amdpal";
  std::string errMsg;
  const Target *target = TargetRegistry::lookupTarget(triple, errMsg);
  // Allow no signed zeros - this enables omod modifiers (div:2, mul:2)
  TargetOptions targetOpts;
  targetOpts.NoSignedZerosFPMath = true;

  // Enable instruction encoding output - outputs hex in comment mirroring
  // llvm-mc behaviour
  if (ShowEncoding) {
    targetOpts.MCOptions.ShowMCEncoding = true;
    targetOpts.MCOptions.AsmVerbose = true;
  }

  // If the "opt" option is given, set the optimization level to that value.
  if (OptLevel.getPosition() != 0)
    optLevel = OptLevel;

  LLPC_OUTS("TargetMachine optimization level = " << static_cast<uint32_t>(optLevel) << "\n");

  return std::unique_ptr<TargetMachine>(target->createTargetMachine(triple, gpuName, "", targetOpts, {}, {}, optLevel));
}

// =====================================================================================================================
// Create the LgcContext.
//
// @param targetMachine : LLVM TargetMachine to use. Caller retains ownership and must free it when finished.
// @param context : LLVM context to give each Builder. Caller retains ownership and must free it when finished.
// @param palAbiVersion : PAL pipeline ABI version to compile for
LgcContext *LgcContext::create(TargetMachine *targetMachine, LLVMContext &context, unsigned palAbiVersion) {
  assert(Initialized && "Must call LgcContext::initialize before LgcContext::create");

  LgcContext *builderContext = new LgcContext(context, palAbiVersion);
  builderContext->m_targetMachine = targetMachine;

  builderContext->m_targetInfo = new TargetInfo;
  if (!builderContext->m_targetInfo->setTargetInfo(targetMachine->getTargetCPU())) {
    delete builderContext;
    return nullptr;
  }

  return builderContext;
}

// =====================================================================================================================
// Get the value of the -emit-lgc option. BuilderRecorder uses this to decide whether to omit the opcode
// metadata when recording a Builder call.
bool LgcContext::getEmitLgc() {
  return EmitLgc;
}

// =====================================================================================================================
//
// @param context : LLVM context to give each Builder
// @param palAbiVersion : PAL pipeline ABI version to compile for
LgcContext::LgcContext(LLVMContext &context, unsigned palAbiVersion) : m_context(context) {
}

// =====================================================================================================================
LgcContext::~LgcContext() {
  delete m_targetInfo;
  delete m_passManagerCache;
}

// =====================================================================================================================
// Create a Pipeline object for a pipeline compile.
// This actually creates a PipelineState, but returns the Pipeline superclass that is visible to
// the front-end.
Pipeline *LgcContext::createPipeline() {
  return new PipelineState(this, EmitLgc);
}

// =====================================================================================================================
// Create a Builder object. This is now unnecessary, as you can just create a local variable Builder or "new" it
// yourself.
//
// @param pipeline : Ignored
Builder *LgcContext::createBuilder(Pipeline *pipeline) {
  return new Builder(getContext());
}

// =====================================================================================================================
// Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
//
// @param [in/out] passMgr : Pass manager to add passes to
// @param codeGenTimer : Timer to time target passes with, nullptr if not timing
// @param [out] outStream : Output stream
void LgcContext::addTargetPasses(lgc::LegacyPassManager &passMgr, Timer *codeGenTimer, raw_pwrite_stream &outStream) {
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

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 474768
// Old version of the code
llvm::CodeGenOpt::Level LgcContext::getOptimizationLevel() const {
#else
// New version of the code (also handles unknown version, which we treat as latest)
llvm::CodeGenOptLevel LgcContext::getOptimizationLevel() const {
#endif

  return m_targetMachine->getOptLevel();
}

// =====================================================================================================================
// Get pass manager cache
PassManagerCache *LgcContext::getPassManagerCache() {
  if (!m_passManagerCache)
    m_passManagerCache = new PassManagerCache(this);
  return m_passManagerCache;
}
