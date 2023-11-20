/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchBufferOp.h
 * @brief LLPC header file: contains declaration of class lgc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#pragma once

#include "compilerutils/TypeLowering.h"
#include "lgc/patch/Patch.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/UniformityAnalysis.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
// Old version of the code
namespace llvm {
class DivergenceInfo;
}
#else
// New version of the code (also handles unknown version, which we treat as latest)
#endif

namespace lgc {

class BufferDescToPtrOp;
class BufferLengthOp;
class BufferPtrDiffOp;
class PipelineState;

// =====================================================================================================================
// Helper class for lowering buffer operations integrated with a flow based on llvm_dialects::Visitor and TypeLowering.
class BufferOpLowering {
  // Hide operator bool to safe-guard against accidents.
  struct optional_bool : private std::optional<bool> {
    optional_bool() = default;
    optional_bool(const optional_bool &rhs) = default;
    optional_bool &operator=(const optional_bool &rhs) = default;
    optional_bool &operator=(bool rhs) {
      std::optional<bool>::operator=(rhs);
      return *this;
    }
    using std::optional<bool>::has_value;
    using std::optional<bool>::value;
    using std::optional<bool>::value_or;
  };

  struct DescriptorInfo {
    optional_bool invariant;
    optional_bool divergent;
  };

public:
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  BufferOpLowering(TypeLowering &typeLowering, PipelineState &pipelineState, llvm::DivergenceInfo &divergenceInfo);
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  BufferOpLowering(TypeLowering &typeLowering, PipelineState &pipelineState, llvm::UniformityInfo &uniformityInfo);
#endif

  static void registerVisitors(llvm_dialects::VisitorBuilder<BufferOpLowering> &builder);

  void finish();

private:
  void visitAtomicCmpXchgInst(llvm::AtomicCmpXchgInst &atomicCmpXchgInst);
  void visitAtomicRMWInst(llvm::AtomicRMWInst &atomicRmwInst);
  void visitBitCastInst(llvm::BitCastInst &bitCastInst);
  void visitBufferDescToPtr(BufferDescToPtrOp &descToPtr);
  void visitBufferLength(BufferLengthOp &length);
  void visitBufferPtrDiff(BufferPtrDiffOp &ptrDiff);
  void visitGetElementPtrInst(llvm::GetElementPtrInst &getElemPtrInst);
  void visitLoadInst(llvm::LoadInst &loadInst);
  void visitMemCpyInst(llvm::MemCpyInst &memCpyInst);
  void visitMemMoveInst(llvm::MemMoveInst &memMoveInst);
  void visitMemSetInst(llvm::MemSetInst &memSetInst);
  void visitPhiInst(llvm::PHINode &phi);
  void visitStoreInst(llvm::StoreInst &storeInst);
  void visitICmpInst(llvm::ICmpInst &icmpInst);
  void visitInvariantStart(llvm::IntrinsicInst &intrinsic);

  void postVisitLoadInst(llvm::LoadInst &loadInst);
  void postVisitStoreInst(llvm::StoreInst &storeInst);
  void postVisitMemCpyInst(llvm::MemCpyInst &memCpyInst);
  void postVisitMemSetInst(llvm::MemSetInst &memSetInst);

  DescriptorInfo getDescriptorInfo(llvm::Value *desc);
  void copyMetadata(llvm::Value *const dest, const llvm::Value *const src) const;
  llvm::Value *getBaseAddressFromBufferDesc(llvm::Value *const bufferDesc);
  llvm::Value *replaceLoadStore(llvm::Instruction &inst);
  llvm::Instruction *makeLoop(llvm::Value *const loopStart, llvm::Value *const loopEnd, llvm::Value *const loopStride,
                              llvm::Instruction *const insertPos);

  TypeLowering &m_typeLowering;
  llvm::IRBuilder<> m_builder;

  PipelineState &m_pipelineState;
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  llvm::DivergenceInfo &m_uniformityInfo;
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  llvm::UniformityInfo &m_uniformityInfo;
#endif

  // The proxy pointer type used to accumulate offsets.
  llvm::PointerType *m_offsetType = nullptr;

  // Map of buffer descriptor infos (for tracking invariance and divergence).
  llvm::DenseMap<llvm::Value *, DescriptorInfo> m_descriptors;

  llvm::SmallVector<llvm::PHINode *> m_divergentPhis;

  // Instructions to handle during finish().
  llvm::SmallVector<llvm::Instruction *> m_postVisitInsts;

  static constexpr unsigned MinMemOpLoopBytes = 256;
};

// =====================================================================================================================
// Represents the pass of LLVM patching operations for buffer operations
class PatchBufferOp : public llvm::InstVisitor<PatchBufferOp>, public llvm::PassInfoMixin<PatchBufferOp> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Patch LLVM for buffer operations"; }
};

} // namespace lgc
