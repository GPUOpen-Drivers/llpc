/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPassManager.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PassManager.
 ***********************************************************************************************************************
 */
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"

#include "llpcPassManager.h"

namespace llvm
{
namespace cl
{

// -verify-ir : verify the IR after each pass
static cl::opt<bool> VerifyIr("verify-ir",
                              cl::desc("Verify IR after each pass"),
                              cl::init(false));

// -dump-cfg-after : dump CFG as .dot files after specified pass
static cl::opt<std::string> DumpCfgAfter("dump-cfg-after",
                                         cl::desc("Dump CFG as .dot files after specified pass"),
                                         cl::init(""));

} // cl

} // llvm

using namespace llvm;
using namespace Llpc;

// =====================================================================================================================
// Get the PassInfo for a registered pass given short name
static const PassInfo* GetPassInfo(
    StringRef passName)   // Short name of pass
{
    if (passName.empty())
    {
        return nullptr;
    }

    const PassRegistry& passRegistry = *PassRegistry::getPassRegistry();
    const PassInfo* pPassInfo = passRegistry.getPassInfo(passName);
    if (pPassInfo == nullptr)
    {
        report_fatal_error(Twine('\"') + Twine(passName) +
                           Twine("\" pass is not registered."));
    }
    return pPassInfo;
}

// =====================================================================================================================
// Get the ID for a registered pass given short name
static AnalysisID GetPassIdFromName(
    StringRef passName)   // Short name of pass
{
  const PassInfo* pPassInfo = GetPassInfo(passName);
  return pPassInfo ? pPassInfo->getTypeInfo() : nullptr;
}

// =====================================================================================================================
// Constructor
Llpc::PassManager::PassManager() :
    legacy::PassManager()
{
    if (cl::DumpCfgAfter.empty() == false)
    {
        m_dumpCfgAfter = GetPassIdFromName(cl::DumpCfgAfter);
    }
}

// =====================================================================================================================
// Add a pass to the pass manager.
void Llpc::PassManager::add(
    Pass* pPass)    // [in] Pass to add to the pass manager
{
    // Do not add any passes after calling stop(), except immutable passes.
    if (m_stopped && (pPass->getAsImmutablePass() == nullptr))
    {
        return;
    }

    // Skip the jump threading pass as it interacts really badly with the structurizer.
    if (pPass->getPassName().equals("Jump Threading"))
    {
        return;
    }

    // Add the pass to the superclass pass manager.
    legacy::PassManager::add(pPass);

    if (cl::VerifyIr)
    {
        // Add a verify pass after it.
        legacy::PassManager::add(createVerifierPass(true)); // FatalErrors=true
    }

    AnalysisID passId = pPass->getPassID();
    if (passId == m_dumpCfgAfter)
    {
        // Add a CFG printer pass after it.
        legacy::PassManager::add(createCFGPrinterLegacyPassPass());
    }
}

// =====================================================================================================================
// Stop adding passes to the pass manager, except immutable ones.
void Llpc::PassManager::stop()
{
    m_stopped = true;
}

