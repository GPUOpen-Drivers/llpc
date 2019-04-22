/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llpcDebug.h"
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

// -dump-pass-name : dump executed pass name
static cl::opt<bool> DumpPassName("dump-pass-name", cl::desc("Dump executed pass name"), cl::init(false));

// -disable-pass-indices: indices of passes to be disabled
static cl::list<uint32_t> DisablePassIndices("disable-pass-indices", cl::ZeroOrMore, cl::desc("Indices of passes to be disabled"));

} // cl

} // llvm

using namespace llvm;
namespace Llpc
{
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
PassManager::PassManager(
    uint32_t* pPassIndex)   // [in,out] Pointer of PassIndex
    :
    m_pPassIndex(pPassIndex)
{
    if (cl::DumpCfgAfter.empty() == false)
    {
        m_dumpCfgAfter = GetPassIdFromName(cl::DumpCfgAfter);
    }

    m_jumpThreading = GetPassIdFromName("jump-threading");
    m_printModule = GetPassIdFromName("print-module");
}

// =====================================================================================================================
// Add a pass to the pass manager.
void PassManager::add(
    Pass* pPass)    // [in] Pass to add to the pass manager
{
    // Do not add any passes after calling stop(), except immutable passes.
    if (m_stopped && (pPass->getAsImmutablePass() == nullptr))
    {
        return;
    }

    AnalysisID passId = pPass->getPassID();

    // Skip the jump threading pass as it interacts really badly with the structurizer.
    if (passId == m_jumpThreading)
    {
        return;
    }

    if (passId != m_printModule)
    {
        uint32_t passIndex = (*m_pPassIndex)++;

        for (auto disableIndex : cl::DisablePassIndices)
        {
            if (disableIndex == passIndex)
            {
                LLPC_OUTS("Pass[" << passIndex << "] = " << pPass->getPassName() << " (disabled)\n");
                return;
            }
        }

        if (cl::DumpPassName)
        {
            LLPC_OUTS("Pass[" << passIndex << "] = " << pPass->getPassName() << "\n");
        }
    }

    // Add the pass to the superclass pass manager.
    legacy::PassManager::add(pPass);

    if (cl::VerifyIr)
    {
        // Add a verify pass after it.
        legacy::PassManager::add(createVerifierPass(true)); // FatalErrors=true
    }

    if (passId == m_dumpCfgAfter)
    {
        // Add a CFG printer pass after it.
        legacy::PassManager::add(createCFGPrinterLegacyPassPass());
    }
}

// =====================================================================================================================
// Stop adding passes to the pass manager, except immutable ones.
void PassManager::stop()
{
    m_stopped = true;
}

// =====================================================================================================================
// Runs passes on the module
bool PassManager::run(
    Module* pModule)  // [in] LLVM module
{
    bool success = false;
#if LLPC_ENABLE_EXCEPTION
    try
#endif
    {
        success = legacy::PassManager::run(*pModule);
    }
#if LLPC_ENABLE_EXCEPTION
    catch (const char*)
    {
        success = false;
    }
#endif
    return success;
}

} // Llpc
