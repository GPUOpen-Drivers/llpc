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
 * @file  llpcPatchBufferOp.h
 * @brief LLPC header file: contains declaration of class Llpc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/InstVisitor.h"

#include "llpcPatch.h"

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of LLVM patching operations for buffer operations
class PatchBufferOp final:
    public llvm::FunctionPass,
    public llvm::InstVisitor<PatchBufferOp>
{
public:
    PatchBufferOp();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override;
    bool runOnFunction(llvm::Function& function) override;

    // Visitors
    void visitAtomicCmpXchgInst(AtomicCmpXchgInst& atomicCmpXchgInst);
    void visitAtomicRMWInst(llvm::AtomicRMWInst& atomicRmwInst);
    void visitBitCastInst(llvm::BitCastInst& bitCastInst);
    void visitCallInst(llvm::CallInst& callInst);
    void visitExtractElementInst(llvm::ExtractElementInst& extractElementInst);
    void visitGetElementPtrInst(llvm::GetElementPtrInst& getElemPtrInst);
    void visitInsertElementInst(llvm::InsertElementInst& insertElementInst);
    void visitLoadInst(llvm::LoadInst& loadInst);
    void visitMemCpyInst(llvm::MemCpyInst& memCpyInst);
    void visitMemMoveInst(llvm::MemMoveInst& memMoveInst);
    void visitMemSetInst(llvm::MemSetInst& memSetInst);
    void visitPHINode(llvm::PHINode& phiNode);
    void visitSelectInst(llvm::SelectInst& selectInst);
    void visitStoreInst(llvm::StoreInst& storeInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchBufferOp);

    llvm::Instruction* GetPointerOperandAsInst(llvm::Value* const pValue);
    llvm::Value* GetBaseAddressFromBufferDesc(llvm::Value* const pBufferDesc) const;
    void CopyMetadata(llvm::Value* const pDest, const llvm::Value* const pSrc) const;
    llvm::PointerType* GetRemappedType(llvm::Type* const pType) const;
    bool RemoveUsersForInvariantStarts(llvm::Value* const pValue);
    llvm::Value* ReplaceLoad(llvm::LoadInst* const pLoadInst);
    void ReplaceStore(llvm::StoreInst* const pStoreInst);

    // -----------------------------------------------------------------------------------------------------------------

    using Replacement = std::pair<llvm::Value*, llvm::Value*>;
    llvm::DenseMap<llvm::Instruction*, Replacement> m_replacementMap;      // The replacement map.
    llvm::DenseSet<llvm::Value*>                    m_invariantSet;        // The invariant set.
    llvm::DenseSet<llvm::Value*>                    m_divergenceSet;       // The divergence set.
    llvm::LegacyDivergenceAnalysis*                 m_pDivergenceAnalysis; // The divergence analysis.
    std::unique_ptr<llvm::IRBuilder<>>              m_pBuilder;            // The IRBuilder.
    Context*                                        m_pContext;            // The LLPC Context.
};

} // Llpc
