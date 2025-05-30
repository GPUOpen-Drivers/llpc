/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  LowerBufferOperations.h
 * @brief LLPC header file: contains declaration of class lgc::LowerBufferOperations.
 ***********************************************************************************************************************
 */
#pragma once

#include "compilerutils/TypeLowering.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/lowering/LgcLowering.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/UniformityAnalysis.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"
#include <utility>

namespace lgc {

class BufferAddrToPtrOp;
class BufferDescToPtrOp;
class ConvertToStridedBufferPointerOp;
class StridedBufferDescToPtrOp;
class BufferLoadDescToPtrOp;
class StridedBufferLoadDescToPtrOp;
class StridedBufferAddrAndStrideToPtrOp;
class StridedIndexAddOp;
class BufferLengthOp;
class BufferPtrDiffOp;
class LoadTfeOp;
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
    optional_bool globallyCoherent;
  };

public:
  BufferOpLowering(compilerutils::TypeLowering &typeLowering, PipelineState &pipelineState,
                   llvm::UniformityInfo &uniformityInfo);

  static void registerVisitors(llvm_dialects::VisitorBuilder<BufferOpLowering> &builder);

  void finish();

private:
  void visitAtomicCmpXchgInst(llvm::AtomicCmpXchgInst &atomicCmpXchgInst);
  void visitAtomicRMWInst(llvm::AtomicRMWInst &atomicRmwInst);
  void visitBitCastInst(llvm::BitCastInst &bitCastInst);
  void visitBufferAddrToPtr(BufferAddrToPtrOp &op);
  void visitBufferDescToPtr(BufferDescToPtrOp &descToPtr);
  void visitStridedBufferDescToPtr(StridedBufferDescToPtrOp &descToPtr);
  void visitBufferLoadDescToPtr(BufferLoadDescToPtrOp &loadDescToPtr);
  void visitConvertToStridedBufferPointer(ConvertToStridedBufferPointerOp &convertToStrided);
  void visitStridedBufferLoadDescToPtr(StridedBufferLoadDescToPtrOp &loadDescToPtr);
  void visitStridedBufferAddrAndStrideToPtr(StridedBufferAddrAndStrideToPtrOp &addrAndStrideToPtr);
  void visitStridedIndexAdd(StridedIndexAddOp &indexAdd);
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
  void visitLoadTfeOp(LoadTfeOp &loadTfe);
  void visitReadFirstLane(llvm::IntrinsicInst &intrinsic);

  void postVisitLoadInst(llvm::LoadInst &loadInst);
  void postVisitStoreInst(llvm::StoreInst &storeInst);
  void postVisitMemCpyInst(llvm::MemCpyInst &memCpyInst);
  void postVisitMemSetInst(llvm::MemSetInst &memSetInst);
  void postVisitLoadTfeOp(LoadTfeOp &loadTfe);

  DescriptorInfo getDescriptorInfo(llvm::Value *desc);
  bool isAnyBufferPointer(const llvm::Value *const pointerVal);
  void copyMetadata(llvm::Value *const dest, const llvm::Value *const src) const;
  llvm::Value *getBaseAddressFromBufferDesc(llvm::Value *const bufferDesc);
  llvm::Value *replaceLoadStore(llvm::Instruction &inst);
  llvm::Instruction *makeLoop(llvm::Value *const loopStart, llvm::Value *const loopEnd, llvm::Value *const loopStride,
                              llvm::Instruction *const insertPos);
  llvm::Value *createGlobalPointerAccess(llvm::Value *const bufferDesc, llvm::Value *const offset,
                                         llvm::Value *const strideIndex, llvm::Type *const type,
                                         llvm::Instruction &inst,
                                         const llvm::function_ref<llvm::Value *(llvm::Value *)> callback);
  llvm::Value *createLoadDesc(llvm::Value *buffAddress, bool forceRawView, bool isCompact, llvm::Value *forcedStride);

  ShaderStageEnum getMemoryInstShaderStage(llvm::Instruction *inst);

  compilerutils::TypeLowering &m_typeLowering;
  BuilderImpl m_builder;

  PipelineState &m_pipelineState;
  llvm::UniformityInfo &m_uniformityInfo;

  // The proxy pointer type used to accumulate offsets.
  llvm::PointerType *m_offsetType = nullptr;

  // Map of buffer descriptor infos (for tracking invariance and divergence).
  llvm::DenseMap<llvm::Value *, DescriptorInfo> m_descriptors;
  llvm::DenseMap<llvm::Value *, std::pair<llvm::Value *, llvm::ConstantInt *>> m_stridedDescriptors;

  llvm::SmallVector<llvm::PHINode *> m_divergentPhis;

  // Instructions to handle during finish().
  llvm::SmallVector<llvm::Instruction *> m_postVisitInsts;

  static constexpr unsigned MinMemOpLoopBytes = 256;
};

// =====================================================================================================================
// Represents the pass of LGC lowering operations for buffer operations
class LowerBufferOperations : public llvm::InstVisitor<LowerBufferOperations>,
                              public llvm::PassInfoMixin<LowerBufferOperations> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower buffer operations"; }
};

} // namespace lgc
