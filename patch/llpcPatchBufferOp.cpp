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
 * @file  llpcPatchBufferOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-buffer-op"

#include "llvm/InitializePasses.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/LegacyDivergenceAnalysis.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "SPIRVInternal.h"
#include "llpcBuilder.h"
#include "llpcContext.h"
#include "llpcIntrinsDefs.h"
#include "llpcPatchBufferOp.h"
#include "llpcPipelineShaders.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchBufferOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching for buffer operations
FunctionPass* CreatePatchBufferOp()
{
    return new PatchBufferOp();
}

// =====================================================================================================================
PatchBufferOp::PatchBufferOp()
    :
    FunctionPass(ID)
{
}

// =====================================================================================================================
// Get the analysis usage of this pass.
void PatchBufferOp::getAnalysisUsage(
    AnalysisUsage& analysisUsage // [out] The analysis usage.
    ) const
{
    analysisUsage.addRequired<LegacyDivergenceAnalysis>();
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addPreserved<PipelineShaders>();
    analysisUsage.addRequired<TargetTransformInfoWrapperPass>();
    analysisUsage.addPreserved<TargetTransformInfoWrapperPass>();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
bool PatchBufferOp::runOnFunction(
    Function& function) // [in,out] LLVM function to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Buffer-Op\n");

    m_pContext = static_cast<Context*>(&function.getContext());
    m_pBuilder.reset(new IRBuilder<>(*m_pContext));

    // Invoke visitation of the target instructions.
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();

    // If the function is not a valid shader stage, bail.
    if (pPipelineShaders->GetShaderStage(&function) == ShaderStageInvalid)
    {
        return false;
    }

    m_pDivergenceAnalysis = &getAnalysis<LegacyDivergenceAnalysis>();

    // To replace the fat pointer uses correctly we need to walk the basic blocks strictly in domination order to avoid
    // visiting a use of a fat pointer before it was actually defined.
    ReversePostOrderTraversal<Function*> traversal(&function);
    for (BasicBlock* const pBlock : traversal)
    {
        visit(*pBlock);
    }

    // Some instructions can modify the CFG and thus have to be performed after the normal visitors.
    for (Instruction* const pInst : m_postVisitInsts)
    {
        if (MemSetInst* const pMemSet = dyn_cast<MemSetInst>(pInst))
        {
            PostVisitMemSetInst(*pMemSet);
        }
        else if (MemCpyInst* const pMemCpy = dyn_cast<MemCpyInst>(pInst))
        {
            PostVisitMemCpyInst(*pMemCpy);
        }
    }
    m_postVisitInsts.clear();

    const bool changed = (m_replacementMap.empty() == false);

    for (auto& replaceMap : m_replacementMap)
    {
        Instruction* const pInst = dyn_cast<Instruction>(replaceMap.first);

        if (pInst == nullptr)
        {
            continue;
        }

        if (isa<StoreInst>(pInst) == false)
        {
            pInst->replaceAllUsesWith(UndefValue::get(pInst->getType()));
        }

        pInst->eraseFromParent();
    }

    m_replacementMap.clear();
    m_invariantSet.clear();
    m_divergenceSet.clear();

    return changed;
}

// =====================================================================================================================
// Visits "cmpxchg" instruction.
void PatchBufferOp::visitAtomicCmpXchgInst(
    AtomicCmpXchgInst& atomicCmpXchgInst) // [in] The instruction
{
    // If the type we are doing an atomic operation on is not a fat pointer, bail.
    if (atomicCmpXchgInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&atomicCmpXchgInst);

    Value* const pPointer = GetPointerOperandAsInst(atomicCmpXchgInst.getPointerOperand());

    Type* const pStoreType = atomicCmpXchgInst.getNewValOperand()->getType();

    const bool isSlc = atomicCmpXchgInst.getMetadata(LLVMContext::MD_nontemporal);

    Value* const pBufferDesc = m_replacementMap[pPointer].first;
    Value* const pBaseIndex = m_pBuilder->CreatePtrToInt(m_replacementMap[pPointer].second, m_pBuilder->getInt32Ty());
    CopyMetadata(pBaseIndex, &atomicCmpXchgInst);

    // If our buffer descriptor is divergent or is not a 32-bit integer, need to handle it differently.
    if ((m_divergenceSet.count(pBufferDesc) > 0) || (pStoreType->isIntegerTy(32) == false))
    {
        Value* const pBaseAddr = GetBaseAddressFromBufferDesc(pBufferDesc);

        // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
        Value* const pBound = m_pBuilder->CreateExtractElement(pBufferDesc, 2);
        Value* const pInBound = m_pBuilder->CreateICmpULT(pBaseIndex, pBound);
        Value* const pNewBaseIndex = m_pBuilder->CreateSelect(pInBound, pBaseIndex, m_pBuilder->getInt32(0));

        // Add on the index to the address.
        Value* pAtomicPointer = m_pBuilder->CreateGEP(pBaseAddr, pNewBaseIndex);

        pAtomicPointer = m_pBuilder->CreateBitCast(pAtomicPointer, pStoreType->getPointerTo(ADDR_SPACE_GLOBAL));

        const AtomicOrdering successOrdering = atomicCmpXchgInst.getSuccessOrdering();
        const AtomicOrdering failureOrdering = atomicCmpXchgInst.getFailureOrdering();

        Value* const pCompareValue = atomicCmpXchgInst.getCompareOperand();
        Value* const pNewValue = atomicCmpXchgInst.getNewValOperand();
        AtomicCmpXchgInst* const pNewAtomicCmpXchg = m_pBuilder->CreateAtomicCmpXchg(pAtomicPointer,
                                                                                     pCompareValue,
                                                                                     pNewValue,
                                                                                     successOrdering,
                                                                                     failureOrdering);
        pNewAtomicCmpXchg->setVolatile(atomicCmpXchgInst.isVolatile());
        pNewAtomicCmpXchg->setSyncScopeID(atomicCmpXchgInst.getSyncScopeID());
        pNewAtomicCmpXchg->setWeak(atomicCmpXchgInst.isWeak());
        CopyMetadata(pNewAtomicCmpXchg, &atomicCmpXchgInst);

        // Record the atomic instruction so we remember to delete it later.
        m_replacementMap[&atomicCmpXchgInst] = std::make_pair(nullptr, nullptr);

        atomicCmpXchgInst.replaceAllUsesWith(pNewAtomicCmpXchg);
    }
    else
    {
        switch (atomicCmpXchgInst.getSuccessOrdering())
        {
        case AtomicOrdering::Release:
        case AtomicOrdering::AcquireRelease:
        case AtomicOrdering::SequentiallyConsistent:
            {
                FenceInst* const pFence = m_pBuilder->CreateFence(AtomicOrdering::Release,
                                                                  atomicCmpXchgInst.getSyncScopeID());
                CopyMetadata(pFence, &atomicCmpXchgInst);
                break;
            }
        default:
            {
                break;
            }
        }

        Value* const pAtomicCall = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_atomic_cmpswap,
                                                               atomicCmpXchgInst.getNewValOperand()->getType(),
                                                               {
                                                                   atomicCmpXchgInst.getNewValOperand(),
                                                                   atomicCmpXchgInst.getCompareOperand(),
                                                                   pBufferDesc,
                                                                   pBaseIndex,
                                                                   m_pBuilder->getInt32(0),
                                                                   m_pBuilder->getInt32(isSlc ? 1 : 0)
                                                               });

        switch (atomicCmpXchgInst.getSuccessOrdering())
        {
        case AtomicOrdering::Acquire:
        case AtomicOrdering::AcquireRelease:
        case AtomicOrdering::SequentiallyConsistent:
            {
                FenceInst* const pFence = m_pBuilder->CreateFence(AtomicOrdering::Acquire,
                                                                  atomicCmpXchgInst.getSyncScopeID());
                CopyMetadata(pFence, &atomicCmpXchgInst);
                break;
            }
        default:
            {
                break;
            }
        }

        Value* pResultValue = UndefValue::get(atomicCmpXchgInst.getType());

        pResultValue = m_pBuilder->CreateInsertValue(pResultValue, pAtomicCall, static_cast<uint64_t>(0));
        CopyMetadata(pResultValue, &atomicCmpXchgInst);

        // NOTE: If we have a strong compare exchange, LLVM optimization will always set the compare result to "Equal".
        // Thus, we have to correct this behaviour and do the comparison by ourselves.
        if (atomicCmpXchgInst.isWeak() == false)
        {
            Value* const pValueEqual = m_pBuilder->CreateICmpEQ(pAtomicCall, atomicCmpXchgInst.getCompareOperand());
            CopyMetadata(pValueEqual, &atomicCmpXchgInst);

            pResultValue = m_pBuilder->CreateInsertValue(pResultValue, pValueEqual, static_cast<uint64_t>(1));
            CopyMetadata(pResultValue, &atomicCmpXchgInst);
        }

        // Record the atomic instruction so we remember to delete it later.
        m_replacementMap[&atomicCmpXchgInst] = std::make_pair(nullptr, nullptr);

        atomicCmpXchgInst.replaceAllUsesWith(pResultValue);
    }
}

// =====================================================================================================================
// Visits "atomicrmw" instruction.
void PatchBufferOp::visitAtomicRMWInst(
    AtomicRMWInst& atomicRmwInst) // [in] The instruction
{
    // If the type we are doing an atomic operation on is not a fat pointer, bail.
    if (atomicRmwInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&atomicRmwInst);

    Value* const pPointer = GetPointerOperandAsInst(atomicRmwInst.getPointerOperand());

    Type* const pStoreType = atomicRmwInst.getValOperand()->getType();

    const bool isSlc = atomicRmwInst.getMetadata(LLVMContext::MD_nontemporal);

    Value* const pBufferDesc = m_replacementMap[pPointer].first;
    Value* const pBaseIndex = m_pBuilder->CreatePtrToInt(m_replacementMap[pPointer].second, m_pBuilder->getInt32Ty());
    CopyMetadata(pBaseIndex, &atomicRmwInst);

    // If our buffer descriptor is divergent, need to handle it differently.
    if (m_divergenceSet.count(pBufferDesc) > 0)
    {
        Value* const pBaseAddr = GetBaseAddressFromBufferDesc(pBufferDesc);

        // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
        Value* const pBound = m_pBuilder->CreateExtractElement(pBufferDesc, 2);
        Value* const pInBound = m_pBuilder->CreateICmpULT(pBaseIndex, pBound);
        Value* const pNewBaseIndex = m_pBuilder->CreateSelect(pInBound, pBaseIndex, m_pBuilder->getInt32(0));

        // Add on the index to the address.
        Value* pAtomicPointer = m_pBuilder->CreateGEP(pBaseAddr, pNewBaseIndex);

        pAtomicPointer = m_pBuilder->CreateBitCast(pAtomicPointer, pStoreType->getPointerTo(ADDR_SPACE_GLOBAL));

        AtomicRMWInst* const pNewAtomicRmw = m_pBuilder->CreateAtomicRMW(atomicRmwInst.getOperation(),
                                                                         pAtomicPointer,
                                                                         atomicRmwInst.getValOperand(),
                                                                         atomicRmwInst.getOrdering());
        pNewAtomicRmw->setVolatile(atomicRmwInst.isVolatile());
        pNewAtomicRmw->setSyncScopeID(atomicRmwInst.getSyncScopeID());
        CopyMetadata(pNewAtomicRmw, &atomicRmwInst);

        // Record the atomic instruction so we remember to delete it later.
        m_replacementMap[&atomicRmwInst] = std::make_pair(nullptr, nullptr);

        atomicRmwInst.replaceAllUsesWith(pNewAtomicRmw);
    }
    else
    {
        switch (atomicRmwInst.getOrdering())
        {
        case AtomicOrdering::Release:
        case AtomicOrdering::AcquireRelease:
        case AtomicOrdering::SequentiallyConsistent:
            {
                FenceInst* const pFence = m_pBuilder->CreateFence(AtomicOrdering::Release, atomicRmwInst.getSyncScopeID());
                CopyMetadata(pFence, &atomicRmwInst);
                break;
            }
        default:
            {
                break;
            }
        }

        Intrinsic::ID intrinsic = Intrinsic::not_intrinsic;
        switch (atomicRmwInst.getOperation())
        {
        case AtomicRMWInst::Xchg:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_swap;
            break;
        case AtomicRMWInst::Add:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_add;
            break;
        case AtomicRMWInst::Sub:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_sub;
            break;
        case AtomicRMWInst::And:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_and;
            break;
        case AtomicRMWInst::Or:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_or;
            break;
        case AtomicRMWInst::Xor:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_xor;
            break;
        case AtomicRMWInst::Max:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_smax;
            break;
        case AtomicRMWInst::Min:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_smin;
            break;
        case AtomicRMWInst::UMax:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_umax;
            break;
        case AtomicRMWInst::UMin:
            intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_umin;
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        Value* const pAtomicCall = m_pBuilder->CreateIntrinsic(intrinsic,
                                                               cast<IntegerType>(pStoreType),
                                                               {
                                                                   atomicRmwInst.getValOperand(),
                                                                   pBufferDesc,
                                                                   pBaseIndex,
                                                                   m_pBuilder->getInt32(0),
                                                                   m_pBuilder->getInt32(isSlc * 2)
                                                               });
        CopyMetadata(pAtomicCall, &atomicRmwInst);

        switch (atomicRmwInst.getOrdering())
        {
        case AtomicOrdering::Acquire:
        case AtomicOrdering::AcquireRelease:
        case AtomicOrdering::SequentiallyConsistent:
            {
                FenceInst* const pFence = m_pBuilder->CreateFence(AtomicOrdering::Acquire, atomicRmwInst.getSyncScopeID());
                CopyMetadata(pFence, &atomicRmwInst);
                break;
            }
        default:
            {
                break;
            }
        }

        // Record the atomic instruction so we remember to delete it later.
        m_replacementMap[&atomicRmwInst] = std::make_pair(nullptr, nullptr);

        atomicRmwInst.replaceAllUsesWith(pAtomicCall);
    }
}

// =====================================================================================================================
// Visits "bitcast" instruction.
void PatchBufferOp::visitBitCastInst(
    BitCastInst& bitCastInst) // [in] The instruction
{
    Type* const pDestType = bitCastInst.getType();

    // If the type is not a pointer type, bail.
    if (pDestType->isPointerTy() == false)
    {
        return;
    }

    // If the pointer is not a fat pointer, bail.
    if (pDestType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&bitCastInst);

    Value* const pPointer = GetPointerOperandAsInst(bitCastInst.getOperand(0));

    Value* const pNewBitCast = m_pBuilder->CreateBitCast(m_replacementMap[pPointer].second,
                                                         GetRemappedType(bitCastInst.getDestTy()));

    CopyMetadata(pNewBitCast, pPointer);

    m_replacementMap[&bitCastInst] = std::make_pair(m_replacementMap[pPointer].first, pNewBitCast);
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchBufferOp::visitCallInst(
    CallInst& callInst) // [in] The instruction
{
    Function* const pCalledFunc = callInst.getCalledFunction();

    // If the call does not have a called function, bail.
    if (pCalledFunc == nullptr)
    {
        return;
    }

    const StringRef callName(pCalledFunc->getName());

    // If the call is not a late intrinsic call we need to replace, bail.
    if (callName.startswith(LlpcName::LaterCallPrefix) == false)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&callInst);

    if (callName.equals(LlpcName::LateLaunderFatPointer))
    {
        Constant* const pNullPointer = ConstantPointerNull::get(GetRemappedType(callInst.getType()));
        m_replacementMap[&callInst] = std::make_pair(callInst.getArgOperand(0), pNullPointer);

        // Check for any invariant starts that use the pointer.
        if (RemoveUsersForInvariantStarts(&callInst))
        {
            m_invariantSet.insert(callInst.getArgOperand(0));
        }

        // If the incoming index to the fat pointer launder was divergent, remember it.
        if (m_pDivergenceAnalysis->isDivergent(callInst.getArgOperand(0)))
        {
            m_divergenceSet.insert(callInst.getArgOperand(0));
        }
    }
    else if (callName.startswith(LlpcName::LateBufferLength))
    {
        Value* const pPointer = GetPointerOperandAsInst(callInst.getArgOperand(0));

        // Extract element 2 which is the NUM_RECORDS field from the buffer descriptor.
        Value* const pBufferLength = m_pBuilder->CreateExtractElement(m_replacementMap[pPointer].first, 2);
        // Record the call instruction so we remember to delete it later.
        m_replacementMap[&callInst] = std::make_pair(nullptr, nullptr);

        callInst.replaceAllUsesWith(pBufferLength);
    }
    else
    {
        LLPC_NEVER_CALLED();
    }
}

// =====================================================================================================================
// Visits "extractelement" instruction.
void PatchBufferOp::visitExtractElementInst(
    ExtractElementInst& extractElementInst) // [in] The instruction
{
    PointerType* const pPointerType = dyn_cast<PointerType>(extractElementInst.getType());

    // If the extract element is not extracting a pointer, bail.
    if (pPointerType == nullptr)
    {
        return;
    }

    // If the type we are GEPing into is not a fat pointer, bail.
    if (pPointerType->getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&extractElementInst);

    Value* const pPointer = GetPointerOperandAsInst(extractElementInst.getVectorOperand());
    Value* const pIndex = extractElementInst.getIndexOperand();

    Value* const pPointerElem = m_pBuilder->CreateExtractElement(m_replacementMap[pPointer].second, pIndex);
    CopyMetadata(pPointerElem, pPointer);

    m_replacementMap[&extractElementInst] = std::make_pair(m_replacementMap[pPointer].first, pPointerElem);
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
void PatchBufferOp::visitGetElementPtrInst(
    GetElementPtrInst& getElemPtrInst) // [in] The instruction
{
    // If the type we are GEPing into is not a fat pointer, bail.
    if (getElemPtrInst.getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&getElemPtrInst);

    Value* const pPointer = GetPointerOperandAsInst(getElemPtrInst.getPointerOperand());

    SmallVector<Value*, 8> indices(getElemPtrInst.idx_begin(), getElemPtrInst.idx_end());

    Value* pNewGetElemPtr = nullptr;

    if (getElemPtrInst.isInBounds())
    {
        pNewGetElemPtr = m_pBuilder->CreateInBoundsGEP(m_replacementMap[pPointer].second, indices);
    }
    else
    {
        pNewGetElemPtr = m_pBuilder->CreateGEP(m_replacementMap[pPointer].second, indices);
    }

    CopyMetadata(pNewGetElemPtr, pPointer);

    m_replacementMap[&getElemPtrInst] = std::make_pair(m_replacementMap[pPointer].first, pNewGetElemPtr);
}

// =====================================================================================================================
// Visits "insertelement" instruction.
void PatchBufferOp::visitInsertElementInst(
    InsertElementInst& insertElementInst) // [in] The instruction
{
    Type* const pType = insertElementInst.getType();

    // If the type is not a vector, bail.
    if (pType->isVectorTy() == false)
    {
        return;
    }

    PointerType* const pPointerType = dyn_cast<PointerType>(pType->getVectorElementType());

    // If the extract element is not extracting from a vector of pointers, bail.
    if (pPointerType == nullptr)
    {
        return;
    }

    // If the type we are GEPing into is not a fat pointer, bail.
    if (pPointerType->getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&insertElementInst);

    Value* const pPointer = GetPointerOperandAsInst(insertElementInst.getOperand(1));
    Value* const pIndex = m_replacementMap[pPointer].second;

    Value* pIndexVector = nullptr;

    if (isa<UndefValue>(insertElementInst.getOperand(0)))
    {
        pIndexVector = UndefValue::get(VectorType::get(pIndex->getType(), pType->getVectorNumElements()));
    }
    else
    {
        pIndexVector = m_replacementMap[GetPointerOperandAsInst(insertElementInst.getOperand(0))].second;
    }

    pIndexVector = m_pBuilder->CreateInsertElement(pIndexVector, pIndex, insertElementInst.getOperand(2));
    CopyMetadata(pIndexVector, pPointer);

    m_replacementMap[&insertElementInst] = std::make_pair(m_replacementMap[pPointer].first, pIndexVector);
}

// =====================================================================================================================
// Visits "load" instruction.
void PatchBufferOp::visitLoadInst(
    LoadInst& loadInst) // [in] The instruction
{
    const uint32_t addrSpace = loadInst.getPointerAddressSpace();

    if (addrSpace == ADDR_SPACE_CONST)
    {
        m_pBuilder->SetInsertPoint(&loadInst);

        Type* const pLoadType = loadInst.getType();

        // If the load is not a pointer type, bail.
        if (pLoadType->isPointerTy() == false)
        {
            return;
        }

        // If the address space of the loaded pointer is not a buffer fat pointer, bail.
        if (pLoadType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
        {
            return;
        }

        LLPC_ASSERT(loadInst.isVolatile() == false);
        LLPC_ASSERT(loadInst.getOrdering() == AtomicOrdering::NotAtomic);

        Type* const pCastType = m_pContext->Int32x4Ty()->getPointerTo(ADDR_SPACE_CONST);

        Value* const pPointer = GetPointerOperandAsInst(loadInst.getPointerOperand());

        Value* const pLoadPointer = m_pBuilder->CreateBitCast(pPointer, pCastType);

        LoadInst* const pNewLoad = m_pBuilder->CreateLoad(pLoadPointer);
        pNewLoad->setVolatile(loadInst.isVolatile());
        pNewLoad->setAlignment(MaybeAlign(loadInst.getAlignment()));
        pNewLoad->setOrdering(loadInst.getOrdering());
        pNewLoad->setSyncScopeID(loadInst.getSyncScopeID());
        CopyMetadata(pNewLoad, &loadInst);

        Constant* const pNullPointer = ConstantPointerNull::get(GetRemappedType(pLoadType));

        m_replacementMap[&loadInst] = std::make_pair(pNewLoad, pNullPointer);

        // If we removed an invariant load, remember that our new load is invariant.
        if (RemoveUsersForInvariantStarts(&loadInst))
        {
            m_invariantSet.insert(pNewLoad);
        }

        // If the original load was divergent, it means we are using descriptor indexing and need to remember it.
        if (m_pDivergenceAnalysis->isDivergent(&loadInst))
        {
            m_divergenceSet.insert(pNewLoad);
        }
    }
    else if (addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        Value* const pNewLoad = ReplaceLoad(&loadInst);
        CopyMetadata(pNewLoad, &loadInst);

        // Record the load instruction so we remember to delete it later.
        m_replacementMap[&loadInst] = std::make_pair(nullptr, nullptr);

        loadInst.replaceAllUsesWith(pNewLoad);
    }
}

// =====================================================================================================================
// Visits "memcpy" instruction.
void PatchBufferOp::visitMemCpyInst(
    MemCpyInst& memCpyInst) // [in] The memcpy instruction
{
    Value* const pDest = memCpyInst.getArgOperand(0);
    Value* const pSrc = memCpyInst.getArgOperand(1);

    const uint32_t destAddrSpace = pDest->getType()->getPointerAddressSpace();
    const uint32_t srcAddrSpace = pSrc->getType()->getPointerAddressSpace();

    // If either of the address spaces are fat pointers.
    if ((destAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER) ||
        (srcAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER))
    {
        // Handling memcpy requires us to modify the CFG, so we need to do it after the initial visit pass.
        m_postVisitInsts.push_back(&memCpyInst);
    }
}

// =====================================================================================================================
// Visits "memmove" instruction.
void PatchBufferOp::visitMemMoveInst(
    MemMoveInst& memMoveInst) // [in] The memmove instruction
{
    Value* const pDest = memMoveInst.getArgOperand(0);
    Value* const pSrc = memMoveInst.getArgOperand(1);

    const uint32_t destAddrSpace = pDest->getType()->getPointerAddressSpace();
    const uint32_t srcAddrSpace = pSrc->getType()->getPointerAddressSpace();

    // If either of the address spaces are not fat pointers, bail.
    if ((destAddrSpace != ADDR_SPACE_BUFFER_FAT_POINTER) &&
        (srcAddrSpace != ADDR_SPACE_BUFFER_FAT_POINTER))
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&memMoveInst);

    const uint32_t destAlignment = memMoveInst.getParamAlignment(0);
    const uint32_t srcAlignment = memMoveInst.getParamAlignment(1);

    // We assume LLVM is not introducing variable length mem moves.
    ConstantInt* const pLength = dyn_cast<ConstantInt>(memMoveInst.getArgOperand(2));
    LLPC_ASSERT(pLength != nullptr);

    // Get a vector type that is the length of the memmove.
    VectorType* const pMemoryType = VectorType::get(m_pBuilder->getInt8Ty(), pLength->getZExtValue());

    PointerType* const pCastDestType = pMemoryType->getPointerTo(destAddrSpace);
    Value* const pCastDest = m_pBuilder->CreateBitCast(pDest, pCastDestType);
    CopyMetadata(pCastDest, &memMoveInst);

    PointerType* const pCastSrcType = pMemoryType->getPointerTo(srcAddrSpace);
    Value* const pCastSrc = m_pBuilder->CreateBitCast(pSrc, pCastSrcType);
    CopyMetadata(pCastSrc, &memMoveInst);

    LoadInst* const pSrcLoad = m_pBuilder->CreateAlignedLoad(pCastSrc, srcAlignment);
    CopyMetadata(pSrcLoad, &memMoveInst);

    StoreInst* const pDestStore = m_pBuilder->CreateAlignedStore(pSrcLoad, pCastDest, destAlignment);
    CopyMetadata(pDestStore, &memMoveInst);

    // Record the memmove instruction so we remember to delete it later.
    m_replacementMap[&memMoveInst] = std::make_pair(nullptr, nullptr);

    // Visit the load and store instructions to fold away fat pointer load/stores we might have just created.
    if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastDest))
    {
        visitBitCastInst(*pCast);
    }

    if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastSrc))
    {
        visitBitCastInst(*pCast);
    }

    visitLoadInst(*pSrcLoad);
    visitStoreInst(*pDestStore);
}

// =====================================================================================================================
// Visits "memset" instruction.
void PatchBufferOp::visitMemSetInst(
    MemSetInst& memSetInst) // [in] The memset instruction
{
    Value* const pDest = memSetInst.getArgOperand(0);

    const uint32_t destAddrSpace = pDest->getType()->getPointerAddressSpace();

    // If the address spaces is a fat pointer.
    if (destAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        // Handling memset requires us to modify the CFG, so we need to do it after the initial visit pass.
        m_postVisitInsts.push_back(&memSetInst);
    }
}

// =====================================================================================================================
// Visits "phi" instruction.
void PatchBufferOp::visitPHINode(
    PHINode& phiNode) // [in] The phi node
{
    Type* const pType = phiNode.getType();

    // If the type is not a pointer type, bail.
    if (pType->isPointerTy() == false)
    {
        return;
    }

    // If the pointer is not a fat pointer, bail.
    if (pType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    SmallVector<Value*, 8> incomings;

    for (uint32_t i = 0, incomingValueCount = phiNode.getNumIncomingValues(); i < incomingValueCount; i++)
    {
        // PHIs require us to insert new incomings in the preceeding basic blocks.
        m_pBuilder->SetInsertPoint(phiNode.getIncomingBlock(i)->getTerminator());

        incomings.push_back(GetPointerOperandAsInst(phiNode.getIncomingValue(i)));
    }

    Value* pBufferDesc = nullptr;

    for (Value* const pIncoming : incomings)
    {
        Value* const pIncomingBufferDesc = m_replacementMap[pIncoming].first;

        if (pBufferDesc == nullptr)
        {
            pBufferDesc = pIncomingBufferDesc;
        }
        else if (pBufferDesc != pIncomingBufferDesc)
        {
            pBufferDesc = nullptr;
            break;
        }
    }

    m_pBuilder->SetInsertPoint(&phiNode);

    // If the buffer descriptor was null, it means the PHI is changing the buffer descriptor, and we need a new PHI.
    if (pBufferDesc == nullptr)
    {
        PHINode* const pNewPhiNode = m_pBuilder->CreatePHI(m_pContext->Int32x4Ty(), incomings.size());
        CopyMetadata(pNewPhiNode, &phiNode);

        bool isInvariant = true;
        bool isDivergent = false;

        for (BasicBlock* const pBlock : phiNode.blocks())
        {
            const int32_t blockIndex = phiNode.getBasicBlockIndex(pBlock);
            LLPC_ASSERT(blockIndex >= 0);

            Value* const pIncomingBufferDesc = m_replacementMap[incomings[blockIndex]].first;

            pNewPhiNode->addIncoming(pIncomingBufferDesc, pBlock);

            // If the incoming buffer descriptor is not invariant, the PHI cannot be marked invariant either.
            if (m_invariantSet.count(pIncomingBufferDesc) == 0)
            {
                isInvariant = false;
            }

            if ((m_divergenceSet.count(pIncomingBufferDesc) > 0) || m_pDivergenceAnalysis->isDivergent(&phiNode))
            {
                isDivergent = true;
            }
        }

        pBufferDesc = pNewPhiNode;

        if (isInvariant)
        {
            m_invariantSet.insert(pBufferDesc);
        }

        if (isDivergent)
        {
            m_divergenceSet.insert(pBufferDesc);
        }
    }

    PHINode* const pNewPhiNode = m_pBuilder->CreatePHI(GetRemappedType(phiNode.getType()), incomings.size());
    CopyMetadata(pNewPhiNode, &phiNode);

    m_replacementMap[&phiNode] = std::make_pair(pBufferDesc, pNewPhiNode);

    for (BasicBlock* const pBlock : phiNode.blocks())
    {
        const int32_t blockIndex = phiNode.getBasicBlockIndex(pBlock);
        LLPC_ASSERT(blockIndex >= 0);

        Value* pIncomingIndex = m_replacementMap[incomings[blockIndex]].second;

        if (pIncomingIndex == nullptr)
        {
            if (Instruction* const pInst = dyn_cast<Instruction>(incomings[blockIndex]))
            {
                visit(*pInst);
                pIncomingIndex = m_replacementMap[pInst].second;
            }
        }

        pNewPhiNode->addIncoming(pIncomingIndex, pBlock);
    }

    m_replacementMap[&phiNode] = std::make_pair(pBufferDesc, pNewPhiNode);
}

// =====================================================================================================================
// Visits "select" instruction.
void PatchBufferOp::visitSelectInst(
    SelectInst& selectInst) // [in] The select instruction
{
    Type* const pDestType = selectInst.getType();

    // If the type is not a pointer type, bail.
    if (pDestType->isPointerTy() == false)
    {
        return;
    }

    // If the pointer is not a fat pointer, bail.
    if (pDestType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&selectInst);

    Value* const pValue1 = GetPointerOperandAsInst(selectInst.getTrueValue());
    Value* const pValue2 = GetPointerOperandAsInst(selectInst.getFalseValue());

    Value* const pBufferDesc1 = m_replacementMap[pValue1].first;
    Value* const pBufferDesc2 = m_replacementMap[pValue2].first;

    Value* pBufferDesc = nullptr;

    if (pBufferDesc1 == pBufferDesc2)
    {
        // If the buffer descriptors are the same, then no select needed.
        pBufferDesc = pBufferDesc1;
    }
    else if ((pBufferDesc1 == nullptr) || (pBufferDesc2 == nullptr))
    {
        // Select the non-nullptr buffer descriptor
        pBufferDesc = pBufferDesc1 != nullptr ? pBufferDesc1 : pBufferDesc2;
    }
    else
    {
        // Otherwise we need to insert a select between the buffer descriptors.
        pBufferDesc = m_pBuilder->CreateSelect(selectInst.getCondition(), pBufferDesc1, pBufferDesc2);
        CopyMetadata(pBufferDesc, &selectInst);

        // If both incomings are invariant, mark the new select as invariant too.
        if ((m_invariantSet.count(pBufferDesc1) > 0) && (m_invariantSet.count(pBufferDesc2) > 0))
        {
            m_invariantSet.insert(pBufferDesc);
        }
    }

    Value* const pIndex1 = m_replacementMap[pValue1].second;
    Value* const pIndex2 = m_replacementMap[pValue2].second;

    Value* const pNewSelect = m_pBuilder->CreateSelect(selectInst.getCondition(), pIndex1, pIndex2);
    CopyMetadata(pNewSelect, &selectInst);

    m_replacementMap[&selectInst] = std::make_pair(pBufferDesc, pNewSelect);

    // If either of the incoming buffer descriptors are divergent, mark the new buffer descriptor as divergent too.
    if ((m_divergenceSet.count(pBufferDesc1) > 0) || (m_divergenceSet.count(pBufferDesc2) > 0))
    {
        m_divergenceSet.insert(pBufferDesc);
    }
    else if (m_pDivergenceAnalysis->isDivergent(&selectInst) && (pBufferDesc1 != pBufferDesc2))
    {
        // Otherwise is the selection is divergent and the buffer descriptors do not match, mark divergent.
        m_divergenceSet.insert(pBufferDesc);
    }
}

// =====================================================================================================================
// Visits "store" instruction.
void PatchBufferOp::visitStoreInst(
    StoreInst& storeInst) // [in] The instruction
{
    // If the address space of the store pointer is not a buffer fat pointer, bail.
    if (storeInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    ReplaceStore(&storeInst);

    // Record the store instruction so we remember to delete it later.
    m_replacementMap[&storeInst] = std::make_pair(nullptr, nullptr);
}

// =====================================================================================================================
// Visits "icmp" instruction.
void PatchBufferOp::visitICmpInst(
    ICmpInst& icmpInst) // [in] The instuction
{
    Type* const pType = icmpInst.getOperand(0)->getType();

    // If the type is not a pointer type, bail.
    if (pType->isPointerTy() == false)
    {
        return;
    }

    // If the pointer is not a fat pointer, bail.
    if (pType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    Value* const pNewICmp = ReplaceICmp(&icmpInst);

    CopyMetadata(pNewICmp, &icmpInst);

    // Record the icmp instruction so we remember to delete it later.
    m_replacementMap[&icmpInst] = std::make_pair(nullptr, nullptr);

    icmpInst.replaceAllUsesWith(pNewICmp);
}

// =====================================================================================================================
// Visits "ptrtoint" instruction.
void PatchBufferOp::visitPtrToIntInst(
    PtrToIntInst& ptrToIntInst) // [in] The "ptrtoint" instruction
{
    Type* const pType = ptrToIntInst.getOperand(0)->getType();

    // If the type is not a pointer type, bail.
    if (pType->isPointerTy() == false)
    {
        return;
    }

    // If the pointer is not a fat pointer, bail.
    if (pType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    {
        return;
    }

    m_pBuilder->SetInsertPoint(&ptrToIntInst);

    Value* const pPointer = GetPointerOperandAsInst(ptrToIntInst.getOperand(0));

    Value* const pNewPtrToInt = m_pBuilder->CreatePtrToInt(m_replacementMap[pPointer].second,
                                                           ptrToIntInst.getDestTy());

    CopyMetadata(pNewPtrToInt, pPointer);

    m_replacementMap[&ptrToIntInst] = std::make_pair(m_replacementMap[pPointer].first, pNewPtrToInt);

    ptrToIntInst.replaceAllUsesWith(pNewPtrToInt);
}

// =====================================================================================================================
// Post-process visits "memcpy" instruction.
void PatchBufferOp::PostVisitMemCpyInst(
    MemCpyInst& memCpyInst) // [in] The memcpy instruction
{
    Value* const pDest = memCpyInst.getArgOperand(0);
    Value* const pSrc = memCpyInst.getArgOperand(1);

    const uint32_t destAddrSpace = pDest->getType()->getPointerAddressSpace();
    const uint32_t srcAddrSpace = pSrc->getType()->getPointerAddressSpace();

    m_pBuilder->SetInsertPoint(&memCpyInst);

    const uint32_t destAlignment = memCpyInst.getParamAlignment(0);
    const uint32_t srcAlignment = memCpyInst.getParamAlignment(1);

    ConstantInt* const pConstantLength = dyn_cast<ConstantInt>(memCpyInst.getArgOperand(2));

    const uint64_t constantLength = (pConstantLength != nullptr) ? pConstantLength->getZExtValue() : 0;

    // NOTE: If we do not have a constant length, or the constant length is bigger than the minimum we require to
    // generate a loop, we make a loop to handle the memcpy instead. If we did not generate a loop here for any
    // constant-length memcpy with a large number of bytes would generate thousands of load/store instructions that
    // causes LLVM's optimizations and our AMDGPU backend to crawl (and generate worse code!).
    if ((pConstantLength == nullptr) || (constantLength > MinMemOpLoopBytes))
    {
        // NOTE: We want to perform our memcpy operation on the greatest stride of bytes possible (load/storing up to
        // DWORDx4 or 16 bytes per loop iteration). If we have a constant length, we check if the the alignment and
        // number of bytes to copy lets us load/store 16 bytes per loop iteration, and if not we check 8, then 4, then
        // 2. Worst case we have to load/store a single byte per loop.
        uint32_t stride = (pConstantLength == nullptr) ? 1 : 16;

        while (stride != 1)
        {
            // We only care about DWORD alignment (4 bytes) so clamp the max check here to that.
            const uint32_t minStride = std::min(stride, 4u);
            if ((destAlignment >= minStride) && (srcAlignment >= minStride) && ((constantLength % stride) == 0))
            {
                break;
            }

            stride /= 2;
        }

        Type* pCastDestType = nullptr;
        Type* pCastSrcType = nullptr;

        if (stride == 16)
        {
            pCastDestType = m_pContext->Int32x4Ty()->getPointerTo(destAddrSpace);
            pCastSrcType = m_pContext->Int32x4Ty()->getPointerTo(srcAddrSpace);
        }
        else
        {
            LLPC_ASSERT(stride < 8);
            pCastDestType = m_pBuilder->getIntNTy(stride * 8)->getPointerTo(destAddrSpace);
            pCastSrcType = m_pBuilder->getIntNTy(stride * 8)->getPointerTo(srcAddrSpace);
        }

        Value* pLength = memCpyInst.getArgOperand(2);

        Type* const pLengthType = pLength->getType();

        Value* const pIndex = MakeLoop(ConstantInt::get(pLengthType, 0),
                                       pLength,
                                       ConstantInt::get(pLengthType, stride),
                                       &memCpyInst);

        // Get the current index into our source pointer.
        Value* const pSrcPtr = m_pBuilder->CreateGEP(pSrc, pIndex);
        CopyMetadata(pSrcPtr, &memCpyInst);

        Value* const pCastSrc = m_pBuilder->CreateBitCast(pSrcPtr, pCastSrcType);
        CopyMetadata(pCastSrc, &memCpyInst);

        // Perform a load for the value.
        LoadInst* const pSrcLoad = m_pBuilder->CreateLoad(pCastSrc);
        CopyMetadata(pSrcLoad, &memCpyInst);

        // Get the current index into our destination pointer.
        Value* const pDestPtr = m_pBuilder->CreateGEP(pDest, pIndex);
        CopyMetadata(pDestPtr, &memCpyInst);

        Value* const pCastDest = m_pBuilder->CreateBitCast(pDestPtr, pCastDestType);
        CopyMetadata(pCastDest, &memCpyInst);

        // And perform a store for the value at this byte.
        StoreInst* const pDestStore = m_pBuilder->CreateStore(pSrcLoad, pCastDest);
        CopyMetadata(pDestStore, &memCpyInst);

        // Visit the newly added instructions to turn them into fat pointer variants.
        if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pSrcPtr))
        {
            visitGetElementPtrInst(*pGetElemPtr);
        }

        if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pDestPtr))
        {
            visitGetElementPtrInst(*pGetElemPtr);
        }

        if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastSrc))
        {
            visitBitCastInst(*pCast);
        }

        if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastDest))
        {
            visitBitCastInst(*pCast);
        }

        visitLoadInst(*pSrcLoad);

        visitStoreInst(*pDestStore);
    }
    else
    {
        // Get an vector type that is the length of the memcpy.
        VectorType* const pMemoryType = VectorType::get(m_pBuilder->getInt8Ty(), pConstantLength->getZExtValue());

        PointerType* const pCastDestType = pMemoryType->getPointerTo(destAddrSpace);
        Value* const pCastDest = m_pBuilder->CreateBitCast(pDest, pCastDestType);
        CopyMetadata(pCastDest, &memCpyInst);

        PointerType* const pCastSrcType = pMemoryType->getPointerTo(srcAddrSpace);
        Value* const pCastSrc = m_pBuilder->CreateBitCast(pSrc, pCastSrcType);
        CopyMetadata(pCastSrc, &memCpyInst);

        LoadInst* const pSrcLoad = m_pBuilder->CreateAlignedLoad(pCastSrc, srcAlignment);
        CopyMetadata(pSrcLoad, &memCpyInst);

        StoreInst* const pDestStore = m_pBuilder->CreateAlignedStore(pSrcLoad, pCastDest, destAlignment);
        CopyMetadata(pDestStore, &memCpyInst);

        // Visit the newly added instructions to turn them into fat pointer variants.
        if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastDest))
        {
            visitBitCastInst(*pCast);
        }

        if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastSrc))
        {
            visitBitCastInst(*pCast);
        }

        visitLoadInst(*pSrcLoad);
        visitStoreInst(*pDestStore);
    }

    // Record the memcpy instruction so we remember to delete it later.
    m_replacementMap[&memCpyInst] = std::make_pair(nullptr, nullptr);
}

// =====================================================================================================================
// Post-process visits "memset" instruction.
void PatchBufferOp::PostVisitMemSetInst(
    MemSetInst& memSetInst) // [in] The memset instruction
{
    Value* const pDest = memSetInst.getArgOperand(0);

    const uint32_t destAddrSpace = pDest->getType()->getPointerAddressSpace();

    m_pBuilder->SetInsertPoint(&memSetInst);

    Value* const pValue = memSetInst.getArgOperand(1);

    const uint32_t destAlignment = memSetInst.getParamAlignment(0);

    ConstantInt* const pConstantLength = dyn_cast<ConstantInt>(memSetInst.getArgOperand(2));

    const uint64_t constantLength = (pConstantLength != nullptr) ? pConstantLength->getZExtValue() : 0;

    // NOTE: If we do not have a constant length, or the constant length is bigger than the minimum we require to
    // generate a loop, we make a loop to handle the memcpy instead. If we did not generate a loop here for any
    // constant-length memcpy with a large number of bytes would generate thousands of load/store instructions that
    // causes LLVM's optimizations and our AMDGPU backend to crawl (and generate worse code!).
    if ((pConstantLength == nullptr) || (constantLength > MinMemOpLoopBytes))
    {
        // NOTE: We want to perform our memset operation on the greatest stride of bytes possible (load/storing up to
        // DWORDx4 or 16 bytes per loop iteration). If we have a constant length, we check if the the alignment and
        // number of bytes to copy lets us load/store 16 bytes per loop iteration, and if not we check 8, then 4, then
        // 2. Worst case we have to load/store a single byte per loop.
        uint32_t stride = (pConstantLength == nullptr) ? 1 : 16;

        while (stride != 1)
        {
            // We only care about DWORD alignment (4 bytes) so clamp the max check here to that.
            const uint32_t minStride = std::min(stride, 4u);
            if ((destAlignment >= minStride) && ((constantLength % stride) == 0))
            {
                break;
            }

            stride /= 2;
        }

        Type* pCastDestType = nullptr;

        if (stride == 16)
        {
            pCastDestType = m_pContext->Int32x4Ty()->getPointerTo(destAddrSpace);
        }
        else
        {
            LLPC_ASSERT(stride < 8);
            pCastDestType = m_pBuilder->getIntNTy(stride * 8)->getPointerTo(destAddrSpace);
        }

        Value* pNewValue = nullptr;

        if (Constant* const pConst = dyn_cast<Constant>(pValue))
        {
            pNewValue = ConstantVector::getSplat(stride, pConst);
            pNewValue = m_pBuilder->CreateBitCast(pNewValue, pCastDestType->getPointerElementType());
            CopyMetadata(pNewValue, &memSetInst);
        }
        else
        {
            Value* const pMemoryPointer = m_pBuilder->CreateAlloca(pCastDestType->getPointerElementType());
            CopyMetadata(pMemoryPointer, &memSetInst);

            Type* const pInt8PtrTy = m_pBuilder->getInt8Ty()->getPointerTo(ADDR_SPACE_PRIVATE);
            Value* const pCastMemoryPointer = m_pBuilder->CreateBitCast(pMemoryPointer, pInt8PtrTy);
            CopyMetadata(pCastMemoryPointer, &memSetInst);

            Value* const pMemSet = m_pBuilder->CreateMemSet(pCastMemoryPointer,
                                                            pValue,
                                                            stride,
                                                            1);
            CopyMetadata(pMemSet, &memSetInst);

            pNewValue = m_pBuilder->CreateLoad(pMemoryPointer);
            CopyMetadata(pNewValue, &memSetInst);
        }

        Value* const pLength = memSetInst.getArgOperand(2);

        Type* const pLengthType = pLength->getType();

        Value* const pIndex = MakeLoop(ConstantInt::get(pLengthType, 0),
                                       pLength,
                                       ConstantInt::get(pLengthType, stride),
                                       &memSetInst);

        // Get the current index into our destination pointer.
        Value* const pDestPtr = m_pBuilder->CreateGEP(pDest, pIndex);
        CopyMetadata(pDestPtr, &memSetInst);

        Value* const pCastDest = m_pBuilder->CreateBitCast(pDestPtr, pCastDestType);
        CopyMetadata(pCastDest, &memSetInst);

        // And perform a store for the value at this byte.
        StoreInst* const pDestStore = m_pBuilder->CreateStore(pNewValue, pCastDest);
        CopyMetadata(pDestStore, &memSetInst);

        if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pDestPtr))
        {
            visitGetElementPtrInst(*pGetElemPtr);
        }

        if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastDest))
        {
            visitBitCastInst(*pCast);
        }

        visitStoreInst(*pDestStore);
    }
    else
    {
        // Get a vector type that is the length of the memset.
        VectorType* const pMemoryType = VectorType::get(m_pBuilder->getInt8Ty(), pConstantLength->getZExtValue());

        Value* pNewValue = nullptr;

        if (Constant* const pConst = dyn_cast<Constant>(pValue))
        {
            pNewValue = ConstantVector::getSplat(pMemoryType->getVectorNumElements(), pConst);
        }
        else
        {
            Value* const pMemoryPointer = m_pBuilder->CreateAlloca(pMemoryType);
            CopyMetadata(pMemoryPointer, &memSetInst);

            Type* const pInt8PtrTy = m_pBuilder->getInt8Ty()->getPointerTo(ADDR_SPACE_PRIVATE);
            Value* const pCastMemoryPointer = m_pBuilder->CreateBitCast(pMemoryPointer, pInt8PtrTy);
            CopyMetadata(pCastMemoryPointer, &memSetInst);

            Value* const pMemSet = m_pBuilder->CreateMemSet(pCastMemoryPointer,
                                                            pValue,
                                                            pMemoryType->getVectorNumElements(),
                                                            1);
            CopyMetadata(pMemSet, &memSetInst);

            pNewValue = m_pBuilder->CreateLoad(pMemoryPointer);
            CopyMetadata(pNewValue, &memSetInst);
        }

        PointerType* const pCastDestType = pMemoryType->getPointerTo(destAddrSpace);
        Value* const pCastDest = m_pBuilder->CreateBitCast(pDest, pCastDestType);
        CopyMetadata(pCastDest, &memSetInst);

        if (BitCastInst* const pCast = dyn_cast<BitCastInst>(pCastDest))
        {
            visitBitCastInst(*pCast);
        }

        StoreInst* const pDestStore = m_pBuilder->CreateAlignedStore(pNewValue, pCastDest, destAlignment);
        CopyMetadata(pDestStore, &memSetInst);
        visitStoreInst(*pDestStore);
    }

    // Record the memset instruction so we remember to delete it later.
    m_replacementMap[&memSetInst] = std::make_pair(nullptr, nullptr);
}

// =====================================================================================================================
// Get a pointer operand as an instruction.
Value* PatchBufferOp::GetPointerOperandAsInst(
    Value* const pValue) // [in] The pointer operand value to get as an instruction.
{
    // If the value is already an instruction, return it.
    if (Instruction* const pInst = dyn_cast<Instruction>(pValue))
    {
        return pInst;
    }

    // If the value is a constant (i.e., null pointer), return it.
    if (isa<Constant>(pValue))
    {
        Constant* const pNullPointer = ConstantPointerNull::get(GetRemappedType(pValue->getType()));
        m_replacementMap[pValue] = std::make_pair(nullptr, pNullPointer);
        return pValue;
    }

    ConstantExpr* const pConstExpr = dyn_cast<ConstantExpr>(pValue);
    LLPC_ASSERT(pConstExpr != nullptr);

    Instruction* const pNewInst = m_pBuilder->Insert(pConstExpr->getAsInstruction());

    // Visit the new instruction we made to ensure we remap the value.
    visit(pNewInst);

    // Check that the new instruction was definitely in the replacement map.
    LLPC_ASSERT(m_replacementMap.count(pNewInst) > 0);

    return pNewInst;
}

// =====================================================================================================================
// Extract the 64-bit address from a buffer descriptor.
Value* PatchBufferOp::GetBaseAddressFromBufferDesc(
    Value* const pBufferDesc // [in] The buffer descriptor to extract the address from
    ) const
{
    Type* const pDescType = pBufferDesc->getType();

    LLPC_ASSERT(pDescType->isVectorTy());
    LLPC_ASSERT(pDescType->getVectorNumElements() == 4);
    LLPC_ASSERT(pDescType->getVectorElementType()->isIntegerTy(32));

    // Get the base address of our buffer by extracting the two components with the 48-bit address, and masking.
    Value* pBaseAddr = m_pBuilder->CreateShuffleVector(pBufferDesc, UndefValue::get(pDescType), { 0, 1 });
    Value* const pBaseAddrMask = ConstantVector::get({
                                                        m_pBuilder->getInt32(0xFFFFFFFF),
                                                        m_pBuilder->getInt32(0xFFFF)
                                                     });
    pBaseAddr = m_pBuilder->CreateAnd(pBaseAddr, pBaseAddrMask);
    pBaseAddr = m_pBuilder->CreateBitCast(pBaseAddr, m_pBuilder->getInt64Ty());
    return m_pBuilder->CreateIntToPtr(pBaseAddr, m_pBuilder->getInt8Ty()->getPointerTo(ADDR_SPACE_GLOBAL));
}

// =====================================================================================================================
// Copy all metadata from one value to another.
void PatchBufferOp::CopyMetadata(
    Value* const       pDest, // [in/out] The destination to copy metadata onto.
    const Value* const pSrc   // [in] The source to copy metadata from.
    ) const
{
    Instruction* const pDestInst = dyn_cast<Instruction>(pDest);

    // If the destination is not an instruction, bail.
    if (pDestInst == nullptr)
    {
        return;
    }

    const Instruction* const pSrcInst = dyn_cast<Instruction>(pSrc);

    // If the source is not an instruction, bail.
    if (pSrcInst == nullptr)
    {
        return;
    }

    SmallVector<std::pair<uint32_t, MDNode*>, 8> allMetaNodes;
    pSrcInst->getAllMetadata(allMetaNodes);

    for (auto metaNode : allMetaNodes)
    {
        pDestInst->setMetadata(metaNode.first, metaNode.second);
    }
}

// =====================================================================================================================
// Get the remapped type for a fat pointer that is usable in indexing. We use the 32-bit wide constant address space for
// this, as it means when we convert the GEP to an integer, the GEP can be converted losslessly to a 32-bit integer,
// which just happens to be what the MUBUF instructions expect.
PointerType* PatchBufferOp::GetRemappedType(
    Type* const pType // [in] The type to remap.
    ) const
{
    LLPC_ASSERT(pType->isPointerTy());
    return pType->getPointerElementType()->getPointerTo(ADDR_SPACE_CONST_32BIT);
}

// =====================================================================================================================
// Remove any users that are invariant starts, returning if any were removed.
bool PatchBufferOp::RemoveUsersForInvariantStarts(
    Value* const pValue) // [in] The value to check the users of.
{
    bool modified = false;

    for (User* const pUser : pValue->users())
    {
        if (BitCastInst* const pBitCast = dyn_cast<BitCastInst>(pUser))
        {
            // Remove any users of the bitcast too.
            if (RemoveUsersForInvariantStarts(pBitCast))
            {
                modified = true;
            }
        }
        else
        {
            IntrinsicInst* const pIntrinsic = dyn_cast<IntrinsicInst>(pUser);

            // If the user isn't an intrinsic, bail.
            if (pIntrinsic == nullptr)
            {
                continue;
            }

            // If the intrinsic is not an invariant load, bail.
            if (pIntrinsic->getIntrinsicID() != Intrinsic::invariant_start)
            {
                continue;
            }

            // Remember the intrinsic because we will want to delete it.
            m_replacementMap[pIntrinsic] = std::make_pair(nullptr, nullptr);

            modified = true;
        }
    }

    return modified;
}

// =====================================================================================================================
// Replace a fat pointer load with the intrinsics required to do the load.
Value* PatchBufferOp::ReplaceLoad(
    LoadInst* const pLoadInst) // [in] The load instruction to replace.
{
    m_pBuilder->SetInsertPoint(pLoadInst);

    Value* const pPointer = GetPointerOperandAsInst(pLoadInst->getPointerOperand());

    const DataLayout& dataLayout = m_pBuilder->GetInsertBlock()->getModule()->getDataLayout();

    Type* const pLoadType = pLoadInst->getType();

    const uint32_t bytesToLoad = static_cast<uint32_t>(dataLayout.getTypeSizeInBits(pLoadType) / 8);

    uint32_t alignment = pLoadInst->getAlignment();

    if (alignment == 0)
    {
        alignment = dataLayout.getABITypeAlignment(pLoadType);
    }

    const bool isInvariant = (m_invariantSet.count(m_replacementMap[pPointer].first) > 0) ||
                             pLoadInst->getMetadata(LLVMContext::MD_invariant_load);
    const bool isSlc = pLoadInst->getMetadata(LLVMContext::MD_nontemporal);
    const bool isGlc = pLoadInst->getOrdering() != AtomicOrdering::NotAtomic;
#if LLPC_BUILD_GFX10
    const bool isDlc = isGlc; // For buffer load on GFX10+, we set DLC = GLC
#endif

    Value* const pBufferDesc = m_replacementMap[pPointer].first;
    Value* const pBaseIndex = m_pBuilder->CreatePtrToInt(m_replacementMap[pPointer].second, m_pBuilder->getInt32Ty());

    // If our buffer descriptor is divergent, need to handle that differently.
    if (m_divergenceSet.count(pBufferDesc) > 0)
    {
        Value* const pBaseAddr = GetBaseAddressFromBufferDesc(pBufferDesc);

        // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
        Value* const pBound = m_pBuilder->CreateExtractElement(pBufferDesc, 2);
        Value* const pInBound = m_pBuilder->CreateICmpULT(pBaseIndex, pBound);
        Value* const pNewBaseIndex = m_pBuilder->CreateSelect(pInBound, pBaseIndex, m_pBuilder->getInt32(0));

        // Add on the index to the address.
        Value* pLoadPointer = m_pBuilder->CreateGEP(pBaseAddr, pNewBaseIndex);

        pLoadPointer = m_pBuilder->CreateBitCast(pLoadPointer, pLoadType->getPointerTo(ADDR_SPACE_GLOBAL));

        LoadInst* const pNewLoad = m_pBuilder->CreateLoad(pLoadPointer);
        pNewLoad->setVolatile(pLoadInst->isVolatile());
        pNewLoad->setAlignment(MaybeAlign(pLoadInst->getAlignment()));
        pNewLoad->setOrdering(pLoadInst->getOrdering());
        pNewLoad->setSyncScopeID(pLoadInst->getSyncScopeID());
        CopyMetadata(pNewLoad, pLoadInst);

        if (isInvariant)
        {
            pNewLoad->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_pContext, None));
        }

        return pNewLoad;
    }

    switch (pLoadInst->getOrdering())
    {
    case AtomicOrdering::Release:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent:
        m_pBuilder->CreateFence(AtomicOrdering::Release, pLoadInst->getSyncScopeID());
        break;
    default:
        break;
    }

    SmallVector<Value*, 8> partLoads;

    uint32_t remainingBytes = bytesToLoad;

    // If the alignment is at least 4, we can use the most efficient dword loads.
    if (alignment >= 4)
    {
        while (remainingBytes >= 4)
        {
            const uint32_t offset = bytesToLoad - remainingBytes;
            Value* const pOffset = (offset == 0) ?
                                   pBaseIndex :
                                   m_pBuilder->CreateAdd(pBaseIndex, m_pBuilder->getInt32(offset));

            Type* pIntLoadType = nullptr;
            Type* pFloatLoadType = nullptr;

            if (remainingBytes >= 16)
            {
                pIntLoadType = m_pContext->Int32x4Ty();
                pFloatLoadType = m_pContext->Floatx4Ty();
                remainingBytes -= 16;
            }
            else if (remainingBytes >= 8)
            {
                pIntLoadType = m_pContext->Int32x2Ty();
                pFloatLoadType = m_pContext->Floatx2Ty();
                remainingBytes -= 8;
            }
            else if (remainingBytes >= 4)
            {
                pIntLoadType = m_pContext->Int32Ty();
                pFloatLoadType = m_pContext->FloatTy();
                remainingBytes -= 4;
            }
            else
            {
                LLPC_NEVER_CALLED();
            }

            Value* pPartLoad = nullptr;

            if (isInvariant)
            {
                CoherentFlag coherent = {};
                coherent.bits.glc = isGlc;
#if LLPC_BUILD_GFX10
                if (m_pContext->GetGfxIpVersion().major >= 10)
                {
                    coherent.bits.dlc = isDlc;
                }
#endif
                pPartLoad = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_buffer_load,
                                                        pIntLoadType,
                                                        {
                                                            pBufferDesc,
                                                            pOffset,
                                                            m_pBuilder->getInt32(coherent.u32All)
                                                        });
            }
            else
            {
                CoherentFlag coherent = {};
                coherent.bits.glc = isGlc;
                coherent.bits.slc = isSlc;
#if LLPC_BUILD_GFX10
                if (m_pContext->GetGfxIpVersion().major >= 10)
                {
                    coherent.bits.dlc = isDlc;
                }
#endif
                pPartLoad = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load,
                                                        pFloatLoadType,
                                                        {
                                                            pBufferDesc,
                                                            pOffset,
                                                            m_pBuilder->getInt32(0),
                                                            m_pBuilder->getInt32(coherent.u32All)
                                                        });
                pPartLoad = m_pBuilder->CreateBitCast(pPartLoad, pIntLoadType);
            }

            CopyMetadata(pPartLoad, pLoadInst);
            partLoads.push_back(pPartLoad);
        }
    }

    // If the alignment is at least 2, we can use ushort loads next.
    if (alignment >= 2)
    {
        for (; remainingBytes >= 2; remainingBytes -= 2)
        {
            const uint32_t offset = bytesToLoad - remainingBytes;
            Value* const pOffset = (offset == 0) ?
                                   pBaseIndex :
                                   m_pBuilder->CreateAdd(pBaseIndex, m_pBuilder->getInt32(offset));

            Value* pPartLoad = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_buffer_load_ushort,
                                                           {},
                                                           {
                                                               pBufferDesc,
                                                               m_pBuilder->getInt32(0),
                                                               pOffset,
                                                               m_pBuilder->getInt1(isGlc),
                                                               m_pBuilder->getInt1(isSlc)
                                                           });
            CopyMetadata(pPartLoad, pLoadInst);
            pPartLoad = m_pBuilder->CreateBitCast(pPartLoad, VectorType::get(m_pBuilder->getInt16Ty(), 2));
            CopyMetadata(pPartLoad, pLoadInst);
            pPartLoad = m_pBuilder->CreateExtractElement(pPartLoad, static_cast<uint64_t>(0));
            CopyMetadata(pPartLoad, pLoadInst);
            partLoads.push_back(pPartLoad);
        }
    }

    for (; remainingBytes >= 1; remainingBytes -= 1)
    {
        const uint32_t offset = bytesToLoad - remainingBytes;
        Value* const pOffset = (offset == 0) ?
                               pBaseIndex :
                               m_pBuilder->CreateAdd(pBaseIndex, m_pBuilder->getInt32(offset));

        Value* pPartLoad = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_buffer_load_ubyte,
                                                       {},
                                                       {
                                                           pBufferDesc,
                                                           m_pBuilder->getInt32(0),
                                                           pOffset,
                                                           m_pBuilder->getInt1(isGlc),
                                                           m_pBuilder->getInt1(isSlc)
                                                       });
        CopyMetadata(pPartLoad, pLoadInst);
        pPartLoad = m_pBuilder->CreateBitCast(pPartLoad, VectorType::get(m_pBuilder->getInt8Ty(), 4));
        CopyMetadata(pPartLoad, pLoadInst);
        pPartLoad = m_pBuilder->CreateExtractElement(pPartLoad, static_cast<uint64_t>(0));
        CopyMetadata(pPartLoad, pLoadInst);
        partLoads.push_back(pPartLoad);
    }

    // The last element in the array has the smallest type we used to load with, so get that.
    Type* pSmallestType = partLoads.back()->getType();

    // And if the type was a vector, we do our insert elements on the elements of it.
    if (pSmallestType->isVectorTy())
    {
        pSmallestType = pSmallestType->getVectorElementType();
    }

    // Get the byte size of the smallest type.
    const uint32_t smallestByteSize = static_cast<uint32_t>(dataLayout.getTypeSizeInBits(pSmallestType) / 8);

    // And create an undef vector whose total size is the number of bytes we loaded.
    Value* pNewLoad = UndefValue::get(VectorType::get(pSmallestType, bytesToLoad / smallestByteSize));

    uint32_t index = 0;

    for (Value* pPartLoad : partLoads)
    {
        // Get the byte size of our load part.
        const uint32_t byteSize = static_cast<uint32_t>(dataLayout.getTypeSizeInBits(pPartLoad->getType()) / 8);

        // Bitcast it to a vector of the smallest load type.
        VectorType* const pCastType = VectorType::get(pSmallestType, byteSize / smallestByteSize);
        pPartLoad = m_pBuilder->CreateBitCast(pPartLoad, pCastType);
        CopyMetadata(pPartLoad, pLoadInst);

        // Run through our the elements of our bitcasted type and insert them into the main load.
        for (uint32_t i = 0, compCount = static_cast<uint32_t>(pCastType->getNumElements()); i < compCount; i++)
        {
            Value* const pLoadElem = m_pBuilder->CreateExtractElement(pPartLoad, i);
            CopyMetadata(pLoadElem, pLoadInst);
            pNewLoad = m_pBuilder->CreateInsertElement(pNewLoad, pLoadElem, index++);
            CopyMetadata(pNewLoad, pLoadInst);
        }
    }

    if (pLoadType->isPointerTy())
    {
        pNewLoad = m_pBuilder->CreateBitCast(pNewLoad, m_pBuilder->getIntNTy(bytesToLoad * 8));
        CopyMetadata(pNewLoad, pLoadInst);
        pNewLoad = m_pBuilder->CreateIntToPtr(pNewLoad, pLoadType);
        CopyMetadata(pNewLoad, pLoadInst);
    }
    else
    {
        pNewLoad = m_pBuilder->CreateBitCast(pNewLoad, pLoadType);
        CopyMetadata(pNewLoad, pLoadInst);
    }

    switch (pLoadInst->getOrdering())
    {
    case AtomicOrdering::Acquire:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent:
        m_pBuilder->CreateFence(AtomicOrdering::Acquire, pLoadInst->getSyncScopeID());
        break;
    default:
        break;
    }

    return pNewLoad;
}

// =====================================================================================================================
// Replace a fat pointer store with the intrinsics required to do the store.
void PatchBufferOp::ReplaceStore(
    StoreInst* const pStoreInst) // [in] The store instruction to replace.
{
    m_pBuilder->SetInsertPoint(pStoreInst);

    Value* const pPointer = GetPointerOperandAsInst(pStoreInst->getPointerOperand());

    const DataLayout& dataLayout = m_pBuilder->GetInsertBlock()->getModule()->getDataLayout();

    Type* const pStoreType = pStoreInst->getValueOperand()->getType();

    const uint32_t bytesToStore = static_cast<uint32_t>(dataLayout.getTypeSizeInBits(pStoreType) / 8);

    uint32_t alignment = pStoreInst->getAlignment();

    if (alignment == 0)
    {
        alignment = dataLayout.getABITypeAlignment(pStoreType);
    }

    const bool isSlc = pStoreInst->getMetadata(LLVMContext::MD_nontemporal);
    const bool isGlc = pStoreInst->getOrdering() != AtomicOrdering::NotAtomic;

    Value* const pBufferDesc = m_replacementMap[pPointer].first;
    Value* const pBaseIndex = m_pBuilder->CreatePtrToInt(m_replacementMap[pPointer].second, m_pBuilder->getInt32Ty());

    // If our buffer descriptor is divergent, need to handle that differently.
    if (m_divergenceSet.count(pBufferDesc) > 0)
    {
        Value* const pBaseAddr = GetBaseAddressFromBufferDesc(pBufferDesc);

        // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
        Value* const pBound = m_pBuilder->CreateExtractElement(pBufferDesc, 2);
        Value* const pInBound = m_pBuilder->CreateICmpULT(pBaseIndex, pBound);
        Value* const pNewBaseIndex = m_pBuilder->CreateSelect(pInBound, pBaseIndex, m_pBuilder->getInt32(0));

        // Add on the index to the address.
        Value* pStorePointer = m_pBuilder->CreateGEP(pBaseAddr, pNewBaseIndex);

        pStorePointer = m_pBuilder->CreateBitCast(pStorePointer, pStoreType->getPointerTo(ADDR_SPACE_GLOBAL));

        StoreInst* const pNewStore = m_pBuilder->CreateStore(pStoreInst->getValueOperand(), pStorePointer);
        pNewStore->setVolatile(pStoreInst->isVolatile());
        pNewStore->setAlignment(MaybeAlign(pStoreInst->getAlignment()));
        pNewStore->setOrdering(pStoreInst->getOrdering());
        pNewStore->setSyncScopeID(pStoreInst->getSyncScopeID());
        CopyMetadata(pNewStore, pStoreInst);
    }
    else
    {
        switch (pStoreInst->getOrdering())
        {
        case AtomicOrdering::Release:
        case AtomicOrdering::AcquireRelease:
        case AtomicOrdering::SequentiallyConsistent:
            m_pBuilder->CreateFence(AtomicOrdering::Release, pStoreInst->getSyncScopeID());
            break;
        default:
            break;
        }

        uint32_t smallestByteSize = 4;

        if ((alignment < 2) || ((bytesToStore & 0x1) != 0))
        {
            smallestByteSize = 1;
        }
        else if ((alignment < 4) || ((bytesToStore & 0x3) != 0))
        {
            smallestByteSize = 2;
        }
        else
        {
            smallestByteSize = 4;
        }

        Type* pSmallestType = nullptr;

        switch (smallestByteSize)
        {
        case 1:
            pSmallestType = m_pBuilder->getInt8Ty();
            break;
        case 2:
            pSmallestType = m_pBuilder->getInt16Ty();
            break;
        case 4:
            pSmallestType = m_pBuilder->getInt32Ty();
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        // Bitcast our value-to-store to a vector of smallest byte size.
        Type* const pCastType = VectorType::get(pSmallestType, bytesToStore / smallestByteSize);

        Value* pStoreValue = pStoreInst->getValueOperand();

        if (pStoreValue->getType()->isPointerTy())
        {
            pStoreValue = m_pBuilder->CreatePtrToInt(pStoreValue, m_pBuilder->getIntNTy(bytesToStore * 8));
            CopyMetadata(pStoreValue, pStoreInst);
        }

        pStoreValue = m_pBuilder->CreateBitCast(pStoreValue, pCastType);
        CopyMetadata(pStoreValue, pStoreInst);

        uint32_t index = 0;

        SmallVector<Value*, 8> partStores;

        uint32_t remainingBytes = bytesToStore;

        // If the alignment is at least 4, we can use the most efficient dword stores.
        if (alignment >= 4)
        {
            while (remainingBytes >= 4)
            {
                uint32_t partStoreWidth = 0;

                if (remainingBytes >= 16)
                {
                    partStoreWidth = 16;
                }
                else if (remainingBytes >= 8)
                {
                    partStoreWidth = 8;
                }
                else if (remainingBytes >= 4)
                {
                    partStoreWidth = 4;
                }
                else
                {
                    LLPC_NEVER_CALLED();
                }

                Value* pPartStore = UndefValue::get(VectorType::get(pSmallestType, partStoreWidth / smallestByteSize));

                for (uint32_t i = 0, compCount = pPartStore->getType()->getVectorNumElements(); i < compCount; i++)
                {
                    Value* const pStoreElem = m_pBuilder->CreateExtractElement(pStoreValue, index++);
                    pPartStore = m_pBuilder->CreateInsertElement(pPartStore, pStoreElem, i);
                }

                Type* pCastType = nullptr;

                if (remainingBytes >= 16)
                {
                    pCastType = m_pContext->Int32x4Ty();
                    remainingBytes -= 16;
                }
                else if (remainingBytes >= 8)
                {
                    pCastType = m_pContext->Int32x2Ty();
                    remainingBytes -= 8;
                }
                else if (remainingBytes >= 4)
                {
                    pCastType = m_pContext->Int32Ty();
                    remainingBytes -= 4;
                }
                else
                {
                    LLPC_NEVER_CALLED();
                }

                pPartStore = m_pBuilder->CreateBitCast(pPartStore, pCastType);
                CopyMetadata(pPartStore, pStoreInst);
                partStores.push_back(pPartStore);
            }
        }

        // If the alignment is at least 2, we can use ushort stores next.
        if (alignment >= 2)
        {
            for (; remainingBytes >= 2; remainingBytes -= 2)
            {
                Value* pPartStore = UndefValue::get(VectorType::get(pSmallestType, 2 / smallestByteSize));

                for (uint32_t i = 0, compCount = pPartStore->getType()->getVectorNumElements(); i < compCount; i++)
                {
                    Value* const pStoreElem = m_pBuilder->CreateExtractElement(pStoreValue, index++);
                    pPartStore = m_pBuilder->CreateInsertElement(pPartStore, pStoreElem, i);
                }

                pPartStore = m_pBuilder->CreateBitCast(pPartStore, m_pBuilder->getInt16Ty());
                CopyMetadata(pPartStore, pStoreInst);
                partStores.push_back(pPartStore);
            }
        }

        // Otherwise use ubyte stores.
        for (; remainingBytes >= 1; remainingBytes -= 1)
        {
            Value* pPartStore = m_pBuilder->CreateExtractElement(pStoreValue, index++);
            CopyMetadata(pPartStore, pStoreInst);
            pPartStore = m_pBuilder->CreateBitCast(pPartStore, m_pBuilder->getInt8Ty());
            CopyMetadata(pPartStore, pStoreInst);
            partStores.push_back(pPartStore);
        }

        uint32_t offset = 0;

        for (Value* pPartStore : partStores)
        {
            // Get the byte size of our store part.
            const uint32_t byteSize = static_cast<uint32_t>(dataLayout.getTypeSizeInBits(pPartStore->getType()) / 8);

            Value* const pOffset = (offset == 0) ?
                                   pBaseIndex :
                                   m_pBuilder->CreateAdd(pBaseIndex, m_pBuilder->getInt32(offset));

            if (byteSize >= 4)
            {
                const uint32_t elements = byteSize / 4;
                Type* pCastType = m_pBuilder->getFloatTy();
                if (elements > 1)
                {
                    pCastType = VectorType::get(pCastType, elements);
                }

                pPartStore = m_pBuilder->CreateBitCast(pPartStore, pCastType);
                CopyMetadata(pPartStore, pStoreInst);

                CoherentFlag coherent = {};
                coherent.bits.glc = isGlc;
                coherent.bits.slc = isSlc;

                Value* const pNewStore = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_store,
                                                                     pCastType,
                                                                     {
                                                                         pPartStore,
                                                                         pBufferDesc,
                                                                         pOffset,
                                                                         m_pBuilder->getInt32(0),
                                                                         m_pBuilder->getInt32(coherent.u32All)
                                                                     });
                CopyMetadata(pNewStore, pStoreInst);
            }
            else if (byteSize == 2)
            {
                pPartStore = m_pBuilder->CreateBitCast(pPartStore, VectorType::get(m_pBuilder->getInt8Ty(), 2));
                CopyMetadata(pPartStore, pStoreInst);

                UndefValue* const pUndef = UndefValue::get(VectorType::get(m_pBuilder->getInt8Ty(), 2));
                pPartStore = m_pBuilder->CreateShuffleVector(pPartStore, pUndef, { 0, 1, 2, 2 });
                CopyMetadata(pPartStore, pStoreInst);

                pPartStore = m_pBuilder->CreateBitCast(pPartStore, m_pContext->FloatTy());
                CopyMetadata(pPartStore, pStoreInst);

                Value* const pNewStore = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_buffer_store_short,
                                                                     {},
                                                                     {
                                                                         pPartStore,
                                                                         pBufferDesc,
                                                                         m_pBuilder->getInt32(0),
                                                                         pOffset,
                                                                         m_pBuilder->getInt1(isGlc),
                                                                         m_pBuilder->getInt1(isSlc)
                                                                     });
                CopyMetadata(pNewStore, pStoreInst);
            }
            else
            {
                UndefValue* pUndef = UndefValue::get(VectorType::get(m_pBuilder->getInt8Ty(), 4));
                pPartStore = m_pBuilder->CreateInsertElement(pUndef, pPartStore, static_cast<uint64_t>(0));
                CopyMetadata(pPartStore, pStoreInst);

                pPartStore = m_pBuilder->CreateBitCast(pPartStore, m_pContext->FloatTy());
                CopyMetadata(pPartStore, pStoreInst);

                Value* const pNewStore = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_buffer_store_byte,
                                                                     {},
                                                                     {
                                                                         pPartStore,
                                                                         pBufferDesc,
                                                                         m_pBuilder->getInt32(0),
                                                                         pOffset,
                                                                         m_pBuilder->getInt1(isGlc),
                                                                         m_pBuilder->getInt1(isSlc)
                                                                     });
                CopyMetadata(pNewStore, pStoreInst);
            }

            offset += byteSize;
        }

        switch (pStoreInst->getOrdering())
        {
        case AtomicOrdering::Acquire:
        case AtomicOrdering::AcquireRelease:
        case AtomicOrdering::SequentiallyConsistent:
            m_pBuilder->CreateFence(AtomicOrdering::Acquire, pStoreInst->getSyncScopeID());
            break;
        default:
            break;
        }
    }
}

// =====================================================================================================================
// Replace fat pointers icmp with the instruction required to do the icmp.
Value* PatchBufferOp::ReplaceICmp(
    ICmpInst* const pICmpInst) // [in] The "icmp" instruction to replace.
{
    m_pBuilder->SetInsertPoint(pICmpInst);

    SmallVector<Value*, 2> bufferDescs;
    SmallVector<Value*, 2> indices;
    for (int i = 0; i < 2; ++i)
    {
        Value* const pOperand = GetPointerOperandAsInst(pICmpInst->getOperand(i));
        bufferDescs.push_back(m_replacementMap[pOperand].first);
        indices.push_back(m_pBuilder->CreatePtrToInt(m_replacementMap[pOperand].second, m_pBuilder->getInt32Ty()));
    }

    Type* const pBufferDescTy = bufferDescs[0]->getType();

    LLPC_ASSERT(pBufferDescTy->isVectorTy());
    LLPC_ASSERT(pBufferDescTy->getVectorNumElements() == 4);
    LLPC_ASSERT(pBufferDescTy->getVectorElementType()->isIntegerTy(32));
    LLPC_UNUSED(pBufferDescTy);
    LLPC_ASSERT((pICmpInst->getPredicate() == ICmpInst::ICMP_EQ) || (pICmpInst->getPredicate() == ICmpInst::ICMP_NE));

    Value* pBufferDescICmp = m_pBuilder->getFalse();
    if ((bufferDescs[0] == nullptr) && (bufferDescs[1] == nullptr))
    {
        pBufferDescICmp = m_pBuilder->getTrue();
    }
    else if ((bufferDescs[0] != nullptr) && (bufferDescs[1] != nullptr))
    {
        Value* const pBufferDescEqual = m_pBuilder->CreateICmpEQ(bufferDescs[0], bufferDescs[1]);

        pBufferDescICmp = m_pBuilder->CreateExtractElement(pBufferDescEqual, static_cast<uint64_t>(0));
        for (uint32_t i = 1; i < 4; ++i)
        {
            Value* pBufferDescElemEqual = m_pBuilder->CreateExtractElement(pBufferDescEqual, i);
            pBufferDescICmp = m_pBuilder->CreateAnd(pBufferDescICmp, pBufferDescElemEqual);
        }
    }

    Value* pIndexICmp = m_pBuilder->CreateICmpEQ(indices[0], indices[1]);

    Value* pNewICmp = m_pBuilder->CreateAnd(pBufferDescICmp, pIndexICmp);

    if (pICmpInst->getPredicate() == ICmpInst::ICMP_NE)
    {
        pNewICmp = m_pBuilder->CreateNot(pNewICmp);
    }

    return pNewICmp;
}

// =====================================================================================================================
// Make a loop, returning the the value of the loop counter. This modifies the insertion point of the builder.
Instruction* PatchBufferOp::MakeLoop(
    Value* const       pLoopStart,  // [in] The start index of the loop.
    Value* const       pLoopEnd,    // [in] The end index of the loop.
    Value* const       pLoopStride, // [in] The stride of the loop.
    Instruction* const pInsertPos)  // [in] The position to insert the loop in the instruction stream.
{
    Value* const pInitialCond = m_pBuilder->CreateICmpNE(pLoopStart, pLoopEnd);

    BasicBlock* const pOrigBlock = pInsertPos->getParent();

    Instruction* const pTerminator = SplitBlockAndInsertIfThen(pInitialCond, pInsertPos, false);

    m_pBuilder->SetInsertPoint(pTerminator);

    // Create a phi node for the loop counter.
    PHINode* const pLoopCounter = m_pBuilder->CreatePHI(pLoopStart->getType(), 2);
    CopyMetadata(pLoopCounter, pInsertPos);

    // Set the loop counter to start value (initialization).
    pLoopCounter->addIncoming(pLoopStart, pOrigBlock);

    // Calculate the next value of the loop counter by doing loopCounter + loopStride.
    Value* const pLoopNextValue = m_pBuilder->CreateAdd(pLoopCounter, pLoopStride);
    CopyMetadata(pLoopNextValue, pInsertPos);

    // And set the loop counter to the next value.
    pLoopCounter->addIncoming(pLoopNextValue, pTerminator->getParent());

    // Our loop condition is just whether the next value of the loop counter is less than the end value.
    Value* const pCond = m_pBuilder->CreateICmpULT(pLoopNextValue, pLoopEnd);
    CopyMetadata(pCond, pInsertPos);

    // And our replacement terminator just branches back to the if body if there is more loop iterations to be done.
    Instruction* const pNewTerminator = m_pBuilder->CreateCondBr(pCond,
                                                                 pTerminator->getParent(),
                                                                 pTerminator->getSuccessor(0));
    CopyMetadata(pNewTerminator, pInsertPos);

    pTerminator->eraseFromParent();

    m_pBuilder->SetInsertPoint(pNewTerminator);

    return pLoopCounter;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for buffer operations.
INITIALIZE_PASS_BEGIN(PatchBufferOp, DEBUG_TYPE,
                "Patch LLVM for buffer operations", false, false)
INITIALIZE_PASS_DEPENDENCY(LegacyDivergenceAnalysis)
INITIALIZE_PASS_DEPENDENCY(PipelineShaders)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(PatchBufferOp, DEBUG_TYPE,
                "Patch LLVM for buffer operations", false, false)
