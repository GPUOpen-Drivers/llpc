/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerMemoryOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerMemoryOp.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerMemoryOp.h"

#define DEBUG_TYPE "llpc-spirv-lower-memory-op"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerMemoryOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering memory operations.
ModulePass* CreateSpirvLowerMemoryOp()
{
    return new SpirvLowerMemoryOp();
}

// =====================================================================================================================
SpirvLowerMemoryOp::SpirvLowerMemoryOp()
    :
    SpirvLower(ID)
{
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerMemoryOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Memory-Op\n");

    SpirvLower::Init(&module);

    visit(m_pModule);

    // Remove those instructions that are replaced by this lower pass
    for (auto pInst : m_preRemoveInsts)
    {
        assert(pInst->user_empty());
        pInst->dropAllReferences();
        pInst->eraseFromParent();
    }
    m_preRemoveInsts.clear();

    for (unsigned i = 0; i < m_storeExpandInfo.size(); i++)
    {
        StoreExpandInfo* pExpandInfo = &m_storeExpandInfo[i];
        ExpandStoreInst(pExpandInfo->pStoreInst, pExpandInfo->getElemPtrs, pExpandInfo->pDynIndex);
    }
    m_storeExpandInfo.clear();

    for (auto pInst : m_removeInsts)
    {
        assert(pInst->user_empty());
        pInst->dropAllReferences();
        pInst->eraseFromParent();
    }
    m_removeInsts.clear();

    LLVM_DEBUG(dbgs() << "After the pass Spirv-Lower-Memory-Op " << module);

    return true;
}

// =====================================================================================================================
// Visits "extractelement" instruction.
void SpirvLowerMemoryOp::visitExtractElementInst(
    ExtractElementInst& extractElementInst)  // "ExtractElement" instruction
{
    auto pSrc = extractElementInst.getOperand(0);
    if (pSrc->getType()->isVectorTy() &&
        isa<LoadInst>(pSrc) &&
        pSrc->hasOneUse())
    {
        // NOTE: Optimize loading vector component for local variable and memory block
        // Original pattern:
        // %1 = load <4 x float> addrspace(7)* %2
        // %2 = extractelement <4 x float> %1, i32 0
        // after transform:
        // %1 = bitcast <4 x float> addrspace(7)* %2 to[4 x float] addrspace(7)*
        // %3 = getelementptr[4 x float] addrspace(7)* %2, i32 0, i32 0
        // %4 = load float addrspace(7)* %3

        auto pLoadInst = cast<LoadInst>(pSrc);
        auto pLoadPtr = pLoadInst->getOperand(0);
        auto addrSpace = pLoadPtr->getType()->getPointerAddressSpace();

        if ((addrSpace == SPIRAS_Local) || (addrSpace == SPIRAS_Uniform))
        {
            auto pSrcTy = pSrc->getType();
            auto pCastTy = ArrayType::get(pSrcTy->getVectorElementType(), pSrcTy->getVectorNumElements());
            auto pCastPtrTy = pCastTy->getPointerTo(addrSpace);
            auto pCastPtr = new BitCastInst(pLoadPtr, pCastPtrTy, "", &extractElementInst);
            Value* idxs[] =
            {
                ConstantInt::get(Type::getInt32Ty(*m_pContext), 0),
                extractElementInst.getOperand(1)
            };
            auto pElementPtr = GetElementPtrInst::Create(nullptr, pCastPtr, idxs, "", &extractElementInst);
            auto pNewLoad = new LoadInst(pElementPtr, "", &extractElementInst);
            extractElementInst.replaceAllUsesWith(pNewLoad);

            m_preRemoveInsts.insert(&extractElementInst);
            m_removeInsts.insert(pLoadInst);
        }
    }
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
void SpirvLowerMemoryOp::visitGetElementPtrInst(
    GetElementPtrInst& getElemPtrInst) // "GetElementPtr" instruction
{
    unsigned operandIndex = InvalidValue;
    unsigned dynIndexBound = 0;

    if (NeedExpandDynamicIndex(&getElemPtrInst, &operandIndex, &dynIndexBound))
    {
        SmallVector<GetElementPtrInst*, 1> getElemPtrs;
        auto pDynIndex = getElemPtrInst.getOperand(operandIndex);
        bool isType64 = (pDynIndex->getType()->getPrimitiveSizeInBits() == 64);

        // Create "getelementptr" instructions with constant indices
        for (unsigned i = 0; i < dynIndexBound; ++i)
        {
            auto pGetElemPtr = cast<GetElementPtrInst>(getElemPtrInst.clone());
            auto pConstIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_pContext), i) :
                                          ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
            pGetElemPtr->setOperand(operandIndex, pConstIndex);
            getElemPtrs.push_back(pGetElemPtr);
            pGetElemPtr->insertBefore(&getElemPtrInst);
        }

        // Copy users, ExpandStoreInst/ExpandLoadInst change getElemPtrInst's user
        std::vector<User*> users;
        for (auto pUser : getElemPtrInst.users())
        {
            users.push_back(pUser);
        }

        // Replace the original "getelementptr" instructions with a group of newly-created "getelementptr" instructions
        for (auto pUser : users)
        {
            auto pLoadInst = dyn_cast<LoadInst>(pUser);
            auto pStoreInst = dyn_cast<StoreInst>(pUser);

            if (pLoadInst != nullptr)
            {
                ExpandLoadInst(pLoadInst, getElemPtrs, pDynIndex);
            }
            else if (pStoreInst != nullptr)
            {
                RecordStoreExpandInfo(pStoreInst, getElemPtrs, pDynIndex);
            }
            else
            {
                llvm_unreachable("Should never be called!");
            }
        }

        // Collect replaced instructions that will be removed
        m_removeInsts.insert(&getElemPtrInst);
    }
}

// =====================================================================================================================
// Checks whether the specified "getelementptr" instruction contains dynamic index and is therefore able to be expanded.
bool SpirvLowerMemoryOp::NeedExpandDynamicIndex(
    GetElementPtrInst* pGetElemPtr,       // [in] "GetElementPtr" instruction
    unsigned*          pOperandIndexOut,  // [out] Index of the operand that represents a dynamic index
    unsigned*          pDynIndexBound     // [out] Upper bound of dynamic index
    ) const
{
    static const unsigned MaxDynIndexBound = 8;

    std::vector<Value*> idxs;
    unsigned operandIndex = InvalidValue;
    bool     needExpand   = false;
    bool     allowExpand  = true;
    auto     pPtrVal      = pGetElemPtr->getPointerOperand();

    // NOTE: We only handle local variables.
    if (pPtrVal->getType()->getPointerAddressSpace() != SPIRAS_Private)
    {
        allowExpand = false;
    }

    for (unsigned i = 1, operandCount = pGetElemPtr->getNumOperands(); allowExpand && (i < operandCount); ++i)
    {
        auto pIndex = pGetElemPtr->getOperand(i);
        if (isa<Constant>(pIndex) == false)
        {
            // Find the operand that represents a dynamic index
            if (operandIndex == InvalidValue)
            {
                // This is the first operand that represents a dynamic index
                operandIndex = i;
                needExpand = true;

                auto pIndexedTy = pGetElemPtr->getIndexedType(pPtrVal->getType()->getPointerElementType(), idxs);
                if (pIndexedTy != nullptr)
                {
                    // Check the upper bound of dynamic index
                    if (isa<ArrayType>(pIndexedTy))
                    {
                        auto pArrayTy = dyn_cast<ArrayType>(pIndexedTy);
                        if (pArrayTy->getArrayNumElements() > MaxDynIndexBound)
                        {
                            // Skip expand if array size greater than threshold
                            allowExpand = false;
                        }
                        else
                        {
                            *pDynIndexBound = pArrayTy->getArrayNumElements();
                        }
                    }
                    else if (isa<VectorType>(pIndexedTy))
                    {
                        // Always expand for vector
                        auto pVectorTy = dyn_cast<VectorType>(pIndexedTy);
                        *pDynIndexBound = pVectorTy->getVectorNumElements();
                    }
                    else
                    {
                        llvm_unreachable("Should never be called!");
                        allowExpand = false;
                    }
                }
                else
                {
                    llvm_unreachable("Should never be called!");
                    allowExpand = false;
                }
            }
            else
            {
                // Skip expand if there are more than one dynamic indices
                allowExpand = false;
            }
        }
        else
        {
            idxs.push_back(pIndex);
        }
    }

    if (needExpand && allowExpand)
    {
        // Skip expand if the user of "getelementptr" is neither "load" nor "store"
        for (auto pUser : pGetElemPtr->users())
        {
            if ((isa<LoadInst>(pUser) == false) && (isa<StoreInst>(pUser) == false))
            {
                allowExpand = false;
                break;
            }
        }
    }

    *pOperandIndexOut = operandIndex;
    return needExpand && allowExpand;
}

// =====================================================================================================================
// Expands "load" instruction with constant-index "getelementptr" instructions.
void SpirvLowerMemoryOp::ExpandLoadInst(
    LoadInst*                    pLoadInst,       // [in] "Load" instruction
    ArrayRef<GetElementPtrInst*> getElemPtrs,     // [in] A group of "getelementptr" with constant indices
    Value*                       pDynIndex)       // [in] Dynamic index
{
    // Expand is something like this:
    //
    //   firstValue  = load getElemPtrs[0]
    //
    //   secondValue = load getElemPtrs[1]
    //   firstValue  = (dynIndex == 1) ?  secondValue : firstValue
    //
    //   secondValue = load getElemPtrs[2]
    //   firstValue  = (dynIndex == 2) ?  secondValue : firstValue
    //   ...
    //   secondValue = load getElemPtrs[upperBound -2]
    //   firstValue  = (dynIndex == upperBound - 2) ? secondValue : firstValue
    //   secondValue = load getElemPtrs[upperBound - 1]
    //   firstValue  = (dynIndex == upperBound - 1) ? secondValue : firstValue
    //
    //   loadValue   = firstValue

    bool isType64 = (pDynIndex->getType()->getPrimitiveSizeInBits() == 64);
    Instruction* pFirstLoadValue = new LoadInst(getElemPtrs[0], "", false, pLoadInst);

    for (unsigned i = 1, getElemPtrCount = getElemPtrs.size(); i < getElemPtrCount; ++i)
    {
        auto pConstIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_pContext), i) :
                                      ConstantInt::get(Type::getInt32Ty(*m_pContext), i);

        auto pSecondLoadValue = new LoadInst(getElemPtrs[i], "", false, pLoadInst);
        auto pCond = new ICmpInst(pLoadInst, ICmpInst::ICMP_EQ, pDynIndex, pConstIndex);
        pFirstLoadValue = SelectInst::Create(pCond, pSecondLoadValue, pFirstLoadValue, "", pLoadInst);
    }

    pLoadInst->replaceAllUsesWith(pFirstLoadValue);
    m_preRemoveInsts.insert(pLoadInst);
}

// =====================================================================================================================
// Record store expansion info after visit, because splitBasicBlock will disturb the visit.
void SpirvLowerMemoryOp::RecordStoreExpandInfo(
    StoreInst*                   pStoreInst,     // [in] "Store" instruction
    ArrayRef<GetElementPtrInst*> getElemPtrs,    // [in] A group of "getelementptr" with constant indices
    Value*                       pDynIndex)      // [in] Dynamic index
{
    StoreExpandInfo expandInfo = {};
    expandInfo.pStoreInst = pStoreInst;
    expandInfo.pDynIndex  = pDynIndex;

    for (unsigned i = 0; i < getElemPtrs.size(); ++i)
    {
        expandInfo.getElemPtrs.push_back(getElemPtrs[i]);
    }

    m_storeExpandInfo.push_back(expandInfo);
}

// =====================================================================================================================
// Expands "store" instruction with fixed indexed "getelementptr" instructions.
void SpirvLowerMemoryOp::ExpandStoreInst(
    StoreInst*                   pStoreInst,     // [in] "Store" instruction
    ArrayRef<GetElementPtrInst*> getElemPtrs,    // [in] A group of "getelementptr" with constant indices
    Value*                       pDynIndex)      // [in] Dynamic index
{
    const bool robustBufferAccess = m_pContext->GetRobustBufferAccess();
    const unsigned getElemPtrCount = getElemPtrs.size();
    bool isType64 = (pDynIndex->getType()->getPrimitiveSizeInBits() == 64);
    Value* pFirstStoreDest = getElemPtrs[0];

    if (robustBufferAccess)
    {
        // The .entry will be splitted into three blocks, .entry, .store and .endStore
        //
        // Expand is something like this:
        //
        // .entry
        //   ...
        //   if (dynIndex < upperBound) goto .store
        //   else goto .endStore
        //
        // .store
        //   firstPtr  = getElemPtrs[0]
        //
        //   secondPtr = getElemPtrs[1]
        //   firstPtr  = (dynIndex == 1) ? secondPtr : firstPtr
        //
        //   secondPtr = getElemPtrs[2]
        //   firstPtr  = (dynIndex == 2) ? secondPtr : firstPtr
        //   ...
        //   secondPtr = getElemPtrs[upperBound - 2]
        //   firstPtr  = (dynIndex == upperBound - 2) ? secondPtr : firstPtr
        //
        //   secondPtr = getElemPtrs[upperBound - 1]
        //   firstPtr  = (dynIndex == upperBound - 1) ? secondPtr : firstPtr
        //
        //   store storeValue, firstPtr
        //   goto .endStore
        //
        // .endStore
        //   ...
        //   ret

        auto pCheckStoreBlock = pStoreInst->getParent();
        auto pStoreBlock      = pCheckStoreBlock->splitBasicBlock(pStoreInst);
        auto pEndStoreBlock   = pStoreBlock->splitBasicBlock(pStoreInst);

        Instruction* pCheckStoreInsertPos = &pCheckStoreBlock->getInstList().back();
        Instruction* pStoreInsertPos      = &pStoreBlock->getInstList().front();

        auto pGetElemPtrCountVal = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_pContext), getElemPtrCount) :
                                              ConstantInt::get(Type::getInt32Ty(*m_pContext), getElemPtrCount);

        auto pDoStore = new ICmpInst(pCheckStoreInsertPos, ICmpInst::ICMP_ULT, pDynIndex, pGetElemPtrCountVal);
        BranchInst::Create(pStoreBlock, pEndStoreBlock, pDoStore, pCheckStoreInsertPos);

        for (unsigned i = 1; i < getElemPtrCount; ++i)
        {
            auto pSecondStoreDest = getElemPtrs[i];
            auto pConstIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_pContext), i) :
                                          ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
            auto pCond = new ICmpInst(pStoreInsertPos, ICmpInst::ICMP_EQ, pDynIndex, pConstIndex);
            pFirstStoreDest = SelectInst::Create(pCond, pSecondStoreDest, pFirstStoreDest, "", pStoreInsertPos);
        }

        Value* pStoreValue = pStoreInst->getOperand(0);
        new StoreInst(pStoreValue, pFirstStoreDest, pStoreInsertPos);

        pCheckStoreInsertPos->eraseFromParent();

        assert(pStoreInst->user_empty());
        pStoreInst->dropAllReferences();
        pStoreInst->eraseFromParent();
    }
    else
    {
        // .entry
        //   ...
        //   firstPtr  = getElemPtrs[0]
        //
        //   secondPtr = getElemPtrs[1]
        //   firstPtr  = (dynIndex == 1) ? secondPtr : firstPtr
        //
        //   secondPtr = getElemPtrs[2]
        //   firstPtr  = (dynIndex == 2) ? secondPtr : firstPtr
        //   ...
        //   secondPtr = getElemPtrs[upperBound - 2]
        //   firstPtr  = (dynIndex == upperBound - 2) ? secondPtr : firstPtr
        //
        //   secondPtr = getElemPtrs[upperBound - 1]
        //   firstPtr  = (dynIndex == upperBound - 1) ? secondPtr : firstPtr
        //
        //   store storeValue, firstPtr
        //   ...
        //   ret

        for (unsigned i = 1; i < getElemPtrCount; ++i)
        {
            auto pSecondStoreDest = getElemPtrs[i];
            auto pConstIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_pContext), i) :
                                          ConstantInt::get(Type::getInt32Ty(*m_pContext), i);
            auto pCond = new ICmpInst(pStoreInst, ICmpInst::ICMP_EQ, pDynIndex, pConstIndex);
            pFirstStoreDest = SelectInst::Create(pCond, pSecondStoreDest, pFirstStoreDest, "", pStoreInst);
        }

        pStoreInst->setOperand(1, pFirstStoreDest);
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering the memory operations.
INITIALIZE_PASS(SpirvLowerMemoryOp, DEBUG_TYPE,
                "Lower SPIR-V memory operations", false, false)
