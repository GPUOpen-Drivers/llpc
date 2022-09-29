/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PassManager.cpp
 * @brief LLPC source file: contains implementation of class lgc::PassManagerImpl.
 ***********************************************************************************************************************
 */
#include "lgc/PassManager.h"
#include "lgc/util/Debug.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/PrintPasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"

namespace llvm {
namespace cl {

// -verify-ir : verify the IR after each pass
static cl::opt<bool> VerifyIr("verify-ir", cl::desc("Verify IR after each pass"), cl::init(false));

// -dump-cfg-after : dump CFG as .dot files after specified pass
static cl::opt<std::string> DumpCfgAfter("dump-cfg-after", cl::desc("Dump CFG as .dot files after specified pass"),
                                         cl::init(""));

// -dump-pass-name : dump executed pass name
static cl::opt<bool> DumpPassName("dump-pass-name", cl::desc("Dump executed pass name"), cl::init(false));

// -disable-pass-indices: indices of passes to be disabled
static cl::list<unsigned> DisablePassIndices("disable-pass-indices", cl::ZeroOrMore,
                                             cl::desc("Indices of passes to be disabled"));

static cl::opt<bool> DebugPassManager("debug-pass-manager", cl::desc("Print pass management debugging information"),
                                      cl::Hidden, cl::init(false));

} // namespace cl

// A proxy from a ModuleAnalysisManager to a loop.
using ModuleAnalysisManagerLoopProxy =
    OuterAnalysisManagerProxy<ModuleAnalysisManager, Loop, LoopStandardAnalysisResults &>;

} // namespace llvm

using namespace lgc;
using namespace llvm;

namespace {

// =====================================================================================================================
// LLPC's legacy::PassManager override.
// This is the implementation subclass of the PassManager class declared in PassManager.h
class LegacyPassManagerImpl final : public lgc::LegacyPassManager {
public:
  LegacyPassManagerImpl();
  ~LegacyPassManagerImpl() override {}

  void setPassIndex(unsigned *passIndex) override { m_passIndex = passIndex; }
  void add(Pass *pass) override;
  void stop() override;

private:
  bool m_stopped = false;               // Whether we have already stopped adding new passes.
  AnalysisID m_dumpCfgAfter = nullptr;  // -dump-cfg-after pass id
  AnalysisID m_printModule = nullptr;   // Pass id of dump pass "Print Module IR"
  AnalysisID m_jumpThreading = nullptr; // Pass id of opt pass "Jump Threading"
  unsigned *m_passIndex = nullptr;      // Pass Index
};

// =====================================================================================================================
// LLPC's PassManager override.
// This is the implementation subclass of the PassManager class declared in PassManager.h
class PassManagerImpl final : public lgc::PassManager {
public:
  PassManagerImpl(TargetMachine *targetMachine);
  void registerPass(StringRef passName, StringRef className) override;
  void run(Module &module) override;
  void setPassIndex(unsigned *passIndex) override { m_passIndex = passIndex; }

private:
  void registerCallbacks();
  TargetMachine *m_targetMachine;

  // -----------------------------------------------------------------------------------------------------------------

  LoopAnalysisManager m_loopAnalysisManager;               // Loop analysis manager used when running the passes.
  CGSCCAnalysisManager m_cgsccAnalysisManager;             // CGSCC analysis manager used when running the passes.
  PassInstrumentationCallbacks m_instrumentationCallbacks; // Instrumentation callbacks ran when running the passes.
  StandardInstrumentations m_instrumentationStandard;      // LLVM's Standard instrumentations
  unsigned *m_passIndex = nullptr;                         // Pass Index.
  bool initialized = false;                                // Whether the pass manager is initialized or not
};

} // namespace

// =====================================================================================================================
// Get the PassInfo for a registered pass given short name
//
// @param passName : Short name of pass
static const PassInfo *getPassInfo(StringRef passName) {
  if (passName.empty())
    return nullptr;

  const PassRegistry &passRegistry = *PassRegistry::getPassRegistry();
  const PassInfo *passInfo = passRegistry.getPassInfo(passName);
  if (!passInfo) {
    report_fatal_error(Twine('\"') + Twine(passName) + Twine("\" pass is not registered."));
  }
  return passInfo;
}

// =====================================================================================================================
// Get the ID for a registered pass given short name
//
// @param passName : Short name of pass
static AnalysisID getPassIdFromName(StringRef passName) {
  const PassInfo *passInfo = getPassInfo(passName);
  return passInfo ? passInfo->getTypeInfo() : nullptr;
}

// =====================================================================================================================
// Create a LegacyPassManagerImpl
lgc::LegacyPassManager *lgc::LegacyPassManager::Create() {
  return new LegacyPassManagerImpl;
}

// =====================================================================================================================
// Create a PassManagerImpl
//
// @param targetMachine : Optional target machine argument. Must be provided if the AMDLLPC target specific alias
// analysis pass needs to be registered.
lgc::PassManager *lgc::PassManager::Create(TargetMachine *targetMachine) {
  return new PassManagerImpl(targetMachine);
}

// =====================================================================================================================
LegacyPassManagerImpl::LegacyPassManagerImpl() : LegacyPassManager() {
  if (!cl::DumpCfgAfter.empty())
    m_dumpCfgAfter = getPassIdFromName(cl::DumpCfgAfter);

  m_jumpThreading = getPassIdFromName("jump-threading");
  m_printModule = getPassIdFromName("print-module");
}

// =====================================================================================================================
PassManagerImpl::PassManagerImpl(TargetMachine *targetMachine)
    : PassManager(), m_targetMachine(targetMachine),
      m_instrumentationStandard(cl::DebugPassManager, cl::DebugPassManager || cl::VerifyIr,
                                /*PrintPassOpts=*/{true, false, true}) {
  if (!cl::DumpCfgAfter.empty())
    report_fatal_error("The --dump-cfg-after option is not supported with the new pass manager.");

  // Setup custom instrumentation callbacks and register LLVM's default module
  // analyses to the analysis manager.
  registerCallbacks();

  // Register standard instrumentation callbacks.
  m_instrumentationStandard.registerCallbacks(m_instrumentationCallbacks);
}

// =====================================================================================================================
// Register a pass to identify it with a short name in the pass manager
//
// @param passName : Dash-case short name to use for registration
// @param className : Full pass name
void PassManagerImpl::registerPass(StringRef passName, StringRef className) {
  m_instrumentationCallbacks.addClassToPassName(className, passName);
}

// =====================================================================================================================
// Run all the added passes with the pass managers's module analysis manager
//
// @param module : Module to run the passes on
void PassManagerImpl::run(Module &module) {
  // We register LLVM's default analysis sets late to be sure our custom
  // analyses are added beforehand.
  if (!initialized) {
    PassBuilder passBuilder(m_targetMachine, PipelineTuningOptions(), None, &m_instrumentationCallbacks);
    passBuilder.registerModuleAnalyses(m_moduleAnalysisManager);
    passBuilder.registerCGSCCAnalyses(m_cgsccAnalysisManager);
    passBuilder.registerFunctionAnalyses(m_functionAnalysisManager);
    passBuilder.registerLoopAnalyses(m_loopAnalysisManager);
    passBuilder.crossRegisterProxies(m_loopAnalysisManager, m_functionAnalysisManager, m_cgsccAnalysisManager,
                                     m_moduleAnalysisManager);
    m_loopAnalysisManager.registerPass([&] { return ModuleAnalysisManagerLoopProxy(m_moduleAnalysisManager); });
    initialized = true;
  }
  ModulePassManager::run(module, m_moduleAnalysisManager);
}

// =====================================================================================================================
// Register LLPC's custom callbacks
//
void PassManagerImpl::registerCallbacks() {
  // Before running a pass, we increment the pass index if it exists, and we
  // dump the pass name if needed.
  auto beforePass = [this](StringRef passName, Any ir) {
    if (passName != PrintModulePass::name() && m_passIndex) {
      unsigned passIndex = (*m_passIndex)++;
      if (cl::DumpPassName)
        LLPC_OUTS("Pass[" << passIndex << "] = " << passName << "\n");
    }
  };
  m_instrumentationCallbacks.registerBeforeSkippedPassCallback(beforePass);
  m_instrumentationCallbacks.registerBeforeNonSkippedPassCallback(beforePass);

  m_instrumentationCallbacks.registerShouldRunOptionalPassCallback([this](StringRef passName, Any ir) { // NOLINT
    // Skip the jump threading pass as it interacts really badly with the structurizer.
    if (passName == JumpThreadingPass::name())
      return false;
    // Check if the user disabled that specific pass index.
    if (passName != PrintModulePass::name() && m_passIndex) {
      unsigned passIndex = *m_passIndex;
      for (auto disableIndex : cl::DisablePassIndices) {
        if (disableIndex == passIndex) {
          LLPC_OUTS("Pass[" << passIndex << "] = " << passName << " (disabled)\n");
          return false;
        }
      }
    }
    return true;
  });
}

// =====================================================================================================================
// Add a pass to the pass manager.
//
// @param pass : Pass to add to the pass manager
void LegacyPassManagerImpl::add(Pass *pass) {
  // Do not add any passes after calling stop(), except immutable passes.
  if (m_stopped && !pass->getAsImmutablePass())
    return;

  AnalysisID passId = pass->getPassID();

  // Skip the jump threading pass as it interacts really badly with the structurizer.
  if (passId == m_jumpThreading)
    return;

  if (passId != m_printModule && m_passIndex) {
    unsigned passIndex = (*m_passIndex)++;

    for (auto disableIndex : cl::DisablePassIndices) {
      if (disableIndex == passIndex) {
        LLPC_OUTS("Pass[" << passIndex << "] = " << pass->getPassName() << " (disabled)\n");
        return;
      }
    }

    if (cl::DumpPassName)
      LLPC_OUTS("Pass[" << passIndex << "] = " << pass->getPassName() << "\n");
  }

  // Add the pass to the superclass pass manager.
  legacy::PassManager::add(pass);

  if (cl::VerifyIr) {
    // Add a verify pass after it.
    legacy::PassManager::add(createVerifierPass(true)); // FatalErrors=true
  }

  if (passId == m_dumpCfgAfter) {
    // Add a CFG printer pass after it.
    legacy::PassManager::add(createCFGPrinterLegacyPassPass());
  }
}

// =====================================================================================================================
// Stop adding passes to the pass manager, except immutable ones.
void LegacyPassManagerImpl::stop() {
  m_stopped = true;
}
