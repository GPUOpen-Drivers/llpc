/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"

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

} // namespace cl

} // namespace llvm

using namespace lgc;
using namespace llvm;

namespace {

// =====================================================================================================================
// LLPC's legacy::PassManager override.
// This is the implementation subclass of the PassManager class declared in PassManager.h
class PassManagerImpl final : public lgc::PassManager {
public:
  PassManagerImpl();
  ~PassManagerImpl() override {}

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
// Create a PassManagerImpl
lgc::PassManager *lgc::PassManager::Create() {
  return new PassManagerImpl;
}

// =====================================================================================================================
PassManagerImpl::PassManagerImpl() : PassManager() {
  if (!cl::DumpCfgAfter.empty())
    m_dumpCfgAfter = getPassIdFromName(cl::DumpCfgAfter);

  m_jumpThreading = getPassIdFromName("jump-threading");
  m_printModule = getPassIdFromName("print-module");
}

// =====================================================================================================================
// Add a pass to the pass manager.
//
// @param pass : Pass to add to the pass manager
void PassManagerImpl::add(Pass *pass) {
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
void PassManagerImpl::stop() {
  m_stopped = true;
}
