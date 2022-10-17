/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchBufferOp.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchBufferOp.h"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/DivergenceAnalysis.h"
#include "llvm/Analysis/LegacyDivergenceAnalysis.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "lgc-patch-buffer-op"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char LegacyPatchBufferOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching for buffer operations
FunctionPass *createLegacyPatchBufferOp() {
  return new LegacyPatchBufferOp();
}

// =====================================================================================================================
LegacyPatchBufferOp::LegacyPatchBufferOp() : FunctionPass(ID) {
}

// =====================================================================================================================
// Get the analysis usage of this pass.
//
// @param [out] analysisUsage : The analysis usage.
void LegacyPatchBufferOp::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.addRequired<LegacyDivergenceAnalysis>();
  analysisUsage.addRequired<LegacyPipelineStateWrapper>();
  analysisUsage.addRequired<TargetTransformInfoWrapperPass>();
  analysisUsage.addPreserved<TargetTransformInfoWrapperPass>();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in/out] function : LLVM function to be run on
// @returns : True if the function was modified by the transformation and false otherwise
bool LegacyPatchBufferOp::runOnFunction(Function &function) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(function.getParent());
  LegacyDivergenceAnalysis *divergenceAnalysis = &getAnalysis<LegacyDivergenceAnalysis>();
  auto isDivergent = [divergenceAnalysis](const Value &value) { return divergenceAnalysis->isDivergent(&value); };
  return m_impl.runImpl(function, pipelineState, isDivergent);
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in/out] function : LLVM function to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchBufferOp::run(Function &function, FunctionAnalysisManager &analysisManager) {
  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();
  DivergenceInfo &divergenceInfo = analysisManager.getResult<DivergenceAnalysis>(function);
  auto isDivergent = [&](const Value &value) { return divergenceInfo.isDivergent(value); };
  if (runImpl(function, pipelineState, isDivergent))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in,out] function : LLVM function to be run on
// @param pipelineState : Pipeline state
// @param isDivergent : Function returning true if the given value is divergent
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchBufferOp::runImpl(Function &function, PipelineState *pipelineState,
                            std::function<bool(const llvm::Value &)> isDivergent) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Buffer-Op\n");

  m_pipelineState = pipelineState;
  m_isDivergent = std::move(isDivergent);
  m_context = &function.getContext();
  m_builder = std::make_unique<IRBuilder<>>(*m_context);

  // Invoke visitation of the target instructions.

  // If the function is not a valid shader stage, bail.
  if (lgc::getShaderStage(&function) == ShaderStageInvalid) {
    return false;
  }

  // To replace the fat pointer uses correctly we need to walk the basic blocks strictly in domination order to avoid
  // visiting a use of a fat pointer before it was actually defined.
  ReversePostOrderTraversal<Function *> traversal(&function);
  for (BasicBlock *const block : traversal)
    visit(*block);

  fixIncompletePhis();

  // Some instructions can modify the CFG and thus have to be performed after the normal visitors.
  for (Instruction *const inst : m_postVisitInsts) {
    if (MemSetInst *const memSet = dyn_cast<MemSetInst>(inst))
      postVisitMemSetInst(*memSet);
    else if (MemCpyInst *const memCpy = dyn_cast<MemCpyInst>(inst))
      postVisitMemCpyInst(*memCpy);
  }
  m_postVisitInsts.clear();

  const bool changed = (!m_replacementMap.empty());

  for (auto &replaceMap : m_replacementMap) {
    Instruction *const inst = dyn_cast<Instruction>(replaceMap.first);

    if (!inst)
      continue;

    if (!isa<StoreInst>(inst))
      inst->replaceAllUsesWith(UndefValue::get(inst->getType()));

    inst->eraseFromParent();
  }

  m_replacementMap.clear();
  m_incompletePhis.clear();
  m_invariantSet.clear();
  m_divergenceSet.clear();

  return changed;
}

// =====================================================================================================================
// Visits "cmpxchg" instruction.
//
// @param atomicCmpXchgInst : The instruction
void PatchBufferOp::visitAtomicCmpXchgInst(AtomicCmpXchgInst &atomicCmpXchgInst) {
  // If the type we are doing an atomic operation on is not a fat pointer, bail.
  if (atomicCmpXchgInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&atomicCmpXchgInst);

  Value *const pointer = getPointerOperandAsInst(atomicCmpXchgInst.getPointerOperand());

  Type *const storeType = atomicCmpXchgInst.getNewValOperand()->getType();

  const bool isSlc = atomicCmpXchgInst.getMetadata(LLVMContext::MD_nontemporal);

  Value *const bufferDesc = m_replacementMap[pointer].first;
  Value *const baseIndex = m_builder->CreatePtrToInt(m_replacementMap[pointer].second, m_builder->getInt32Ty());
  copyMetadata(baseIndex, &atomicCmpXchgInst);

  // If our buffer descriptor is divergent or is not a 32-bit integer, need to handle it differently.
  if (m_divergenceSet.count(bufferDesc) > 0 || !storeType->isIntegerTy(32)) {
    Value *const baseAddr = getBaseAddressFromBufferDesc(bufferDesc);

    // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
    Value *const bound = m_builder->CreateExtractElement(bufferDesc, 2);
    Value *const inBound = m_builder->CreateICmpULT(baseIndex, bound);
    Value *const newBaseIndex = m_builder->CreateSelect(inBound, baseIndex, m_builder->getInt32(0));

    // Add on the index to the address.
    Value *atomicPointer = m_builder->CreateGEP(m_builder->getInt8Ty(), baseAddr, newBaseIndex);

    atomicPointer = m_builder->CreateBitCast(atomicPointer, storeType->getPointerTo(ADDR_SPACE_GLOBAL));

    const AtomicOrdering successOrdering = atomicCmpXchgInst.getSuccessOrdering();
    const AtomicOrdering failureOrdering = atomicCmpXchgInst.getFailureOrdering();

    Value *const compareValue = atomicCmpXchgInst.getCompareOperand();
    Value *const newValue = atomicCmpXchgInst.getNewValOperand();
    AtomicCmpXchgInst *const newAtomicCmpXchg = m_builder->CreateAtomicCmpXchg(
        atomicPointer, compareValue, newValue, MaybeAlign(), successOrdering, failureOrdering);
    newAtomicCmpXchg->setVolatile(atomicCmpXchgInst.isVolatile());
    newAtomicCmpXchg->setSyncScopeID(atomicCmpXchgInst.getSyncScopeID());
    newAtomicCmpXchg->setWeak(atomicCmpXchgInst.isWeak());
    copyMetadata(newAtomicCmpXchg, &atomicCmpXchgInst);

    // Record the atomic instruction so we remember to delete it later.
    m_replacementMap[&atomicCmpXchgInst] = std::make_pair(nullptr, nullptr);

    atomicCmpXchgInst.replaceAllUsesWith(newAtomicCmpXchg);
  } else {
    switch (atomicCmpXchgInst.getSuccessOrdering()) {
    case AtomicOrdering::Release:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent: {
      FenceInst *const fence = m_builder->CreateFence(AtomicOrdering::Release, atomicCmpXchgInst.getSyncScopeID());
      copyMetadata(fence, &atomicCmpXchgInst);
      break;
    }
    default: {
      break;
    }
    }

    Value *const atomicCall = m_builder->CreateIntrinsic(
        Intrinsic::amdgcn_raw_buffer_atomic_cmpswap, atomicCmpXchgInst.getNewValOperand()->getType(),
        {atomicCmpXchgInst.getNewValOperand(), atomicCmpXchgInst.getCompareOperand(), bufferDesc, baseIndex,
         m_builder->getInt32(0), m_builder->getInt32(isSlc ? 1 : 0)});

    switch (atomicCmpXchgInst.getSuccessOrdering()) {
    case AtomicOrdering::Acquire:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent: {
      FenceInst *const fence = m_builder->CreateFence(AtomicOrdering::Acquire, atomicCmpXchgInst.getSyncScopeID());
      copyMetadata(fence, &atomicCmpXchgInst);
      break;
    }
    default: {
      break;
    }
    }

    Value *resultValue = UndefValue::get(atomicCmpXchgInst.getType());

    resultValue = m_builder->CreateInsertValue(resultValue, atomicCall, static_cast<uint64_t>(0));
    copyMetadata(resultValue, &atomicCmpXchgInst);

    // NOTE: If we have a strong compare exchange, LLVM optimization will always set the compare result to "Equal".
    // Thus, we have to correct this behaviour and do the comparison by ourselves.
    if (!atomicCmpXchgInst.isWeak()) {
      Value *const valueEqual = m_builder->CreateICmpEQ(atomicCall, atomicCmpXchgInst.getCompareOperand());
      copyMetadata(valueEqual, &atomicCmpXchgInst);

      resultValue = m_builder->CreateInsertValue(resultValue, valueEqual, static_cast<uint64_t>(1));
      copyMetadata(resultValue, &atomicCmpXchgInst);
    }

    // Record the atomic instruction so we remember to delete it later.
    m_replacementMap[&atomicCmpXchgInst] = std::make_pair(nullptr, nullptr);

    atomicCmpXchgInst.replaceAllUsesWith(resultValue);
  }
}

// =====================================================================================================================
// Visits "atomicrmw" instruction.
//
// @param atomicRmwInst : The instruction
void PatchBufferOp::visitAtomicRMWInst(AtomicRMWInst &atomicRmwInst) {
  if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
    m_builder->SetInsertPoint(&atomicRmwInst);

    Value *const pointer = getPointerOperandAsInst(atomicRmwInst.getPointerOperand());

    Type *const storeType = atomicRmwInst.getValOperand()->getType();

    const bool isSlc = atomicRmwInst.getMetadata(LLVMContext::MD_nontemporal);

    Value *const bufferDesc = m_replacementMap[pointer].first;
    Value *const baseIndex = m_builder->CreatePtrToInt(m_replacementMap[pointer].second, m_builder->getInt32Ty());
    copyMetadata(baseIndex, &atomicRmwInst);

    // If our buffer descriptor is divergent, need to handle it differently.
    if (m_divergenceSet.count(bufferDesc) > 0) {
      Value *const baseAddr = getBaseAddressFromBufferDesc(bufferDesc);

      // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
      Value *const bound = m_builder->CreateExtractElement(bufferDesc, 2);
      Value *const inBound = m_builder->CreateICmpULT(baseIndex, bound);
      Value *const newBaseIndex = m_builder->CreateSelect(inBound, baseIndex, m_builder->getInt32(0));

      // Add on the index to the address.
      Value *atomicPointer = m_builder->CreateGEP(m_builder->getInt8Ty(), baseAddr, newBaseIndex);

      atomicPointer = m_builder->CreateBitCast(atomicPointer, storeType->getPointerTo(ADDR_SPACE_GLOBAL));

      AtomicRMWInst *const newAtomicRmw =
          m_builder->CreateAtomicRMW(atomicRmwInst.getOperation(), atomicPointer, atomicRmwInst.getValOperand(),
                                     atomicRmwInst.getAlign(), atomicRmwInst.getOrdering());
      newAtomicRmw->setVolatile(atomicRmwInst.isVolatile());
      newAtomicRmw->setSyncScopeID(atomicRmwInst.getSyncScopeID());
      copyMetadata(newAtomicRmw, &atomicRmwInst);

      // Record the atomic instruction so we remember to delete it later.
      m_replacementMap[&atomicRmwInst] = std::make_pair(nullptr, nullptr);

      atomicRmwInst.replaceAllUsesWith(newAtomicRmw);
    } else {
      switch (atomicRmwInst.getOrdering()) {
      case AtomicOrdering::Release:
      case AtomicOrdering::AcquireRelease:
      case AtomicOrdering::SequentiallyConsistent: {
        FenceInst *const fence = m_builder->CreateFence(AtomicOrdering::Release, atomicRmwInst.getSyncScopeID());
        copyMetadata(fence, &atomicRmwInst);
        break;
      }
      default: {
        break;
      }
      }
      Intrinsic::ID intrinsic = Intrinsic::not_intrinsic;
      switch (atomicRmwInst.getOperation()) {
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
      case AtomicRMWInst::FAdd:
        intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_fadd;
        break;
      case AtomicRMWInst::FMax:
        intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_fmax;
        break;
      case AtomicRMWInst::FMin:
        intrinsic = Intrinsic::amdgcn_raw_buffer_atomic_fmin;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }

      Value *const atomicCall = m_builder->CreateIntrinsic(intrinsic, storeType,
                                                           {atomicRmwInst.getValOperand(), bufferDesc, baseIndex,
                                                            m_builder->getInt32(0), m_builder->getInt32(isSlc * 2)});
      copyMetadata(atomicCall, &atomicRmwInst);

      switch (atomicRmwInst.getOrdering()) {
      case AtomicOrdering::Acquire:
      case AtomicOrdering::AcquireRelease:
      case AtomicOrdering::SequentiallyConsistent: {
        FenceInst *const fence = m_builder->CreateFence(AtomicOrdering::Acquire, atomicRmwInst.getSyncScopeID());
        copyMetadata(fence, &atomicRmwInst);
        break;
      }
      default: {
        break;
      }
      }

      // Record the atomic instruction so we remember to delete it later.
      m_replacementMap[&atomicRmwInst] = std::make_pair(nullptr, nullptr);

      atomicRmwInst.replaceAllUsesWith(atomicCall);
    }
  } else if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_GLOBAL) {
    AtomicRMWInst::BinOp op = atomicRmwInst.getOperation();
    Type *const storeType = atomicRmwInst.getValOperand()->getType();
    if (op == AtomicRMWInst::FMin || op == AtomicRMWInst::FMax || op == AtomicRMWInst::FAdd) {
      Value *const pointer = getPointerOperandAsInst(atomicRmwInst.getPointerOperand());
      m_builder->SetInsertPoint(&atomicRmwInst);
      Intrinsic::ID intrinsic = Intrinsic::not_intrinsic;
      switch (atomicRmwInst.getOperation()) {
      case AtomicRMWInst::FMin:
        intrinsic = Intrinsic::amdgcn_global_atomic_fmin;
        break;
      case AtomicRMWInst::FMax:
        intrinsic = Intrinsic::amdgcn_global_atomic_fmax;
        break;
      case AtomicRMWInst::FAdd:
        intrinsic = Intrinsic::amdgcn_global_atomic_fadd;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
      Value *const atomicCall = m_builder->CreateIntrinsic(intrinsic, {storeType, pointer->getType(), storeType},
                                                           {pointer, atomicRmwInst.getValOperand()});
      copyMetadata(atomicCall, &atomicRmwInst);
      // Record the atomic instruction so we remember to delete it later.
      m_replacementMap[&atomicRmwInst] = std::make_pair(nullptr, nullptr);

      atomicRmwInst.replaceAllUsesWith(atomicCall);
    }
  } else if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_LOCAL) {
    AtomicRMWInst::BinOp op = atomicRmwInst.getOperation();
    Type *const storeType = atomicRmwInst.getValOperand()->getType();
    if (op == AtomicRMWInst::FMin || op == AtomicRMWInst::FMax || op == AtomicRMWInst::FAdd) {
      Value *const pointer = getPointerOperandAsInst(atomicRmwInst.getPointerOperand());
      m_builder->SetInsertPoint(&atomicRmwInst);
      Intrinsic::ID intrinsic = Intrinsic::not_intrinsic;
      switch (atomicRmwInst.getOperation()) {
      case AtomicRMWInst::FMin:
        intrinsic = Intrinsic::amdgcn_ds_fmin;
        break;
      case AtomicRMWInst::FMax:
        intrinsic = Intrinsic::amdgcn_ds_fmax;
        break;
      case AtomicRMWInst::FAdd:
        intrinsic = Intrinsic::amdgcn_ds_fadd;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }

      Value *const atomicCall = m_builder->CreateIntrinsic(
          intrinsic, {storeType},
          {pointer, atomicRmwInst.getValOperand(),
           m_builder->getInt32(static_cast<uint32_t>(atomicRmwInst.getOrdering())),
           m_builder->getInt32(atomicRmwInst.getSyncScopeID()), m_builder->getInt1(atomicRmwInst.isVolatile())});
      copyMetadata(atomicCall, &atomicRmwInst);
      // Record the atomic instruction so we remember to delete it later.
      m_replacementMap[&atomicRmwInst] = std::make_pair(nullptr, nullptr);
      atomicRmwInst.replaceAllUsesWith(atomicCall);
    }
  }
}

// =====================================================================================================================
// Visits "bitcast" instruction.
//
// @param bitCastInst : The instruction
void PatchBufferOp::visitBitCastInst(BitCastInst &bitCastInst) {
  Type *const destType = bitCastInst.getType();

  // If the type is not a pointer type, bail.
  if (!destType->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (destType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&bitCastInst);

  Value *const pointer = getPointerOperandAsInst(bitCastInst.getOperand(0));

  Value *const newBitCast =
      m_builder->CreateBitCast(m_replacementMap[pointer].second, getRemappedType(bitCastInst.getDestTy()));

  copyMetadata(newBitCast, pointer);

  m_replacementMap[&bitCastInst] = std::make_pair(m_replacementMap[pointer].first, newBitCast);
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : The instruction
void PatchBufferOp::visitCallInst(CallInst &callInst) {
  Function *const calledFunc = callInst.getCalledFunction();

  // If the call does not have a called function, bail.
  if (!calledFunc)
    return;

  const StringRef callName(calledFunc->getName());

  // If the call is not a late intrinsic call we need to replace, bail.
  if (!callName.startswith(lgcName::LaterCallPrefix))
    return;

  m_builder->SetInsertPoint(&callInst);

  if (callName.equals(lgcName::LateLaunderFatPointer)) {
    Constant *const nullPointer = ConstantPointerNull::get(getRemappedType(callInst.getType()));
    m_replacementMap[&callInst] = std::make_pair(callInst.getArgOperand(0), nullPointer);

    // Check for any invariant starts that use the pointer.
    if (removeUsersForInvariantStarts(&callInst))
      m_invariantSet.insert(callInst.getArgOperand(0));

    // If the incoming index to the fat pointer launder was divergent, remember it.
    if (m_isDivergent(*callInst.getArgOperand(0)))
      m_divergenceSet.insert(callInst.getArgOperand(0));
  } else if (callName.startswith(lgcName::LateBufferLength)) {
    Value *const pointer = getPointerOperandAsInst(callInst.getArgOperand(0));

    // Extract element 2 which is the NUM_RECORDS field from the buffer descriptor.
    Value *const bufferDesc = m_replacementMap[pointer].first;
    Value *numRecords = m_builder->CreateExtractElement(bufferDesc, 2);
    Value *offset = callInst.getArgOperand(1);

    // If null descriptors are allowed, we must guarantee a 0 result for a null buffer descriptor.
    //
    // What we implement here is in fact more robust: ensure that the subtraction of the offset is clamped to 0.
    // The backend should be able to achieve this with a single additional ALU instruction (e.g. s_max_u32).
    if (m_pipelineState->getOptions().allowNullDescriptor) {
      Value *const underflow = m_builder->CreateICmpUGT(offset, numRecords);
      numRecords = m_builder->CreateSelect(underflow, offset, numRecords);
    }

    numRecords = m_builder->CreateSub(numRecords, offset);

    // Record the call instruction so we remember to delete it later.
    m_replacementMap[&callInst] = std::make_pair(nullptr, nullptr);

    callInst.replaceAllUsesWith(numRecords);
  } else if (callName.startswith(lgcName::LateBufferPtrDiff)) {
    Type *const ty = callInst.getArgOperand(0)->getType();
    Value *const lhs = getPointerOperandAsInst(callInst.getArgOperand(1));
    Value *const rhs = getPointerOperandAsInst(callInst.getArgOperand(2));

    assert(lhs->getType()->isPointerTy() && lhs->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER &&
           rhs->getType()->isPointerTy() && rhs->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER &&
           "Argument to BufferPtrDiff is not a buffer fat pointer");

    Value *const lhsPtrToInt = m_builder->CreatePtrToInt(m_replacementMap[lhs].second, m_builder->getInt64Ty());
    Value *const rhsPtrToInt = m_builder->CreatePtrToInt(m_replacementMap[rhs].second, m_builder->getInt64Ty());

    copyMetadata(lhsPtrToInt, lhs);
    copyMetadata(rhsPtrToInt, rhs);

    Value *const difference = m_builder->CreateSub(lhsPtrToInt, rhsPtrToInt);
    Constant *const size = ConstantExpr::getSizeOf(ty);
    Value *const elementDifference = m_builder->CreateExactSDiv(difference, size);

    // Record the call instruction so we remember to delete it later.
    m_replacementMap[&callInst] = std::make_pair(nullptr, nullptr);

    callInst.replaceAllUsesWith(elementDifference);
  } else
    llvm_unreachable("Should never be called!");
}

// =====================================================================================================================
// Visits "extractelement" instruction.
//
// @param extractElementInst : The instruction
void PatchBufferOp::visitExtractElementInst(ExtractElementInst &extractElementInst) {
  PointerType *const pointerType = dyn_cast<PointerType>(extractElementInst.getType());

  // If the extract element is not extracting a pointer, bail.
  if (!pointerType)
    return;

  // If the type we are GEPing into is not a fat pointer, bail.
  if (pointerType->getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&extractElementInst);

  Value *const pointer = getPointerOperandAsInst(extractElementInst.getVectorOperand());
  Value *const index = extractElementInst.getIndexOperand();

  Value *const pointerElem = m_builder->CreateExtractElement(m_replacementMap[pointer].second, index);
  copyMetadata(pointerElem, pointer);

  m_replacementMap[&extractElementInst] = std::make_pair(m_replacementMap[pointer].first, pointerElem);
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtrInst : The instruction
void PatchBufferOp::visitGetElementPtrInst(GetElementPtrInst &getElemPtrInst) {
  // If the type we are GEPing into is not a fat pointer, bail.
  if (getElemPtrInst.getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&getElemPtrInst);

  Value *const pointer = getPointerOperandAsInst(getElemPtrInst.getPointerOperand());

  SmallVector<Value *, 8> indices(getElemPtrInst.idx_begin(), getElemPtrInst.idx_end());

  Value *newGetElemPtr = nullptr;
  auto getElemPtrPtr = m_replacementMap[pointer].second;
  auto getElemPtrEltTy = getElemPtrInst.getSourceElementType();
  assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(getElemPtrPtr->getType()->getScalarType(), getElemPtrEltTy));

  if (getElemPtrInst.isInBounds())
    newGetElemPtr = m_builder->CreateInBoundsGEP(getElemPtrEltTy, getElemPtrPtr, indices);
  else
    newGetElemPtr = m_builder->CreateGEP(getElemPtrEltTy, getElemPtrPtr, indices);

  copyMetadata(newGetElemPtr, pointer);

  m_replacementMap[&getElemPtrInst] = std::make_pair(m_replacementMap[pointer].first, newGetElemPtr);
}

// =====================================================================================================================
// Visits "insertelement" instruction.
//
// @param insertElementInst : The instruction
void PatchBufferOp::visitInsertElementInst(InsertElementInst &insertElementInst) {
  Type *const type = insertElementInst.getType();

  // If the type is not a vector, bail.
  if (!type->isVectorTy())
    return;

  PointerType *const pointerType = dyn_cast<PointerType>(cast<VectorType>(type)->getElementType());

  // If the extract element is not extracting from a vector of pointers, bail.
  if (!pointerType)
    return;

  // If the type we are GEPing into is not a fat pointer, bail.
  if (pointerType->getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&insertElementInst);

  Value *const pointer = getPointerOperandAsInst(insertElementInst.getOperand(1));
  Value *const index = m_replacementMap[pointer].second;

  Value *indexVector = nullptr;

  if (isa<UndefValue>(insertElementInst.getOperand(0)))
    indexVector =
        UndefValue::get(FixedVectorType::get(index->getType(), cast<FixedVectorType>(type)->getNumElements()));
  else
    indexVector = m_replacementMap[getPointerOperandAsInst(insertElementInst.getOperand(0))].second;

  indexVector = m_builder->CreateInsertElement(indexVector, index, insertElementInst.getOperand(2));
  copyMetadata(indexVector, pointer);

  m_replacementMap[&insertElementInst] = std::make_pair(m_replacementMap[pointer].first, indexVector);
}

// =====================================================================================================================
// Visits "load" instruction.
//
// @param loadInst : The instruction
void PatchBufferOp::visitLoadInst(LoadInst &loadInst) {
  const unsigned addrSpace = loadInst.getPointerAddressSpace();

  if (addrSpace == ADDR_SPACE_CONST) {
    m_builder->SetInsertPoint(&loadInst);

    Type *const loadType = loadInst.getType();

    // If the load is not a pointer type, bail.
    if (!loadType->isPointerTy())
      return;

    // If the address space of the loaded pointer is not a buffer fat pointer, bail.
    if (loadType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
      return;

    assert(loadInst.isVolatile() == false);
    assert(loadInst.getOrdering() == AtomicOrdering::NotAtomic);

    Type *const castType = FixedVectorType::get(Type::getInt32Ty(*m_context), 4)->getPointerTo(ADDR_SPACE_CONST);

    Value *const pointer = getPointerOperandAsInst(loadInst.getPointerOperand());

    Value *const loadPointer = m_builder->CreateBitCast(pointer, castType);

    LoadInst *const newLoad =
        m_builder->CreateAlignedLoad(m_builder->getInt32Ty(), loadPointer, loadInst.getAlign(), loadInst.isVolatile());
    newLoad->setOrdering(loadInst.getOrdering());
    newLoad->setSyncScopeID(loadInst.getSyncScopeID());
    copyMetadata(newLoad, &loadInst);

    Constant *const nullPointer = ConstantPointerNull::get(getRemappedType(loadType));

    m_replacementMap[&loadInst] = std::make_pair(newLoad, nullPointer);

    // If we removed an invariant load, remember that our new load is invariant.
    if (removeUsersForInvariantStarts(&loadInst))
      m_invariantSet.insert(newLoad);

    // If the original load was divergent, it means we are using descriptor indexing and need to remember it.
    if (m_isDivergent(loadInst))
      m_divergenceSet.insert(newLoad);
  } else if (addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER) {
    Value *const newLoad = replaceLoadStore(loadInst);

    // Record the load instruction so we remember to delete it later.
    m_replacementMap[&loadInst] = std::make_pair(nullptr, nullptr);

    loadInst.replaceAllUsesWith(newLoad);
  }
}

// =====================================================================================================================
// Visits "memcpy" instruction.
//
// @param memCpyInst : The memcpy instruction
void PatchBufferOp::visitMemCpyInst(MemCpyInst &memCpyInst) {
  Value *const dest = memCpyInst.getArgOperand(0);
  Value *const src = memCpyInst.getArgOperand(1);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();
  const unsigned srcAddrSpace = src->getType()->getPointerAddressSpace();

  // If either of the address spaces are fat pointers.
  if (destAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || srcAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER) {
    // Handling memcpy requires us to modify the CFG, so we need to do it after the initial visit pass.
    m_postVisitInsts.push_back(&memCpyInst);
  }
}

// =====================================================================================================================
// Visits "memmove" instruction.
//
// @param memMoveInst : The memmove instruction
void PatchBufferOp::visitMemMoveInst(MemMoveInst &memMoveInst) {
  Value *const dest = memMoveInst.getArgOperand(0);
  Value *const src = memMoveInst.getArgOperand(1);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();
  const unsigned srcAddrSpace = src->getType()->getPointerAddressSpace();

  // If either of the address spaces are not fat pointers, bail.
  if (destAddrSpace != ADDR_SPACE_BUFFER_FAT_POINTER && srcAddrSpace != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&memMoveInst);

  const MaybeAlign destAlignment = memMoveInst.getParamAlign(0);
  const MaybeAlign srcAlignment = memMoveInst.getParamAlign(1);

  // We assume LLVM is not introducing variable length mem moves.
  ConstantInt *const length = cast<ConstantInt>(memMoveInst.getArgOperand(2));

  // Get a vector type that is the length of the memmove.
  VectorType *const memoryType = FixedVectorType::get(m_builder->getInt8Ty(), length->getZExtValue());

  PointerType *const castDestType = memoryType->getPointerTo(destAddrSpace);
  Value *const castDest = m_builder->CreateBitCast(dest, castDestType);
  copyMetadata(castDest, &memMoveInst);

  PointerType *const castSrcType = memoryType->getPointerTo(srcAddrSpace);
  Value *const castSrc = m_builder->CreateBitCast(src, castSrcType);
  copyMetadata(castSrc, &memMoveInst);

  LoadInst *const srcLoad = m_builder->CreateAlignedLoad(memoryType, castSrc, srcAlignment);
  copyMetadata(srcLoad, &memMoveInst);

  StoreInst *const destStore = m_builder->CreateAlignedStore(srcLoad, castDest, destAlignment);
  copyMetadata(destStore, &memMoveInst);

  // Record the memmove instruction so we remember to delete it later.
  m_replacementMap[&memMoveInst] = std::make_pair(nullptr, nullptr);

  // Visit the load and store instructions to fold away fat pointer load/stores we might have just created.
  if (BitCastInst *const cast = dyn_cast<BitCastInst>(castDest))
    visitBitCastInst(*cast);

  if (BitCastInst *const cast = dyn_cast<BitCastInst>(castSrc))
    visitBitCastInst(*cast);

  visitLoadInst(*srcLoad);
  visitStoreInst(*destStore);
}

// =====================================================================================================================
// Visits "memset" instruction.
//
// @param memSetInst : The memset instruction
void PatchBufferOp::visitMemSetInst(MemSetInst &memSetInst) {
  Value *const dest = memSetInst.getArgOperand(0);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();

  // If the address spaces is a fat pointer.
  if (destAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER) {
    // Handling memset requires us to modify the CFG, so we need to do it after the initial visit pass.
    m_postVisitInsts.push_back(&memSetInst);
  }
}

// =====================================================================================================================
// Visits "phi" instruction.
//
// @param phiNode : The phi node
void PatchBufferOp::visitPHINode(PHINode &phiNode) {
  Type *const type = phiNode.getType();

  // If the type is not a pointer type, bail.
  if (!type->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (type->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  SmallVector<Value *, 8> incomings;

  for (unsigned i = 0, incomingValueCount = phiNode.getNumIncomingValues(); i < incomingValueCount; i++) {
    // PHIs require us to insert new incomings in the preceding basic blocks.
    m_builder->SetInsertPoint(phiNode.getIncomingBlock(i)->getTerminator());

    incomings.push_back(getPointerOperandAsInst(phiNode.getIncomingValue(i)));
  }

  Value *bufferDesc = nullptr;

  for (Value *const incoming : incomings) {
    Value *const incomingBufferDesc = m_replacementMap[incoming].first;

    if (!bufferDesc)
      bufferDesc = incomingBufferDesc;
    else if (bufferDesc != incomingBufferDesc) {
      bufferDesc = nullptr;
      break;
    }
  }

  m_builder->SetInsertPoint(&phiNode);

  // If the buffer descriptor was null, it means the PHI is changing the buffer descriptor, and we need a new PHI.
  if (!bufferDesc) {
    PHINode *const newPhiNode =
        m_builder->CreatePHI(FixedVectorType::get(Type::getInt32Ty(*m_context), 4), incomings.size());
    copyMetadata(newPhiNode, &phiNode);

    bool isInvariant = true;
    bool isDivergent = false;

    for (BasicBlock *const block : phiNode.blocks()) {
      const int blockIndex = phiNode.getBasicBlockIndex(block);
      assert(blockIndex >= 0);

      Value *incomingBufferDesc = m_replacementMap[incomings[blockIndex]].first;

      if (!incomingBufferDesc) {
        // If we cannot get an incoming buffer descriptor from the replacement map, it is unvisited yet. Generate an
        // incomplete phi and fix it later.
        incomingBufferDesc = UndefValue::get(newPhiNode->getType());
        m_incompletePhis[{newPhiNode, block}] = incomings[blockIndex];
      }

      newPhiNode->addIncoming(incomingBufferDesc, block);

      // If the incoming buffer descriptor is not invariant, the PHI cannot be marked invariant either.
      if (m_invariantSet.count(incomingBufferDesc) == 0)
        isInvariant = false;

      if (m_divergenceSet.count(incomingBufferDesc) > 0 || m_isDivergent(phiNode))
        isDivergent = true;
    }

    bufferDesc = newPhiNode;

    if (isInvariant)
      m_invariantSet.insert(bufferDesc);

    if (isDivergent)
      m_divergenceSet.insert(bufferDesc);
  }

  PHINode *const newPhiNode = m_builder->CreatePHI(getRemappedType(phiNode.getType()), incomings.size());
  copyMetadata(newPhiNode, &phiNode);

  m_replacementMap[&phiNode] = std::make_pair(bufferDesc, newPhiNode);

  for (BasicBlock *const block : phiNode.blocks()) {
    const int blockIndex = phiNode.getBasicBlockIndex(block);
    assert(blockIndex >= 0);

    Value *incomingIndex = m_replacementMap[incomings[blockIndex]].second;

    if (!incomingIndex) {
      // If we cannot get an incoming index from the replacement map, do the same as buffer descriptor.
      incomingIndex = UndefValue::get(newPhiNode->getType());
      m_incompletePhis[{newPhiNode, block}] = incomings[blockIndex];
    }

    newPhiNode->addIncoming(incomingIndex, block);
  }

  m_replacementMap[&phiNode] = std::make_pair(bufferDesc, newPhiNode);
}

// =====================================================================================================================
// Visits "select" instruction.
//
// @param selectInst : The select instruction
void PatchBufferOp::visitSelectInst(SelectInst &selectInst) {
  Type *const destType = selectInst.getType();

  // If the type is not a pointer type, bail.
  if (!destType->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (destType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&selectInst);

  Value *const value1 = getPointerOperandAsInst(selectInst.getTrueValue());
  Value *const value2 = getPointerOperandAsInst(selectInst.getFalseValue());

  Value *const bufferDesc1 = m_replacementMap[value1].first;
  Value *const bufferDesc2 = m_replacementMap[value2].first;

  Value *bufferDesc = nullptr;

  if (bufferDesc1 == bufferDesc2) {
    // If the buffer descriptors are the same, then no select needed.
    bufferDesc = bufferDesc1;
  } else if (!bufferDesc1 || !bufferDesc2) {
    // Select the non-nullptr buffer descriptor
    bufferDesc = bufferDesc1 ? bufferDesc1 : bufferDesc2;
  } else {
    // Otherwise we need to insert a select between the buffer descriptors.
    bufferDesc = m_builder->CreateSelect(selectInst.getCondition(), bufferDesc1, bufferDesc2);
    copyMetadata(bufferDesc, &selectInst);

    // If both incomings are invariant, mark the new select as invariant too.
    if (m_invariantSet.count(bufferDesc1) > 0 && m_invariantSet.count(bufferDesc2) > 0)
      m_invariantSet.insert(bufferDesc);
  }

  Value *const index1 = m_replacementMap[value1].second;
  Value *const index2 = m_replacementMap[value2].second;

  Value *const newSelect = m_builder->CreateSelect(selectInst.getCondition(), index1, index2);
  copyMetadata(newSelect, &selectInst);

  m_replacementMap[&selectInst] = std::make_pair(bufferDesc, newSelect);

  // If either of the incoming buffer descriptors are divergent, mark the new buffer descriptor as divergent too.
  if (m_divergenceSet.count(bufferDesc1) > 0 || m_divergenceSet.count(bufferDesc2) > 0)
    m_divergenceSet.insert(bufferDesc);
  else if (m_isDivergent(selectInst) && bufferDesc1 != bufferDesc2) {
    // Otherwise is the selection is divergent and the buffer descriptors do not match, mark divergent.
    m_divergenceSet.insert(bufferDesc);
  }
}

// =====================================================================================================================
// Visits "store" instruction.
//
// @param storeInst : The instruction
void PatchBufferOp::visitStoreInst(StoreInst &storeInst) {
  // If the address space of the store pointer is not a buffer fat pointer, bail.
  if (storeInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  replaceLoadStore(storeInst);

  // Record the store instruction so we remember to delete it later.
  m_replacementMap[&storeInst] = std::make_pair(nullptr, nullptr);
}

// =====================================================================================================================
// Visits "icmp" instruction.
//
// @param icmpInst : The instruction
void PatchBufferOp::visitICmpInst(ICmpInst &icmpInst) {
  Type *const type = icmpInst.getOperand(0)->getType();

  // If the type is not a pointer type, bail.
  if (!type->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (type->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  Value *const newICmp = replaceICmp(&icmpInst);

  copyMetadata(newICmp, &icmpInst);

  // Record the icmp instruction so we remember to delete it later.
  m_replacementMap[&icmpInst] = std::make_pair(nullptr, nullptr);

  icmpInst.replaceAllUsesWith(newICmp);
}

// =====================================================================================================================
// Visits "ptrtoint" instruction.
//
// @param ptrToIntInst : The "ptrtoint" instruction
void PatchBufferOp::visitPtrToIntInst(PtrToIntInst &ptrToIntInst) {
  Type *const type = ptrToIntInst.getOperand(0)->getType();

  // If the type is not a pointer type, bail.
  if (!type->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (type->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder->SetInsertPoint(&ptrToIntInst);

  Value *const pointer = getPointerOperandAsInst(ptrToIntInst.getOperand(0));

  Value *const newPtrToInt = m_builder->CreatePtrToInt(m_replacementMap[pointer].second, ptrToIntInst.getDestTy());

  copyMetadata(newPtrToInt, pointer);

  m_replacementMap[&ptrToIntInst] = std::make_pair(m_replacementMap[pointer].first, newPtrToInt);

  ptrToIntInst.replaceAllUsesWith(newPtrToInt);
}

// =====================================================================================================================
// Post-process visits "memcpy" instruction.
//
// @param memCpyInst : The memcpy instruction
void PatchBufferOp::postVisitMemCpyInst(MemCpyInst &memCpyInst) {
  Value *const dest = memCpyInst.getArgOperand(0);
  Value *const src = memCpyInst.getArgOperand(1);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();
  const unsigned srcAddrSpace = src->getType()->getPointerAddressSpace();

  m_builder->SetInsertPoint(&memCpyInst);

  const MaybeAlign destAlignment = memCpyInst.getParamAlign(0);
  const MaybeAlign srcAlignment = memCpyInst.getParamAlign(1);

  ConstantInt *const lengthConstant = dyn_cast<ConstantInt>(memCpyInst.getArgOperand(2));

  const uint64_t constantLength = lengthConstant ? lengthConstant->getZExtValue() : 0;

  // NOTE: If we do not have a constant length, or the constant length is bigger than the minimum we require to
  // generate a loop, we make a loop to handle the memcpy instead. If we did not generate a loop here for any
  // constant-length memcpy with a large number of bytes would generate thousands of load/store instructions that
  // causes LLVM's optimizations and our AMDGPU backend to crawl (and generate worse code!).
  if (!lengthConstant || constantLength > MinMemOpLoopBytes) {
    // NOTE: We want to perform our memcpy operation on the greatest stride of bytes possible (load/storing up to
    // dwordx4 or 16 bytes per loop iteration). If we have a constant length, we check if the alignment and
    // number of bytes to copy lets us load/store 16 bytes per loop iteration, and if not we check 8, then 4, then
    // 2. Worst case we have to load/store a single byte per loop.
    unsigned stride = !lengthConstant ? 1 : 16;

    while (stride != 1) {
      // We only care about dword alignment (4 bytes) so clamp the max check here to that.
      const unsigned minStride = std::min(stride, 4u);
      if (destAlignment.valueOrOne() >= minStride && srcAlignment.valueOrOne() >= minStride &&
          (constantLength % stride) == 0)
        break;

      stride /= 2;
    }

    Type *memoryType = nullptr;

    if (stride == 16) {
      memoryType = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
    } else {
      assert(stride < 8);
      memoryType = m_builder->getIntNTy(stride * 8);
    }

    Type *castDestType = memoryType->getPointerTo(destAddrSpace);
    Type *castSrcType = memoryType->getPointerTo(srcAddrSpace);

    Value *length = memCpyInst.getArgOperand(2);

    Type *const lengthType = length->getType();

    Value *const index =
        makeLoop(ConstantInt::get(lengthType, 0), length, ConstantInt::get(lengthType, stride), &memCpyInst);

    // Get the current index into our source pointer.
    assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(src->getType()->getScalarType(), m_builder->getInt8Ty()));
    Value *const srcPtr = m_builder->CreateGEP(m_builder->getInt8Ty(), src, index);
    copyMetadata(srcPtr, &memCpyInst);

    Value *const castSrc = m_builder->CreateBitCast(srcPtr, castSrcType);
    copyMetadata(castSrc, &memCpyInst);

    // Perform a load for the value.
    LoadInst *const srcLoad = m_builder->CreateLoad(memoryType, castSrc);
    copyMetadata(srcLoad, &memCpyInst);

    // Get the current index into our destination pointer.
    assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(dest->getType()->getScalarType(), m_builder->getInt8Ty()));
    Value *const destPtr = m_builder->CreateGEP(m_builder->getInt8Ty(), dest, index);
    copyMetadata(destPtr, &memCpyInst);

    Value *const castDest = m_builder->CreateBitCast(destPtr, castDestType);
    copyMetadata(castDest, &memCpyInst);

    // And perform a store for the value at this byte.
    StoreInst *const destStore = m_builder->CreateStore(srcLoad, castDest);
    copyMetadata(destStore, &memCpyInst);

    // Visit the newly added instructions to turn them into fat pointer variants.
    if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(srcPtr))
      visitGetElementPtrInst(*getElemPtr);

    if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(destPtr))
      visitGetElementPtrInst(*getElemPtr);

    if (BitCastInst *const cast = dyn_cast<BitCastInst>(castSrc))
      visitBitCastInst(*cast);

    if (BitCastInst *const cast = dyn_cast<BitCastInst>(castDest))
      visitBitCastInst(*cast);

    visitLoadInst(*srcLoad);

    visitStoreInst(*destStore);
  } else {
    // Get an vector type that is the length of the memcpy.
    VectorType *const memoryType = FixedVectorType::get(m_builder->getInt8Ty(), lengthConstant->getZExtValue());

    PointerType *const castDestType = memoryType->getPointerTo(destAddrSpace);
    Value *const castDest = m_builder->CreateBitCast(dest, castDestType);
    copyMetadata(castDest, &memCpyInst);

    PointerType *const castSrcType = memoryType->getPointerTo(srcAddrSpace);
    Value *const castSrc = m_builder->CreateBitCast(src, castSrcType);
    copyMetadata(castSrc, &memCpyInst);

    LoadInst *const srcLoad = m_builder->CreateAlignedLoad(memoryType, castSrc, srcAlignment);
    copyMetadata(srcLoad, &memCpyInst);

    StoreInst *const destStore = m_builder->CreateAlignedStore(srcLoad, castDest, destAlignment);
    copyMetadata(destStore, &memCpyInst);

    // Visit the newly added instructions to turn them into fat pointer variants.
    if (BitCastInst *const cast = dyn_cast<BitCastInst>(castDest))
      visitBitCastInst(*cast);

    if (BitCastInst *const cast = dyn_cast<BitCastInst>(castSrc))
      visitBitCastInst(*cast);

    visitLoadInst(*srcLoad);
    visitStoreInst(*destStore);
  }

  // Record the memcpy instruction so we remember to delete it later.
  m_replacementMap[&memCpyInst] = std::make_pair(nullptr, nullptr);
}

// =====================================================================================================================
// Post-process visits "memset" instruction.
//
// @param memSetInst : The memset instruction
void PatchBufferOp::postVisitMemSetInst(MemSetInst &memSetInst) {
  Value *const dest = memSetInst.getArgOperand(0);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();

  m_builder->SetInsertPoint(&memSetInst);

  Value *const value = memSetInst.getArgOperand(1);

  const MaybeAlign destAlignment = memSetInst.getParamAlign(0);

  ConstantInt *const lengthConstant = dyn_cast<ConstantInt>(memSetInst.getArgOperand(2));

  const uint64_t constantLength = lengthConstant ? lengthConstant->getZExtValue() : 0;

  // NOTE: If we do not have a constant length, or the constant length is bigger than the minimum we require to
  // generate a loop, we make a loop to handle the memcpy instead. If we did not generate a loop here for any
  // constant-length memcpy with a large number of bytes would generate thousands of load/store instructions that
  // causes LLVM's optimizations and our AMDGPU backend to crawl (and generate worse code!).
  if (!lengthConstant || constantLength > MinMemOpLoopBytes) {
    // NOTE: We want to perform our memset operation on the greatest stride of bytes possible (load/storing up to
    // dwordx4 or 16 bytes per loop iteration). If we have a constant length, we check if the alignment and
    // number of bytes to copy lets us load/store 16 bytes per loop iteration, and if not we check 8, then 4, then
    // 2. Worst case we have to load/store a single byte per loop.
    unsigned stride = !lengthConstant ? 1 : 16;

    while (stride != 1) {
      // We only care about dword alignment (4 bytes) so clamp the max check here to that.
      const unsigned minStride = std::min(stride, 4u);
      if (destAlignment.valueOrOne() >= minStride && (constantLength % stride) == 0)
        break;

      stride /= 2;
    }

    Type *castDestType = nullptr;

    if (stride == 16)
      castDestType = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
    else {
      assert(stride < 8);
      castDestType = m_builder->getIntNTy(stride * 8);
    }

    Value *newValue = nullptr;

    if (Constant *const constVal = dyn_cast<Constant>(value)) {
      newValue = ConstantVector::getSplat(ElementCount::get(stride, false), constVal);
      newValue = m_builder->CreateBitCast(newValue, castDestType);
      copyMetadata(newValue, &memSetInst);
    } else {
      Value *const memoryPointer = m_builder->CreateAlloca(castDestType);
      copyMetadata(memoryPointer, &memSetInst);

      Type *const int8PtrTy = m_builder->getInt8Ty()->getPointerTo(ADDR_SPACE_PRIVATE);
      Value *const castMemoryPointer = m_builder->CreateBitCast(memoryPointer, int8PtrTy);
      copyMetadata(castMemoryPointer, &memSetInst);

      Value *const memSet = m_builder->CreateMemSet(castMemoryPointer, value, stride, Align());
      copyMetadata(memSet, &memSetInst);

      newValue = m_builder->CreateLoad(castDestType, memoryPointer);
      copyMetadata(newValue, &memSetInst);
    }

    Value *const length = memSetInst.getArgOperand(2);

    Type *const lengthType = length->getType();

    Value *const index =
        makeLoop(ConstantInt::get(lengthType, 0), length, ConstantInt::get(lengthType, stride), &memSetInst);

    // Get the current index into our destination pointer.
    assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(dest->getType()->getScalarType(), m_builder->getInt8Ty()));
    Value *const destPtr = m_builder->CreateGEP(m_builder->getInt8Ty(), dest, index);
    copyMetadata(destPtr, &memSetInst);

    Value *const castDest = m_builder->CreateBitCast(destPtr, castDestType->getPointerTo(destAddrSpace));
    copyMetadata(castDest, &memSetInst);

    // And perform a store for the value at this byte.
    StoreInst *const destStore = m_builder->CreateStore(newValue, castDest);
    copyMetadata(destStore, &memSetInst);

    if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(destPtr))
      visitGetElementPtrInst(*getElemPtr);

    if (BitCastInst *const cast = dyn_cast<BitCastInst>(castDest))
      visitBitCastInst(*cast);

    visitStoreInst(*destStore);
  } else {
    // Get a vector type that is the length of the memset.
    VectorType *const memoryType = FixedVectorType::get(m_builder->getInt8Ty(), lengthConstant->getZExtValue());

    Value *newValue = nullptr;

    if (Constant *const constVal = dyn_cast<Constant>(value))
      newValue = ConstantVector::getSplat(cast<VectorType>(memoryType)->getElementCount(), constVal);
    else {
      Value *const memoryPointer = m_builder->CreateAlloca(memoryType);
      copyMetadata(memoryPointer, &memSetInst);

      Type *const int8PtrTy = m_builder->getInt8Ty()->getPointerTo(ADDR_SPACE_PRIVATE);
      Value *const castMemoryPointer = m_builder->CreateBitCast(memoryPointer, int8PtrTy);
      copyMetadata(castMemoryPointer, &memSetInst);

      Value *const memSet = m_builder->CreateMemSet(castMemoryPointer, value,
                                                    cast<FixedVectorType>(memoryType)->getNumElements(), Align());
      copyMetadata(memSet, &memSetInst);

      newValue = m_builder->CreateLoad(memoryType, memoryPointer);
      copyMetadata(newValue, &memSetInst);
    }

    PointerType *const castDestType = memoryType->getPointerTo(destAddrSpace);
    Value *const castDest = m_builder->CreateBitCast(dest, castDestType);
    copyMetadata(castDest, &memSetInst);

    if (BitCastInst *const cast = dyn_cast<BitCastInst>(castDest))
      visitBitCastInst(*cast);

    StoreInst *const destStore = m_builder->CreateAlignedStore(newValue, castDest, destAlignment);
    copyMetadata(destStore, &memSetInst);
    visitStoreInst(*destStore);
  }

  // Record the memset instruction so we remember to delete it later.
  m_replacementMap[&memSetInst] = std::make_pair(nullptr, nullptr);
}

// =====================================================================================================================
// Get a pointer operand as an instruction.
//
// @param value : The pointer operand value to get as an instruction.
Value *PatchBufferOp::getPointerOperandAsInst(Value *const value) {
  // If the value is already an instruction, return it.
  if (Instruction *const inst = dyn_cast<Instruction>(value))
    return inst;

  // If the value is a constant (i.e., null pointer), return it.
  if (isa<Constant>(value)) {
    Constant *const nullPointer = ConstantPointerNull::get(getRemappedType(value->getType()));
    m_replacementMap[value] = std::make_pair(nullptr, nullPointer);
    return value;
  }

  ConstantExpr *const constExpr = cast<ConstantExpr>(value);

  Instruction *const newInst = m_builder->Insert(constExpr->getAsInstruction());

  // Visit the new instruction we made to ensure we remap the value.
  visit(newInst);

  // Check that the new instruction was definitely in the replacement map.
  assert(m_replacementMap.count(newInst) > 0);

  return newInst;
}

// =====================================================================================================================
// Extract the 64-bit address from a buffer descriptor.
//
// @param bufferDesc : The buffer descriptor to extract the address from
Value *PatchBufferOp::getBaseAddressFromBufferDesc(Value *const bufferDesc) const {
  Type *const descType = bufferDesc->getType();

  assert(descType->isVectorTy());
  assert(cast<FixedVectorType>(descType)->getNumElements() == 4);
  assert(cast<VectorType>(descType)->getElementType()->isIntegerTy(32));

  // Get the base address of our buffer by extracting the two components with the 48-bit address, and masking.
  Value *baseAddr = m_builder->CreateShuffleVector(bufferDesc, UndefValue::get(descType), ArrayRef<int>{0, 1});
  Value *const baseAddrMask = ConstantVector::get({m_builder->getInt32(0xFFFFFFFF), m_builder->getInt32(0xFFFF)});
  baseAddr = m_builder->CreateAnd(baseAddr, baseAddrMask);
  baseAddr = m_builder->CreateBitCast(baseAddr, m_builder->getInt64Ty());
  return m_builder->CreateIntToPtr(baseAddr, m_builder->getInt8Ty()->getPointerTo(ADDR_SPACE_GLOBAL));
}

// =====================================================================================================================
// Copy all metadata from one value to another.
//
// @param [in/out] dest : The destination to copy metadata onto.
// @param src : The source to copy metadata from.
void PatchBufferOp::copyMetadata(Value *const dest, const Value *const src) const {
  Instruction *const destInst = dyn_cast<Instruction>(dest);

  // If the destination is not an instruction, bail.
  if (!destInst)
    return;

  const Instruction *const srcInst = dyn_cast<Instruction>(src);

  // If the source is not an instruction, bail.
  if (!srcInst)
    return;

  SmallVector<std::pair<unsigned, MDNode *>, 8> allMetaNodes;
  srcInst->getAllMetadata(allMetaNodes);

  for (auto metaNode : allMetaNodes)
    destInst->setMetadata(metaNode.first, metaNode.second);
}

// =====================================================================================================================
// Get the remapped type for a fat pointer that is usable in indexing. We use the 32-bit wide constant address space for
// this, as it means when we convert the GEP to an integer, the GEP can be converted losslessly to a 32-bit integer,
// which just happens to be what the MUBUF instructions expect.
//
// @param type : The type to remap.
PointerType *PatchBufferOp::getRemappedType(Type *const type) const {
  return PointerType::getWithSamePointeeType(cast<PointerType>(type), ADDR_SPACE_CONST_32BIT);
}

// =====================================================================================================================
// Remove any users that are invariant starts, returning if any were removed.
//
// @param value : The value to check the users of.
bool PatchBufferOp::removeUsersForInvariantStarts(Value *const value) {
  bool modified = false;

  for (User *const user : value->users()) {
    if (BitCastInst *const bitCast = dyn_cast<BitCastInst>(user)) {
      // Remove any users of the bitcast too.
      if (removeUsersForInvariantStarts(bitCast))
        modified = true;
    } else {
      IntrinsicInst *const intrinsic = dyn_cast<IntrinsicInst>(user);

      // If the user isn't an intrinsic, bail.
      if (!intrinsic)
        continue;

      // If the intrinsic is not an invariant load, bail.
      if (intrinsic->getIntrinsicID() != Intrinsic::invariant_start)
        continue;

      // Remember the intrinsic because we will want to delete it.
      m_replacementMap[intrinsic] = std::make_pair(nullptr, nullptr);

      modified = true;
    }
  }

  return modified;
}

// =====================================================================================================================
// Replace a fat pointer load or store with the required intrinsics.
//
// @param inst : The instruction to replace.
Value *PatchBufferOp::replaceLoadStore(Instruction &inst) {
  LoadInst *const loadInst = dyn_cast<LoadInst>(&inst);
  StoreInst *const storeInst = dyn_cast<StoreInst>(&inst);

  // Either load instruction or store instruction is valid (not both)
  assert((!loadInst) != (!storeInst));

  bool isLoad = loadInst;
  Type *type = nullptr;
  Value *pointerOperand = nullptr;
  AtomicOrdering ordering = AtomicOrdering::NotAtomic;
  Align alignment;
  SyncScope::ID syncScopeID = 0;

  if (isLoad) {
    type = loadInst->getType();
    pointerOperand = loadInst->getPointerOperand();
    ordering = loadInst->getOrdering();
    alignment = loadInst->getAlign();
    syncScopeID = loadInst->getSyncScopeID();
  } else {
    type = storeInst->getValueOperand()->getType();
    pointerOperand = storeInst->getPointerOperand();
    ordering = storeInst->getOrdering();
    alignment = storeInst->getAlign();
    syncScopeID = storeInst->getSyncScopeID();
  }

  m_builder->SetInsertPoint(&inst);

  Value *const pointer = getPointerOperandAsInst(pointerOperand);

  const DataLayout &dataLayout = m_builder->GetInsertBlock()->getModule()->getDataLayout();

  const unsigned bytesToHandle = static_cast<unsigned>(dataLayout.getTypeStoreSize(type));

  bool isInvariant = false;
  if (isLoad) {
    isInvariant = m_invariantSet.count(m_replacementMap[pointer].first) > 0 ||
                  loadInst->getMetadata(LLVMContext::MD_invariant_load);
  }

  const bool isSlc = inst.getMetadata(LLVMContext::MD_nontemporal);
  const bool isGlc = ordering != AtomicOrdering::NotAtomic;
  const bool isDlc = isGlc; // For buffer load on GFX10+, we set DLC = GLC

  Value *const bufferDesc = m_replacementMap[pointer].first;
  Value *const baseIndex = m_builder->CreatePtrToInt(m_replacementMap[pointer].second, m_builder->getInt32Ty());

  // If our buffer descriptor is divergent, need to handle that differently.
  if (m_divergenceSet.count(bufferDesc) > 0) {
    Value *const baseAddr = getBaseAddressFromBufferDesc(bufferDesc);

    // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
    Value *const bound = m_builder->CreateExtractElement(bufferDesc, 2);
    Value *const inBound = m_builder->CreateICmpULT(baseIndex, bound);
    Value *const newBaseIndex = m_builder->CreateSelect(inBound, baseIndex, m_builder->getInt32(0));

    // Add on the index to the address.
    Value *pointer = m_builder->CreateGEP(m_builder->getInt8Ty(), baseAddr, newBaseIndex);

    pointer = m_builder->CreateBitCast(pointer, type->getPointerTo(ADDR_SPACE_GLOBAL));

    if (isLoad) {
      LoadInst *const newLoad = m_builder->CreateAlignedLoad(type, pointer, alignment, loadInst->isVolatile());
      newLoad->setOrdering(ordering);
      newLoad->setSyncScopeID(syncScopeID);
      copyMetadata(newLoad, loadInst);

      if (isInvariant)
        newLoad->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_context, None));

      return newLoad;
    }
    StoreInst *const newStore =
        m_builder->CreateAlignedStore(storeInst->getValueOperand(), pointer, alignment, storeInst->isVolatile());
    newStore->setOrdering(ordering);
    newStore->setSyncScopeID(syncScopeID);
    copyMetadata(newStore, storeInst);

    return newStore;
  }

  switch (ordering) {
  case AtomicOrdering::Release:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    m_builder->CreateFence(AtomicOrdering::Release, syncScopeID);
    break;
  default:
    break;
  }

  SmallVector<Value *, 8> parts;
  Type *smallestType = nullptr;
  unsigned smallestByteSize = 4;

  if (alignment < 2 || (bytesToHandle & 0x1) != 0) {
    smallestByteSize = 1;
    smallestType = m_builder->getInt8Ty();
  } else if (alignment < 4 || (bytesToHandle & 0x3) != 0) {
    smallestByteSize = 2;
    smallestType = m_builder->getInt16Ty();
  } else {
    smallestByteSize = 4;
    smallestType = m_builder->getInt32Ty();
  }

  // Load: Create an undef vector whose total size is the number of bytes we
  // loaded.
  // Store: Bitcast our value-to-store to a vector of smallest byte size.
  Type *const castType = FixedVectorType::get(smallestType, bytesToHandle / smallestByteSize);

  Value *storeValue = nullptr;
  if (!isLoad) {
    storeValue = storeInst->getValueOperand();
    Type *storeTy = storeValue->getType();
    if (storeTy->isArrayTy()) {
      const unsigned elemCount = cast<ArrayType>(storeTy)->getNumElements();
      Value *castValue = UndefValue::get(castType);
      for (unsigned elemIdx = 0; elemIdx != elemCount; ++elemIdx) {
        Value *elem = m_builder->CreateExtractValue(storeValue, elemIdx);
        elem = m_builder->CreateBitCast(elem, smallestType);
        castValue = m_builder->CreateInsertElement(castValue, elem, elemIdx);
      }
      storeValue = castValue;
      copyMetadata(storeValue, storeInst);
    } else {
      if (storeTy->isPointerTy()) {
        storeValue = m_builder->CreatePtrToInt(storeValue, m_builder->getIntNTy(bytesToHandle * 8));
        copyMetadata(storeValue, storeInst);
      }

      storeValue = m_builder->CreateBitCast(storeValue, castType);
      copyMetadata(storeValue, storeInst);
    }
  }

  // The index in storeValue which we use next
  unsigned storeIndex = 0;

  unsigned remainingBytes = bytesToHandle;
  while (remainingBytes > 0) {
    const unsigned offset = bytesToHandle - remainingBytes;
    Value *offsetVal = offset == 0 ? baseIndex : m_builder->CreateAdd(baseIndex, m_builder->getInt32(offset));

    Type *intAccessType = nullptr;
    unsigned accessSize = 0;

    // Handle the greatest possible size
    if (alignment >= 4 && remainingBytes >= 4) {
      if (remainingBytes >= 16) {
        intAccessType = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
        accessSize = 16;
      } else if (remainingBytes >= 12 && !isInvariant) {
        intAccessType = FixedVectorType::get(Type::getInt32Ty(*m_context), 3);
        accessSize = 12;
      } else if (remainingBytes >= 8) {
        intAccessType = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);
        accessSize = 8;
      } else {
        // remainingBytes >= 4
        intAccessType = Type::getInt32Ty(*m_context);
        accessSize = 4;
      }
    } else if (alignment >= 2 && remainingBytes >= 2) {
      intAccessType = Type::getInt16Ty(*m_context);
      accessSize = 2;
    } else {
      intAccessType = Type::getInt8Ty(*m_context);
      accessSize = 1;
    }
    assert(intAccessType);
    assert(accessSize != 0);

    Value *part = nullptr;

    CoherentFlag coherent = {};
    coherent.bits.glc = isGlc;
    if (!isInvariant)
      coherent.bits.slc = isSlc;

    if (isLoad) {
      if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 10) {
        // TODO For stores?
        coherent.bits.dlc = isDlc;
      }
      if (isInvariant && accessSize >= 4) {
        CallInst *call = m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_buffer_load, intAccessType,
                                                    {bufferDesc, offsetVal, m_builder->getInt32(coherent.u32All)});
        call->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(*m_context, None));
        part = call;
      } else {
        unsigned intrinsicID = Intrinsic::amdgcn_raw_buffer_load;
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Atomic load loses memory semantics
#else
        if (ordering != AtomicOrdering::NotAtomic)
          intrinsicID = Intrinsic::amdgcn_raw_atomic_buffer_load;
#endif
        part = m_builder->CreateIntrinsic(
            intrinsicID, intAccessType,
            {bufferDesc, offsetVal, m_builder->getInt32(0), m_builder->getInt32(coherent.u32All)});
      }
    } else {
      // Store
      unsigned compCount = accessSize / smallestByteSize;
      part = UndefValue::get(FixedVectorType::get(smallestType, compCount));

      for (unsigned i = 0; i < compCount; i++) {
        Value *const storeElem = m_builder->CreateExtractElement(storeValue, storeIndex++);
        part = m_builder->CreateInsertElement(part, storeElem, i);
      }
      part = m_builder->CreateBitCast(part, intAccessType);
      copyMetadata(part, &inst);
      part = m_builder->CreateIntrinsic(
          Intrinsic::amdgcn_raw_buffer_store, intAccessType,
          {part, bufferDesc, offsetVal, m_builder->getInt32(0), m_builder->getInt32(coherent.u32All)});
    }

    copyMetadata(part, &inst);
    if (isLoad)
      parts.push_back(part);

    remainingBytes -= accessSize;
  }

  Value *newInst = nullptr;
  if (isLoad) {
    if (parts.size() == 1) {
      // We do not have to create a vector if we did only one load
      newInst = parts.front();
    } else {
      // And create an undef vector whose total size is the number of bytes we loaded.
      newInst = UndefValue::get(FixedVectorType::get(smallestType, bytesToHandle / smallestByteSize));

      unsigned index = 0;

      for (Value *part : parts) {
        // Get the byte size of our load part.
        const unsigned byteSize = static_cast<unsigned>(dataLayout.getTypeStoreSize(part->getType()));

        // Bitcast it to a vector of the smallest load type.
        FixedVectorType *const castType = FixedVectorType::get(smallestType, byteSize / smallestByteSize);
        part = m_builder->CreateBitCast(part, castType);
        copyMetadata(part, &inst);

        // Run through the elements of our bitcasted type and insert them into the main load.
        for (unsigned i = 0, compCount = static_cast<unsigned>(castType->getNumElements()); i < compCount; i++) {
          Value *const elem = m_builder->CreateExtractElement(part, i);
          copyMetadata(elem, &inst);
          newInst = m_builder->CreateInsertElement(newInst, elem, index++);
          copyMetadata(newInst, &inst);
        }
      }
    }
    assert(newInst);

    if (type->isPointerTy()) {
      newInst = m_builder->CreateBitCast(newInst, m_builder->getIntNTy(bytesToHandle * 8));
      copyMetadata(newInst, &inst);
      newInst = m_builder->CreateIntToPtr(newInst, type);
      copyMetadata(newInst, &inst);
    } else {
      newInst = m_builder->CreateBitCast(newInst, type);
      copyMetadata(newInst, &inst);
    }
  }

  switch (ordering) {
  case AtomicOrdering::Acquire:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    m_builder->CreateFence(AtomicOrdering::Acquire, syncScopeID);
    break;
  default:
    break;
  }

  return newInst;
}

// =====================================================================================================================
// Replace fat pointers icmp with the instruction required to do the icmp.
//
// @param iCmpInst : The "icmp" instruction to replace.
Value *PatchBufferOp::replaceICmp(ICmpInst *const iCmpInst) {
  m_builder->SetInsertPoint(iCmpInst);

  SmallVector<Value *, 2> bufferDescs;
  SmallVector<Value *, 2> indices;
  for (int i = 0; i < 2; ++i) {
    Value *const operand = getPointerOperandAsInst(iCmpInst->getOperand(i));
    bufferDescs.push_back(m_replacementMap[operand].first);
    indices.push_back(m_builder->CreatePtrToInt(m_replacementMap[operand].second, m_builder->getInt32Ty()));
  }

  Type *const bufferDescTy = bufferDescs[0]->getType();

  assert(bufferDescTy->isVectorTy());
  assert(cast<FixedVectorType>(bufferDescTy)->getNumElements() == 4);
  assert(cast<VectorType>(bufferDescTy)->getElementType()->isIntegerTy(32));
  (void(bufferDescTy)); // unused
  assert(iCmpInst->getPredicate() == ICmpInst::ICMP_EQ || iCmpInst->getPredicate() == ICmpInst::ICMP_NE);

  Value *bufferDescICmp = m_builder->getFalse();
  if (!bufferDescs[0] && !bufferDescs[1])
    bufferDescICmp = m_builder->getTrue();
  else if (bufferDescs[0] && bufferDescs[1]) {
    Value *const bufferDescEqual = m_builder->CreateICmpEQ(bufferDescs[0], bufferDescs[1]);

    bufferDescICmp = m_builder->CreateExtractElement(bufferDescEqual, static_cast<uint64_t>(0));
    for (unsigned i = 1; i < 4; ++i) {
      Value *bufferDescElemEqual = m_builder->CreateExtractElement(bufferDescEqual, i);
      bufferDescICmp = m_builder->CreateAnd(bufferDescICmp, bufferDescElemEqual);
    }
  }

  Value *indexICmp = m_builder->CreateICmpEQ(indices[0], indices[1]);

  Value *newICmp = m_builder->CreateAnd(bufferDescICmp, indexICmp);

  if (iCmpInst->getPredicate() == ICmpInst::ICMP_NE)
    newICmp = m_builder->CreateNot(newICmp);

  return newICmp;
}

// =====================================================================================================================
// Make a loop, returning the value of the loop counter. This modifies the insertion point of the builder.
//
// @param loopStart : The start index of the loop.
// @param loopEnd : The end index of the loop.
// @param loopStride : The stride of the loop.
// @param insertPos : The position to insert the loop in the instruction stream.
Instruction *PatchBufferOp::makeLoop(Value *const loopStart, Value *const loopEnd, Value *const loopStride,
                                     Instruction *const insertPos) {
  Value *const initialCond = m_builder->CreateICmpNE(loopStart, loopEnd);

  BasicBlock *const origBlock = insertPos->getParent();

  Instruction *const terminator = SplitBlockAndInsertIfThen(initialCond, insertPos, false);

  m_builder->SetInsertPoint(terminator);

  // Create a phi node for the loop counter.
  PHINode *const loopCounter = m_builder->CreatePHI(loopStart->getType(), 2);
  copyMetadata(loopCounter, insertPos);

  // Set the loop counter to start value (initialization).
  loopCounter->addIncoming(loopStart, origBlock);

  // Calculate the next value of the loop counter by doing loopCounter + loopStride.
  Value *const loopNextValue = m_builder->CreateAdd(loopCounter, loopStride);
  copyMetadata(loopNextValue, insertPos);

  // And set the loop counter to the next value.
  loopCounter->addIncoming(loopNextValue, terminator->getParent());

  // Our loop condition is just whether the next value of the loop counter is less than the end value.
  Value *const cond = m_builder->CreateICmpULT(loopNextValue, loopEnd);
  copyMetadata(cond, insertPos);

  // And our replacement terminator just branches back to the if body if there is more loop iterations to be done.
  Instruction *const newTerminator =
      m_builder->CreateCondBr(cond, terminator->getParent(), terminator->getSuccessor(0));
  copyMetadata(newTerminator, insertPos);

  terminator->eraseFromParent();

  m_builder->SetInsertPoint(newTerminator);

  return loopCounter;
}

// =====================================================================================================================
// Fix incomplete phi incoming values
void PatchBufferOp::fixIncompletePhis() {
  for (auto phi : m_incompletePhis) {
    PHINode *phiNode = phi.first.first;
    BasicBlock *incomingBlock = phi.first.second;
    Value *incoming = phi.second;

    assert(isa<UndefValue>(phiNode->getIncomingValueForBlock(incomingBlock)));
    assert(phiNode->getType()->isVectorTy() || phiNode->getType()->isPointerTy());

    if (phiNode->getType()->isVectorTy())
      // It is a buffer descriptor
      phiNode->setIncomingValueForBlock(incomingBlock, m_replacementMap[incoming].first);
    else
      // It is an index
      phiNode->setIncomingValueForBlock(incomingBlock, m_replacementMap[incoming].second);
  }
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for buffer operations.
INITIALIZE_PASS_BEGIN(LegacyPatchBufferOp, DEBUG_TYPE, "Patch LLVM for buffer operations", false, false)
INITIALIZE_PASS_DEPENDENCY(LegacyDivergenceAnalysis)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(LegacyPatchBufferOp, DEBUG_TYPE, "Patch LLVM for buffer operations", false, false)
