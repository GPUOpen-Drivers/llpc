/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Context.
 ***********************************************************************************************************************
 */
#include "llpcContext.h"
#include "LowerAccessChain.h"
#include "LowerAdvancedBlend.h"
#include "LowerCfgMerges.h"
#include "LowerGlobals.h"
#include "LowerTranslator.h"
#include "Lowering.h"
#include "ProcessGfxRuntimeLibrary.h"
#include "ProcessGpuRtLibrary.h"
#include "SPIRVInternal.h"
#include "llpcCompiler.h"
#include "llpcDebug.h"
#include "llpcDialect.h"
#include "llpcPipelineContext.h"
#include "llpcRayTracingContext.h"
#include "llpcTimerProfiler.h"
#include "vkgcMetroHash.h"
#include "gfxruntime/GfxRuntimeLibrary.h"
#include "llvmraytracing/ContinuationsDialect.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/LgcRtqDialect.h"
#include "lgc/LgcXdlDialect.h"
#include "lgc/PassManager.h"
#include "lgc/RuntimeContext.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IRPrinter/IRPrintingPasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "llpc-context"

using namespace lgc;
using namespace lgc::rt;
using namespace lgc::rtq;
using namespace llvm;
using namespace lgc::cps;
using namespace lgc::ilcps;
using namespace lgc::xdl;

namespace Llpc {

// =====================================================================================================================
//
// @param gfxIp : Graphics IP version info
Context::Context(GfxIpVersion gfxIp) : LLVMContext(), m_gfxIp(gfxIp) {
  m_dialectContext = llvm_dialects::DialectContext::make<LgcDialect, GpurtDialect, LgcRtDialect, LgcRtqDialect,
                                                         LgcCpsDialect, LgcIlCpsDialect, LlpcDialect, LgcXdlDialect,
                                                         continuations::ContinuationsDialect>(*this);

  reset();
}

// =====================================================================================================================
void Context::reset() {
  m_pipelineContext = nullptr;
  delete m_builder;
  m_builder = nullptr;
}

// =====================================================================================================================
// Get (create if necessary) LgcContext
LgcContext *Context::getLgcContext() {
  // Create the LgcContext on first execution or optimization level change.
  if (!m_builderContext || getLastOptimizationLevel() != getOptimizationLevel()) {
    std::string gpuName = LgcContext::getGpuNameString(m_gfxIp.major, m_gfxIp.minor, m_gfxIp.stepping);
    // Pass the state of LLPC_OUTS on to LGC for the logging inside createTargetMachine.
    LgcContext::setLlpcOuts(EnableOuts() ? &outs() : nullptr);
    m_targetMachine = LgcContext::createTargetMachine(gpuName, getOptimizationLevel());
    LgcContext::setLlpcOuts(nullptr);
    if (!m_targetMachine)
      report_fatal_error(Twine("Unknown target '") + Twine(gpuName) + Twine("'"));
    m_builderContext.reset(LgcContext::create(&*m_targetMachine, *this, PAL_CLIENT_INTERFACE_MAJOR_VERSION));
    lgc::GpurtContext::get(*this).theModule = nullptr;
    lgc::GpurtContext::get(*this).ownedTheModule.reset();
    lgc::GfxRuntimeContext::get(*this).theModule.reset();
    // Pass the state of LLPC_OUTS on to LGC.
    LgcContext::setLlpcOuts(EnableOuts() ? &outs() : nullptr);
  }
  return &*m_builderContext;
}

// =====================================================================================================================
// Get optimization level. Also resets what getLastOptimizationLevel() returns.
//
// @returns: the optimization level for the context.
CodeGenOptLevel Context::getOptimizationLevel() {
  uint32_t optLevel = static_cast<uint32_t>(CodeGenOptLevel::Default);

  optLevel = getPipelineContext()->getPipelineOptions()->optimizationLevel;
  if (optLevel > 3)
    optLevel = 3;
  else if (optLevel == 0) // Workaround for noopt bugs in the AMDGPU backend in LLVM.
    optLevel = 1;
  m_lastOptLevel = CodeGenOptLevel(optLevel);
  return *m_lastOptLevel;
}

// =====================================================================================================================
// Get the optimization level returned by the last getOptimizationLevel().
CodeGenOptLevel Context::getLastOptimizationLevel() const {
  return *m_lastOptLevel;
}

// =====================================================================================================================
// Loads library from external LLVM library.
//
// @param lib : Bitcodes of external LLVM library
std::unique_ptr<Module> Context::loadLibrary(const BinaryData *lib) {
  auto memBuffer =
      MemoryBuffer::getMemBuffer(StringRef(static_cast<const char *>(lib->pCode), lib->codeSize), "", false);

  Expected<std::unique_ptr<Module>> moduleOrErr = getLazyBitcodeModule(memBuffer->getMemBufferRef(), *this);

  std::unique_ptr<Module> libModule = nullptr;
  if (!moduleOrErr) {
    Error error = moduleOrErr.takeError();
    LLPC_ERRS("Fails to load LLVM bitcode \n");
  } else {
    libModule = std::move(*moduleOrErr);
    if (Error errCode = libModule->materializeAll()) {
      LLPC_ERRS("Fails to materialize \n");
      libModule = nullptr;
    }
  }

  return libModule;
}

// =====================================================================================================================
// Sets triple and data layout in specified module from the context's target machine.
//
// @param [in/out] module : Module to modify
void Context::setModuleTargetMachine(Module *module) {
  TargetMachine *targetMachine = getLgcContext()->getTargetMachine();
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 529559
  module->setTargetTriple(targetMachine->getTargetTriple().getTriple());
#else
  module->setTargetTriple(targetMachine->getTargetTriple());
#endif
  std::string dataLayoutStr = targetMachine->createDataLayout().getStringRepresentation();
  // continuation stack address space.
  dataLayoutStr = dataLayoutStr + "-p" + std::to_string(cps::stackAddrSpace) + ":32:32";
  // SPIRV address spaces.
  dataLayoutStr = dataLayoutStr + "-p" + std::to_string(SPIRAS_Input) + ":32:32";
  dataLayoutStr = dataLayoutStr + "-p" + std::to_string(SPIRAS_Output) + ":32:32";
  module->setDataLayout(dataLayoutStr);
}

// =====================================================================================================================
// Compute the GPURT key for the current pipeline context.
GpurtKey Context::buildGpurtKey() {
  // Check whether we already have a GPURT library module that can be used
  const Vkgc::RtState *rtState = getPipelineContext()->getRayTracingState();
  GpurtKey key = {};
  key.rtipVersion = rtState->rtIpVersion;
  key.gpurtFeatureFlags = rtState->gpurtFeatureFlags; // gpurtFeatureFlags affect which GPURT library we're using
  key.bvhResDesc.resize(rtState->bvhResDesc.dataSizeInDwords);
  std::copy(rtState->bvhResDesc.descriptorData,
            rtState->bvhResDesc.descriptorData + rtState->bvhResDesc.dataSizeInDwords, key.bvhResDesc.begin());

  if (getPipelineType() == PipelineType::RayTracing) {
    auto &rtContext = *static_cast<RayTracingContext *>(getPipelineContext());
    const auto &rtPipelineBuildInfo = *rtContext.getRayTracingPipelineBuildInfo();
    key.rtPipeline.valid = true;
    key.rtPipeline.cpsFlags = rtPipelineBuildInfo.cpsFlags;
  }

  assert((rtState->gpurtOptionCount == 0) || (rtState->pGpurtOptions != nullptr));

  key.rtPipeline.options.resize(rtState->gpurtOptionCount);
  std::copy(rtState->pGpurtOptions, rtState->pGpurtOptions + rtState->gpurtOptionCount, key.rtPipeline.options.begin());

  // Use a stable sort so that if an option is supplied multiple times, the last occurrence is guaranteed to win.
  llvm::stable_sort(key.rtPipeline.options, [](const Vkgc::GpurtOption &lhs, const Vkgc::GpurtOption &rhs) {
    return lhs.nameHash < rhs.nameHash;
  });

  return key;
}

// =====================================================================================================================
// Ensure that a compatible GPURT library module is attached to this context via GpurtContext.
void Context::ensureGpurtLibrary() {
  const Vkgc::RtState *rtState = getPipelineContext()->getRayTracingState();
  auto &gpurtContext = lgc::GpurtContext::get(*this);

  {
    GpurtKey key = buildGpurtKey();

    if (gpurtContext.ownedTheModule && !m_currentGpurtKey.refines(key)) {
      gpurtContext.theModule = nullptr;
      gpurtContext.ownedTheModule.reset();
    }

    if (gpurtContext.theModule)
      return;

    // Create the GPURT library module
    m_currentGpurtKey = std::move(key);
  }

  ShaderModuleData moduleData = {};
  moduleData.binCode = rtState->gpurtShaderLibrary;
  if (moduleData.binCode.codeSize == 0)
    report_fatal_error("No GPURT library available");
  moduleData.binType = BinaryType::Spirv;
  moduleData.usage.keepUnusedFunctions = true;
  moduleData.usage.rayQueryLibrary = true;
  moduleData.usage.enableRayQuery = true;

  PipelineShaderInfo shaderInfo = {};
  shaderInfo.entryStage = ShaderStageCompute;
  shaderInfo.pModuleData = &moduleData;

  // Disable fast math contract on OpDot when there is no hardware intersectRay
  bool hwIntersectRay = !m_currentGpurtKey.bvhResDesc.empty();
  shaderInfo.options.noContractOpDot = !hwIntersectRay;

  auto gpurt = std::make_unique<Module>("_cs_", *this);
  setModuleTargetMachine(gpurt.get());

  TimerProfiler timerProfiler(getPipelineHashCode(), "LLPC GPURT", TimerProfiler::PipelineTimerEnableMask);
  std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(getLgcContext()));
  Lowering::registerTranslationPasses(*lowerPassMgr);

  timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, true);

  lowerPassMgr->addPass(LowerTranslator(ShaderStageCompute, &shaderInfo, "_gpurtvar_"));
  if (EnableOuts()) {
    lowerPassMgr->addPass(
        PrintModulePass(outs(), "\n"
                                "===============================================================================\n"
                                "// LLPC SPIRV-to-LLVM translation results\n"));
  }

  lowerPassMgr->addPass(LowerCfgMerges());
  lowerPassMgr->addPass(ProcessGpuRtLibrary(m_currentGpurtKey));
  lowerPassMgr->addPass(AlwaysInlinerPass());
  lowerPassMgr->addPass(LowerAccessChain());
  lowerPassMgr->addPass(LowerGlobals());

  // Run some basic optimization to simplify the code. This should be more efficient than optimizing them after they are
  // inlined into the caller.
  FunctionPassManager fpm;
  fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
  fpm.addPass(InstSimplifyPass());
  fpm.addPass(SimplifyCFGPass());
  // DCE is particularly useful for removing dead instructions after continuation call, which may help reducing
  // continuation stack size.
  fpm.addPass(ADCEPass());
  lowerPassMgr->addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));

  timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, false);

  lowerPassMgr->run(*gpurt);

  gpurtContext.ownedTheModule = std::move(gpurt);
  gpurtContext.theModule = gpurtContext.ownedTheModule.get();
}

// =====================================================================================================================
// Ensure that a GfxRuntime library module is attached to this context via GfxRuntimeContext.
void Context::ensureGfxRuntimeLibrary() {
  // Check whether we already have a GPURT library module that can be used
  auto &gfxRuntimeContext = lgc::GfxRuntimeContext::get(*this);

  if (gfxRuntimeContext.theModule)
    return;

  // Create the GfxRuntime library module
  ShaderModuleData moduleData = {};
  std::tie(moduleData.binCode.codeSize, moduleData.binCode.pCode) = Vkgc::GetAdvancedBlendLibrary();
  moduleData.binType = BinaryType::Spirv;
  moduleData.usage.keepUnusedFunctions = true;

  PipelineShaderInfo shaderInfo = {};
  shaderInfo.entryStage = ShaderStageCompute;
  shaderInfo.pEntryTarget = nullptr;
  shaderInfo.pModuleData = &moduleData;

  auto gfxRuntime = std::make_unique<Module>("gfxruntime", *this);
  setModuleTargetMachine(gfxRuntime.get());

  TimerProfiler timerProfiler(getPipelineHashCode(), "LLPC GfxRuntime", TimerProfiler::PipelineTimerEnableMask);
  std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(getLgcContext()));
  Lowering::registerTranslationPasses(*lowerPassMgr);

  timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, true);

  lowerPassMgr->addPass(LowerTranslator(ShaderStageCompute, &shaderInfo));
  if (EnableOuts()) {
    lowerPassMgr->addPass(
        PrintModulePass(outs(), "\n"
                                "===============================================================================\n"
                                "// LLPC SPIRV-to-LLVM translation results\n"));
  }

  lowerPassMgr->addPass(LowerCfgMerges());
  lowerPassMgr->addPass(ProcessGfxRuntimeLibrary());
  lowerPassMgr->addPass(AlwaysInlinerPass());
  lowerPassMgr->addPass(LowerAccessChain());
  lowerPassMgr->addPass(LowerGlobals());
  timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, false);

  lowerPassMgr->run(*gfxRuntime);

  gfxRuntimeContext.theModule = std::move(gfxRuntime);
}

} // namespace Llpc
