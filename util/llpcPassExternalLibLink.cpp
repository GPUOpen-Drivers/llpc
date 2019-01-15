/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PassExternalLibLink.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PassExternalLibLink.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-pass-external-lib-link"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitstreamReader.h"
#include "llvm/Bitcode/BitstreamWriter.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llpc.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcEmuLib.h"
#include "llpcInternal.h"
#include "llpcPassExternalLibLink.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

extern TimeProfileResult g_timeProfileResult;

// =====================================================================================================================
// Initializes static members.
char PassExternalLibLink::ID = 0;

// =====================================================================================================================
// Pass creator, creates the LLVM pass for linking external library of LLVM IR
ModulePass* CreatePassExternalLibLink(
    bool nativeOnly)    // Whether to only link native functions
{
    return new PassExternalLibLink(nativeOnly);
}

// =====================================================================================================================
PassExternalLibLink::PassExternalLibLink(
    bool nativeOnly) // Whether to only link native functions
    :
    llvm::ModulePass(ID),
    m_nativeOnly(nativeOnly)
{
    initializePassExternalLibLinkPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM module.
bool PassExternalLibLink::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    TimeProfiler timeProfiler(&g_timeProfileResult.patchLinkTime);
    auto pContext = static_cast<Context*>(&module.getContext());
    std::map<Module*, ValueToValueMapTy> valueMaps;

    LLVM_DEBUG(dbgs() << "Run the pass Pass-External-Lib-Link\n");

    for (;;)
    {
        LLVM_DEBUG(dbgs() << "Iteration\n");
        // Gather functions that are used and undefined (and not intrinsics).
        SmallVector<Function*, 4> undefinedFuncs;
        for (auto& func : module)
        {
            if (func.use_empty() || !func.empty() || func.isIntrinsic())
            {
                continue;
            }
            undefinedFuncs.push_back(&func);
        }
        // Attempt to link gathered declarations.
        unsigned satisfiedCount = 0;
        for (auto pFunc : undefinedFuncs)
        {
            // We have a declaration that we need to satisfy by linking in a function from the
            // emulation library.
            LLVM_DEBUG(dbgs() << "Looking for " << pFunc->getName() << "\n");
            auto pLibFunc = pContext->GetGlslEmuLib()->GetFunction(pFunc->getName(), m_nativeOnly);
            if (pLibFunc == nullptr)
            {
                if (m_nativeOnly ||
                    pFunc->getName().startswith(LlpcName::InputCallPrefix) ||
                    pFunc->getName().startswith(LlpcName::OutputCallPrefix) ||
                    pFunc->getName().startswith(LlpcName::DescriptorCallPrefix))
                {
                    // Allow unsatisfied externals in the first "native only" linking pass,
                    // or for certain prefixes that are not patched until after linking.
                    continue;
                }
                llvm_unreachable(("not found in emulation library: " + pFunc->getName()).str().c_str());
            }
            ValueToValueMapTy* pValueMap = nullptr;
            auto valueMapsIt = valueMaps.find(pLibFunc->getParent());
            if (valueMapsIt == valueMaps.end())
            {
                // This is the first time we have needed a function from this library module.
                // Copy all functions as declarations from the library module to our module.
                pValueMap = &valueMaps[pLibFunc->getParent()];
                for (auto& libDecl : *pLibFunc->getParent())
                {
                    auto pMappedDecl = module.getFunction(libDecl.getName());
                    if (pMappedDecl == nullptr)
                    {
                        pMappedDecl = Function::Create(cast<FunctionType>(libDecl.getValueType()),
                                                       libDecl.getLinkage(),
                                                       libDecl.getName(),
                                                       &module);
                        pMappedDecl->setAttributes(libDecl.getAttributes());
                    }
                    (*pValueMap)[&libDecl] = pMappedDecl;
                }
            }
            else
            {
                pValueMap = &valueMapsIt->second;
            }
            // Clone the library function across to our module.
            ++satisfiedCount;
            Function::arg_iterator funcArgIter = pFunc->arg_begin();
            for (Function::const_arg_iterator libFuncArgIter = pLibFunc->arg_begin();
                     libFuncArgIter != pLibFunc->arg_end();
                     ++libFuncArgIter)
            {
                funcArgIter->setName(libFuncArgIter->getName());
                (*pValueMap)[&*libFuncArgIter] = &*funcArgIter++;
            }

            SmallVector<ReturnInst*, 8> retInsts;
            CloneFunctionInto(pFunc, pLibFunc, *pValueMap, true, retInsts);
            pFunc->setLinkage(GlobalValue::InternalLinkage);
        }

        if (satisfiedCount == 0)
        {
            // Finished -- no new externals satisified this time round.
            break;
        }
    }

    // Prune any unused declarations that we added above.
    for (auto moduleIt = module.begin(), moduleEnd = module.end(); moduleIt != moduleEnd; )
    {
        auto moduleNext = std::next(moduleIt);
        if (moduleIt->empty() && moduleIt->use_empty())
        {
            moduleIt->eraseFromParent();
        }
        moduleIt = moduleNext;
    }

    return true;
}

} // Llpc

// =====================================================================================================================
// Initializes the LLVM pass for linking external libraries.
INITIALIZE_PASS(PassExternalLibLink, DEBUG_TYPE,
                "LLVM pass for linking external libraries", false, false)
