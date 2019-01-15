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
* @file  llpcSpirvLowerGlobalConstExprRemove.cpp
* @brief LLPC source file: contains implementation of Llpc::SpirvLowerGlobalConstExprRemove
***********************************************************************************************************************
*/
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#include <unordered_set>
#include "SPIRVInternal.h"
#include "llpcSpirvLowerGlobalConstExprRemove.h"

#define DEBUG_TYPE "llpc-spirv-lower-global-const-expr-remove"

using namespace llvm;
using namespace Llpc;
using namespace SPIRV;

char SpirvLowerGlobalConstExprRemove::ID = 0;

// =====================================================================================================================
// Create the pass
ModulePass* Llpc::CreateSpirvLowerGlobalConstExprRemove()
{
    return new SpirvLowerGlobalConstExprRemove();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool SpirvLowerGlobalConstExprRemove::runOnModule(
    llvm::Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Global-Const-Expr-Remove\n");

    SpirvLower::Init(&module);

    // First, identify "tainted constantexprs", that is, ones that refer directly or indirectly to the global variables
    // we are interested in.
    std::vector<ConstantExpr*> taintedConstExprs;
    std::unordered_set<ConstantExpr*> seenConstExprs;
    for (auto& global : module.globals())
    {
        auto addSpace = global.getType()->getAddressSpace();

        // Remove constant expressions for these global variables
        auto isGlobalVar = ((addSpace == SPIRAS_Private) || (addSpace == SPIRAS_Input) ||
            (addSpace == SPIRAS_Output));

        if (isGlobalVar == false)
        {
            continue;
        }

        for (auto pUser : global.users())
        {
            auto pConstExprUser = dyn_cast<ConstantExpr>(pUser);
            if (pConstExprUser != nullptr)
            {
                if (seenConstExprs.insert(pConstExprUser).second)
                {
                    taintedConstExprs.push_back(pConstExprUser);
                }
            }
        }
    }

    for (uint32_t i = 0; i < taintedConstExprs.size(); ++i)
    {
        auto pConstExpr = taintedConstExprs[i];
        for (auto pUser : pConstExpr->users())
        {
            auto pConstExprUser = dyn_cast<ConstantExpr>(pUser);
            if (pConstExprUser != nullptr)
            {
                if (seenConstExprs.insert(pConstExprUser).second)
                {
                    taintedConstExprs.push_back(pConstExprUser);
                }
            }
        }
    }

    if (taintedConstExprs.empty())
    {
        return false;
    }

    // Now use taintedConstExprs as a stack worklist. This minimizes the chance that we create a new use of a
    // constexpr after we have already processed it.
    std::unordered_set<ConstantExpr*> taintedConstExprList;
    for (auto pConstExpr : taintedConstExprs)
    {
        taintedConstExprList.insert(pConstExpr);
    }

    while (taintedConstExprs.empty() == false)
    {
        // Process one constexpr from the worklist.
        auto pConstExpr = taintedConstExprs.back();
        taintedConstExprs.pop_back();
        taintedConstExprList.erase(pConstExpr);

        SmallVector<Use*, 8> constExprUses;
        for (auto& use : pConstExpr->uses())
        {
            constExprUses.push_back(&use);
        }

        for (Use* pUse : constExprUses)
        {
            auto pInstUser = dyn_cast<Instruction>(pUse->getUser());
            if (pInstUser != nullptr)
            {
                // We have a use of the constexpr in an instruction. Replace with an instruction.
                auto pInst = pConstExpr->getAsInstruction();
                pInst->insertBefore(pInstUser);
                *pUse = pInst;
                // If any operand of the instruction is a "tainted constexpr" not on the worklist
                // (because we have already processed it), then we need to re-add it.
                for (auto& operand : pInst->operands())
                {
                    auto pConstExprOperand = dyn_cast<ConstantExpr>(&operand);
                    if ((pConstExprOperand != nullptr) &&
                          (seenConstExprs.find(pConstExprOperand) != seenConstExprs.end()) &&
                          taintedConstExprList.insert(pConstExprOperand).second)
                    {
                        taintedConstExprs.push_back(pConstExprOperand);
                    }
                }
            }
        }
    }

    return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(SpirvLowerGlobalConstExprRemove, DEBUG_TYPE,
        "Lower SPIR-V for removing global constant expressions", false, false)

