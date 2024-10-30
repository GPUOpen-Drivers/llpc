/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  StructurizeBuffers.cpp
 * @brief LLPC source file: contains implementation of class lgc::StructurizeBuffers.
 ***********************************************************************************************************************
 */

#include "lgc/patch/StructurizeBuffers.h"
#include "compilerutils/CompilerUtils.h"
#include "lgc/CommonDefs.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/PipelineState.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Casting.h"

#define DEBUG_TYPE "lgc-structurize-buffers"

using namespace llvm;
using namespace lgc;

namespace {

struct StructurizeBuffersImpl {
  StructurizeBuffersImpl(Function *function, bool robustBufferAccess);

  bool run();
  void visitBufferIndex(BufferIndexOp &bufferIndex);

  Function *m_function;
  llvm_dialects::Builder m_builder;
  MapVector<Value *, SmallVector<BufferIndexOp *>> bufferIndexOps;
  bool robustBufferAccess;
};

} // anonymous namespace

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in/out] function : LLVM function to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses StructurizeBuffers::run(Function &function, FunctionAnalysisManager &analysisManager) {
  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();
  bool robustBufferAccess =
      pipelineState->getOptions().enableExtendedRobustBufferAccess || pipelineState->getOptions().robustBufferAccess;
  StructurizeBuffersImpl impl(&function, robustBufferAccess);

  if (impl.run())
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

StructurizeBuffersImpl::StructurizeBuffersImpl(Function *function, bool robustBufferAccess)
    : m_function(function), m_builder(function->getContext()), robustBufferAccess(robustBufferAccess) {
}

void StructurizeBuffersImpl::visitBufferIndex(BufferIndexOp &bufferIndex) {
  bufferIndexOps[bufferIndex.getPtr()].push_back(&bufferIndex);
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in/out] function : LLVM function to be run on
// @returns : True if the function was modified by the transformation and false otherwise
bool StructurizeBuffersImpl::run() {

  static const auto visitor =
      llvm_dialects::VisitorBuilder<StructurizeBuffersImpl>().add(&StructurizeBuffersImpl::visitBufferIndex).build();

  visitor.visit(*this, *m_function);

  if (bufferIndexOps.empty())
    return false;

  auto isConvertible = [](BufferIndexOp *mark) -> bool {
    if (isa<ConstantInt>(mark->getIndex()))
      return false;
    return mark->getStride() > 4;
  };

  auto storesBuffer = [&m_builder = m_builder](User *user) -> bool {
    if (auto store = dyn_cast<StoreInst>(user))
      return store->getValueOperand()->getType() == m_builder.getPtrTy(ADDR_SPACE_BUFFER_FAT_POINTER);

    return false;
  };

  auto isSupported = [](User *user) -> bool {
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 506212
    return isa<LoadInst, StoreInst, SelectInst, AtomicRMWInst, AtomicCmpXchgInst>(user);
#else
    return isa<LoadInst, StoreInst, SelectInst>(user);
#endif
  };

  SmallVector<Value *> notConvertible;
  for (auto &base : bufferIndexOps) {
    if (robustBufferAccess) {
      notConvertible.push_back(base.first);
      continue;
    }

    if (base.first->getType()->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER) {
      notConvertible.push_back(base.first);
      continue;
    }

    if (llvm::none_of(base.second, isConvertible)) {
      notConvertible.push_back(base.first);
      continue;
    }

    for (auto *bufferIndexOp : base.second) {
      SmallVector<Value *> worklist;
      worklist.push_back(bufferIndexOp);
      bool convertible = true;
      while (!worklist.empty() && convertible) {
        auto *current = worklist.pop_back_val();

        for (auto *user : current->users()) {
          if (isa<GetElementPtrInst>(user)) {
            worklist.push_back(user);
            continue;
          }

          if (!isSupported(user) || storesBuffer(user)) {
            notConvertible.push_back(base.first);
            convertible = false;
            break;
          }
        }
      }
    }
  }

  SmallVector<Instruction *> toRemove;
  for (const auto &pointer : bufferIndexOps) {
    if (llvm::is_contained(notConvertible, pointer.first)) {
      for (auto *bufferIndexOp : bufferIndexOps[pointer.first]) {
        m_builder.SetInsertPoint(bufferIndexOp);
        auto *offset = m_builder.CreateMul(bufferIndexOp->getIndex(), m_builder.getInt32(bufferIndexOp->getStride()));
        auto *gep = m_builder.CreateGEP(m_builder.getInt8Ty(), bufferIndexOp->getPtr(), offset);
        bufferIndexOp->replaceAllUsesWith(gep);
        toRemove.push_back(bufferIndexOp);
      }
    } else {
      for (auto *bufferIndexOp : pointer.second) {
        m_builder.SetInsertPoint(bufferIndexOp);
        Value *strided =
            m_builder.create<ConvertToStridedBufferPointerOp>(bufferIndexOp->getPtr(), bufferIndexOp->getStride());
        strided = m_builder.create<StridedIndexAddOp>(strided, bufferIndexOp->getIndex());

        toRemove.push_back(bufferIndexOp);
        CompilerUtils::replaceAllPointerUses(&m_builder, bufferIndexOp, strided, toRemove);
      }
    }
  }

  for (Instruction *I : reverse(toRemove))
    I->eraseFromParent();

  return true;
}
