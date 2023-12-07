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
 * @file  PatchBufferOp.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchBufferOp.h"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/PostOrderIterator.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
// Old version of the code
#include "llvm/Analysis/DivergenceAnalysis.h"
#else
// New version of the code (also handles unknown version, which we treat as latest)
#endif
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

namespace {

struct PatchBufferOpImpl {
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  PatchBufferOpImpl(LLVMContext &context, PipelineState &pipelineState, DivergenceInfo &divergenceInfo);
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  PatchBufferOpImpl(LLVMContext &context, PipelineState &pipelineState, UniformityInfo &uniformityInfo);
#endif

  bool run(Function &function);

  TypeLowering m_typeLowering;
  BufferOpLowering m_bufferOpLowering;
};

} // anonymous namespace

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(PatchBufferOpImpl, m_typeLowering)
LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(PatchBufferOpImpl, m_bufferOpLowering)

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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  DivergenceInfo &uniformityInfo = analysisManager.getResult<DivergenceAnalysis>(function);
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  UniformityInfo &uniformityInfo = analysisManager.getResult<UniformityInfoAnalysis>(function);
#endif

  PatchBufferOpImpl impl(function.getContext(), *pipelineState, uniformityInfo);
  if (impl.run(function))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Construct the per-run temporaries of the PatchBufferOp pass.
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
// Old version of the code
PatchBufferOpImpl::PatchBufferOpImpl(LLVMContext &context, PipelineState &pipelineState, DivergenceInfo &divergenceInfo)
    : m_typeLowering(context), m_bufferOpLowering(m_typeLowering, pipelineState, divergenceInfo) {
}
#else
// New version of the code (also handles unknown version, which we treat as latest)
PatchBufferOpImpl::PatchBufferOpImpl(LLVMContext &context, PipelineState &pipelineState, UniformityInfo &uniformityInfo)
    : m_typeLowering(context), m_bufferOpLowering(m_typeLowering, pipelineState, uniformityInfo) {
}
#endif

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in,out] function : LLVM function to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchBufferOpImpl::run(Function &function) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Buffer-Op\n");

  static const auto visitor = llvm_dialects::VisitorBuilder<PatchBufferOpImpl>()
                                  .nest(&BufferOpLowering::registerVisitors)
                                  .nest(&TypeLowering::registerVisitors)
                                  .build();

  visitor.visit(*this, function);

  m_typeLowering.finishPhis();
  m_bufferOpLowering.finish();
  return m_typeLowering.finishCleanup();
}

// =====================================================================================================================
// Type lowering rule that lowers a fat buffer pointer to a descriptor and a 32-bit proxy pointer for the offset.
//
// @param typeLowering : the calling TypeLowering object
// @param type : the type to be converted
static SmallVector<Type *> convertBufferPointer(TypeLowering &typeLowering, Type *type) {
  SmallVector<Type *> types;

  if (auto *pointerType = dyn_cast<PointerType>(type)) {
    if (pointerType->getAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
      types.push_back(FixedVectorType::get(Type::getInt32Ty(type->getContext()), 4));
      types.push_back(PointerType::get(type->getContext(), ADDR_SPACE_CONST_32BIT));
    }
  }

  return types;
}

// =====================================================================================================================
// Construct the BufferOpLowering object.
//
// @param typeLowering : the TypeLowering object to be used
// @param pipelineState : the PipelineState object
// @param uniformityInfo : the uniformity analysis result
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
// Old version of the code
BufferOpLowering::BufferOpLowering(TypeLowering &typeLowering, PipelineState &pipelineState,
                                   DivergenceInfo &uniformityInfo)
#else
// New version of the code (also handles unknown version, which we treat as latest)
BufferOpLowering::BufferOpLowering(TypeLowering &typeLowering, PipelineState &pipelineState,
                                   UniformityInfo &uniformityInfo)
#endif
    : m_typeLowering(typeLowering), m_builder(typeLowering.getContext()), m_pipelineState(pipelineState),
      m_uniformityInfo(uniformityInfo) {
  m_typeLowering.addRule(&convertBufferPointer);

  m_offsetType = m_builder.getPtrTy(ADDR_SPACE_CONST_32BIT);
}

// =====================================================================================================================
// Register the visitors for buffer pointer & operation lowering with the given VisitorBuilder.
//
// @param builder : the VisitorBuilder with which to register the callbacks
void BufferOpLowering::registerVisitors(llvm_dialects::VisitorBuilder<BufferOpLowering> &builder) {
  builder.setStrategy(llvm_dialects::VisitorStrategy::ReversePostOrder);
  builder.add(&BufferOpLowering::visitAtomicCmpXchgInst);
  builder.add(&BufferOpLowering::visitAtomicRMWInst);
  builder.add(&BufferOpLowering::visitBitCastInst);
  builder.add(&BufferOpLowering::visitBufferDescToPtr);
  builder.add(&BufferOpLowering::visitBufferLength);
  builder.add(&BufferOpLowering::visitBufferPtrDiff);
  builder.add(&BufferOpLowering::visitGetElementPtrInst);
  builder.add(&BufferOpLowering::visitLoadInst);
  builder.add(&BufferOpLowering::visitMemCpyInst);
  builder.add(&BufferOpLowering::visitMemMoveInst);
  builder.add(&BufferOpLowering::visitMemSetInst);
  builder.add(&BufferOpLowering::visitPhiInst);
  builder.add(&BufferOpLowering::visitStoreInst);
  builder.add(&BufferOpLowering::visitICmpInst);
  builder.addIntrinsic(Intrinsic::invariant_start, &BufferOpLowering::visitInvariantStart);
}

// =====================================================================================================================
// Lower all instructions that were postponed previously.
//
// This must be called *after* TypeLowering::finishPhis() but before TypeLowering::finishCleanup().
void BufferOpLowering::finish() {
  // If PHI nodes on descriptors weren't optimized away, assume that divergence in the original phi was due to sync
  // divergence, and the new phi should be divergent as well.
  //
  // TODO: UniformityAnalysis should really be updatable/preservable

  for (PHINode *originalPhi : m_divergentPhis) {
    auto values = m_typeLowering.getValue(originalPhi);
    if (auto *newPhi = dyn_cast<PHINode>(values[0])) {
      if (newPhi->getParent() == originalPhi->getParent()) {
        DescriptorInfo &di = m_descriptors[newPhi];
        di.divergent = true;
      }
    }
  }

  static const auto visitor = llvm_dialects::VisitorBuilder<BufferOpLowering>()
                                  .add(&BufferOpLowering::postVisitLoadInst)
                                  .add(&BufferOpLowering::postVisitMemCpyInst)
                                  .add(&BufferOpLowering::postVisitMemSetInst)
                                  .add(&BufferOpLowering::postVisitStoreInst)
                                  .build();

  SmallVector<Instruction *> instructions;
  std::swap(instructions, m_postVisitInsts);
  for (Instruction *inst : instructions)
    visitor.visit(*this, *inst);
  assert(m_postVisitInsts.empty());
}

// =====================================================================================================================
// Get the descriptor info describing whether the descriptor is invariant and/or divergent.
//
// This function resolves invariance and divergence to a "known" state if necessary.
//
// @param desc : the descriptor
BufferOpLowering::DescriptorInfo BufferOpLowering::getDescriptorInfo(Value *desc) {
  DescriptorInfo di = m_descriptors[desc];
  if (di.invariant.has_value() && di.divergent.has_value())
    return di;

  // Resolve by first finding all necessary roots and then performing an aggressive data flow fixed point iteration,
  // i.e. start with the tacit assumption that all descriptors are uniform and invariant.
  DenseSet<Value *> seen;
  SmallVector<Value *> searchWorklist;
  SmallVector<Value *> propagationWorklist;
  Value *current = desc;
  for (;;) {
    if (seen.insert(current).second) {
      auto &di = m_descriptors[current];

      if (!di.invariant.has_value() || !di.divergent.has_value()) {
        if (auto *phi = dyn_cast<PHINode>(current)) {
          for (Value *incoming : phi->incoming_values())
            searchWorklist.push_back(incoming);
        } else if (auto *select = dyn_cast<SelectInst>(current)) {
          assert(select->getOperandUse(0).get() == select->getCondition());
          if (m_uniformityInfo.isDivergentUse(select->getOperandUse(0)))
            di.divergent = true;

          if (!di.invariant.has_value() || !di.divergent.has_value()) {
            searchWorklist.push_back(select->getTrueValue());
            searchWorklist.push_back(select->getFalseValue());
          }
        } else {
          // Make conservative assumptions for unhandled instructions.
          bool isConstant = isa<Constant>(current);
          auto &di = m_descriptors[current];
          if (!di.invariant.has_value())
            di.invariant = isConstant;
          if (!di.divergent.has_value()) {
            // TODO: This would be entirely unnecessary if we had updatable divergence info.
            di.divergent = !isConstant;
          }
        }
      }

      if (!di.invariant.value_or(true) || di.divergent.value_or(false))
        propagationWorklist.push_back(current);
    }

    if (searchWorklist.empty())
      break;

    current = searchWorklist.pop_back_val();
  }
  // Fixed-point iteration to propagate "variant" and "divergent" flags.
  while (!propagationWorklist.empty()) {
    current = propagationWorklist.pop_back_val();
    auto diIt = m_descriptors.find(current);
    assert(diIt != m_descriptors.end());
    DescriptorInfo di = diIt->second;

    for (User *user : current->users()) {
      // Make a reasonable effort not to "leak" into instructions we don't understand (e.g., if a pointer / descriptor
      // ended up in an aggregate). Some of these cases could perhaps be handled in a conservative way, but it seems
      // unlikely to be necessary in practice.
      if (!isa<PHINode>(user) && !isa<SelectInst>(user) && !seen.count(user))
        continue;

      auto &userDi = m_descriptors[user];
      bool propagate = false;
      if (!userDi.invariant.has_value() && !di.invariant.value_or(true)) {
        userDi.invariant = false;
        propagate = true;
      }
      if (!userDi.divergent.has_value() && di.divergent.value_or(false)) {
        userDi.divergent = true;
        propagate = true;
      }
      if (propagate)
        propagationWorklist.push_back(user);
    }
  }

  // At this point, seen values that are not "variant"/"divergent" are known to be "invariant"/"uniform".
  for (Value *current : seen) {
    auto &di = m_descriptors[current];
    if (!di.invariant.has_value())
      di.invariant = true;
    if (!di.divergent.has_value())
      di.divergent = false;
  }

  return m_descriptors.find(desc)->second;
}

// =====================================================================================================================
// Visits "cmpxchg" instruction.
//
// @param atomicCmpXchgInst : The instruction
void BufferOpLowering::visitAtomicCmpXchgInst(AtomicCmpXchgInst &atomicCmpXchgInst) {
  // If the type we are doing an atomic operation on is not a fat pointer, bail.
  if (atomicCmpXchgInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder.SetInsertPoint(&atomicCmpXchgInst);

  auto values = m_typeLowering.getValue(atomicCmpXchgInst.getPointerOperand());

  Type *const storeType = atomicCmpXchgInst.getNewValOperand()->getType();

  const bool isNonTemporal = atomicCmpXchgInst.getMetadata(LLVMContext::MD_nontemporal);

  Value *const bufferDesc = values[0];
  Value *const baseIndex = m_builder.CreatePtrToInt(values[1], m_builder.getInt32Ty());
  copyMetadata(baseIndex, &atomicCmpXchgInst);

  // If our buffer descriptor is divergent, need to handle it differently.
  if (getDescriptorInfo(bufferDesc).divergent.value()) {
    Value *const baseAddr = getBaseAddressFromBufferDesc(bufferDesc);

    // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
    Value *const bound = m_builder.CreateExtractElement(bufferDesc, 2);
    Value *const inBound = m_builder.CreateICmpULT(baseIndex, bound);
    Value *const newBaseIndex = m_builder.CreateSelect(inBound, baseIndex, m_builder.getInt32(0));

    // Add on the index to the address.
    Value *atomicPointer = m_builder.CreateGEP(m_builder.getInt8Ty(), baseAddr, newBaseIndex);

    atomicPointer = m_builder.CreateBitCast(atomicPointer, storeType->getPointerTo(ADDR_SPACE_GLOBAL));

    const AtomicOrdering successOrdering = atomicCmpXchgInst.getSuccessOrdering();
    const AtomicOrdering failureOrdering = atomicCmpXchgInst.getFailureOrdering();

    Value *const compareValue = atomicCmpXchgInst.getCompareOperand();
    Value *const newValue = atomicCmpXchgInst.getNewValOperand();
    AtomicCmpXchgInst *const newAtomicCmpXchg = m_builder.CreateAtomicCmpXchg(
        atomicPointer, compareValue, newValue, MaybeAlign(), successOrdering, failureOrdering);
    newAtomicCmpXchg->setVolatile(atomicCmpXchgInst.isVolatile());
    newAtomicCmpXchg->setSyncScopeID(atomicCmpXchgInst.getSyncScopeID());
    newAtomicCmpXchg->setWeak(atomicCmpXchgInst.isWeak());
    copyMetadata(newAtomicCmpXchg, &atomicCmpXchgInst);

    // Record the atomic instruction so we remember to delete it later.
    m_typeLowering.eraseInstruction(&atomicCmpXchgInst);

    atomicCmpXchgInst.replaceAllUsesWith(newAtomicCmpXchg);
  } else {
    switch (atomicCmpXchgInst.getSuccessOrdering()) {
    case AtomicOrdering::Release:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent: {
      FenceInst *const fence = m_builder.CreateFence(AtomicOrdering::Release, atomicCmpXchgInst.getSyncScopeID());
      copyMetadata(fence, &atomicCmpXchgInst);
      break;
    }
    default: {
      break;
    }
    }

    CoherentFlag coherent = {};
    if (m_pipelineState.getTargetInfo().getGfxIpVersion().major <= 11)
      coherent.bits.slc = isNonTemporal ? 1 : 0;

    Value *const atomicCall = m_builder.CreateIntrinsic(
        Intrinsic::amdgcn_raw_buffer_atomic_cmpswap, atomicCmpXchgInst.getNewValOperand()->getType(),
        {atomicCmpXchgInst.getNewValOperand(), atomicCmpXchgInst.getCompareOperand(), bufferDesc, baseIndex,
         m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});

    switch (atomicCmpXchgInst.getSuccessOrdering()) {
    case AtomicOrdering::Acquire:
    case AtomicOrdering::AcquireRelease:
    case AtomicOrdering::SequentiallyConsistent: {
      FenceInst *const fence = m_builder.CreateFence(AtomicOrdering::Acquire, atomicCmpXchgInst.getSyncScopeID());
      copyMetadata(fence, &atomicCmpXchgInst);
      break;
    }
    default: {
      break;
    }
    }

    Value *resultValue = PoisonValue::get(atomicCmpXchgInst.getType());

    resultValue = m_builder.CreateInsertValue(resultValue, atomicCall, static_cast<uint64_t>(0));
    copyMetadata(resultValue, &atomicCmpXchgInst);

    // NOTE: If we have a strong compare exchange, LLVM optimization will always set the compare result to "Equal".
    // Thus, we have to correct this behaviour and do the comparison by ourselves.
    if (!atomicCmpXchgInst.isWeak()) {
      Value *const valueEqual = m_builder.CreateICmpEQ(atomicCall, atomicCmpXchgInst.getCompareOperand());
      copyMetadata(valueEqual, &atomicCmpXchgInst);

      resultValue = m_builder.CreateInsertValue(resultValue, valueEqual, static_cast<uint64_t>(1));
      copyMetadata(resultValue, &atomicCmpXchgInst);
    }

    // Record the atomic instruction so we remember to delete it later.
    m_typeLowering.eraseInstruction(&atomicCmpXchgInst);

    atomicCmpXchgInst.replaceAllUsesWith(resultValue);
  }
}

// =====================================================================================================================
// Visits "atomicrmw" instruction.
//
// @param atomicRmwInst : The instruction
void BufferOpLowering::visitAtomicRMWInst(AtomicRMWInst &atomicRmwInst) {
  if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
    m_builder.SetInsertPoint(&atomicRmwInst);

    auto values = m_typeLowering.getValue(atomicRmwInst.getPointerOperand());

    Type *const storeType = atomicRmwInst.getValOperand()->getType();

    const bool isNonTemporal = atomicRmwInst.getMetadata(LLVMContext::MD_nontemporal);

    Value *const bufferDesc = values[0];
    Value *const baseIndex = m_builder.CreatePtrToInt(values[1], m_builder.getInt32Ty());
    copyMetadata(baseIndex, &atomicRmwInst);

    // If our buffer descriptor is divergent, need to handle it differently.
    if (getDescriptorInfo(bufferDesc).divergent.value()) {
      Value *const baseAddr = getBaseAddressFromBufferDesc(bufferDesc);

      // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
      Value *const bound = m_builder.CreateExtractElement(bufferDesc, 2);
      Value *const inBound = m_builder.CreateICmpULT(baseIndex, bound);
      Value *const newBaseIndex = m_builder.CreateSelect(inBound, baseIndex, m_builder.getInt32(0));

      // Add on the index to the address.
      Value *atomicPointer = m_builder.CreateGEP(m_builder.getInt8Ty(), baseAddr, newBaseIndex);

      atomicPointer = m_builder.CreateBitCast(atomicPointer, storeType->getPointerTo(ADDR_SPACE_GLOBAL));

      AtomicRMWInst *const newAtomicRmw =
          m_builder.CreateAtomicRMW(atomicRmwInst.getOperation(), atomicPointer, atomicRmwInst.getValOperand(),
                                    atomicRmwInst.getAlign(), atomicRmwInst.getOrdering());
      newAtomicRmw->setVolatile(atomicRmwInst.isVolatile());
      newAtomicRmw->setSyncScopeID(atomicRmwInst.getSyncScopeID());
      copyMetadata(newAtomicRmw, &atomicRmwInst);

      // Record the atomic instruction so we remember to delete it later.
      m_typeLowering.eraseInstruction(&atomicRmwInst);

      atomicRmwInst.replaceAllUsesWith(newAtomicRmw);
    } else {
      switch (atomicRmwInst.getOrdering()) {
      case AtomicOrdering::Release:
      case AtomicOrdering::AcquireRelease:
      case AtomicOrdering::SequentiallyConsistent: {
        FenceInst *const fence = m_builder.CreateFence(AtomicOrdering::Release, atomicRmwInst.getSyncScopeID());
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

      CoherentFlag coherent = {};
      if (m_pipelineState.getTargetInfo().getGfxIpVersion().major <= 11) {
        coherent.bits.slc = isNonTemporal ? 1 : 0;
      }

      Value *const atomicCall = m_builder.CreateIntrinsic(intrinsic, storeType,
                                                          {atomicRmwInst.getValOperand(), bufferDesc, baseIndex,
                                                           m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
      copyMetadata(atomicCall, &atomicRmwInst);

      switch (atomicRmwInst.getOrdering()) {
      case AtomicOrdering::Acquire:
      case AtomicOrdering::AcquireRelease:
      case AtomicOrdering::SequentiallyConsistent: {
        FenceInst *const fence = m_builder.CreateFence(AtomicOrdering::Acquire, atomicRmwInst.getSyncScopeID());
        copyMetadata(fence, &atomicRmwInst);
        break;
      }
      default: {
        break;
      }
      }

      // Record the atomic instruction so we remember to delete it later.
      m_typeLowering.eraseInstruction(&atomicRmwInst);

      atomicRmwInst.replaceAllUsesWith(atomicCall);
    }
  } else if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_GLOBAL) {
    AtomicRMWInst::BinOp op = atomicRmwInst.getOperation();
    Type *const storeType = atomicRmwInst.getValOperand()->getType();
    if (op == AtomicRMWInst::FMin || op == AtomicRMWInst::FMax || op == AtomicRMWInst::FAdd) {
      Value *const pointer = atomicRmwInst.getPointerOperand();
      m_builder.SetInsertPoint(&atomicRmwInst);
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
      Value *const atomicCall = m_builder.CreateIntrinsic(intrinsic, {storeType, pointer->getType(), storeType},
                                                          {pointer, atomicRmwInst.getValOperand()});
      copyMetadata(atomicCall, &atomicRmwInst);
      // Record the atomic instruction so we remember to delete it later.
      m_typeLowering.eraseInstruction(&atomicRmwInst);

      atomicRmwInst.replaceAllUsesWith(atomicCall);
    }
  } else if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_LOCAL) {
    AtomicRMWInst::BinOp op = atomicRmwInst.getOperation();
    Type *const storeType = atomicRmwInst.getValOperand()->getType();
    if (op == AtomicRMWInst::FMin || op == AtomicRMWInst::FMax || op == AtomicRMWInst::FAdd) {
      Value *const pointer = atomicRmwInst.getPointerOperand();
      m_builder.SetInsertPoint(&atomicRmwInst);
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

      Value *const atomicCall = m_builder.CreateIntrinsic(
          intrinsic, {storeType},
          {pointer, atomicRmwInst.getValOperand(),
           m_builder.getInt32(static_cast<uint32_t>(atomicRmwInst.getOrdering())),
           m_builder.getInt32(atomicRmwInst.getSyncScopeID()), m_builder.getInt1(atomicRmwInst.isVolatile())});
      copyMetadata(atomicCall, &atomicRmwInst);
      // Record the atomic instruction so we remember to delete it later.
      m_typeLowering.eraseInstruction(&atomicRmwInst);
      atomicRmwInst.replaceAllUsesWith(atomicCall);
    }
  }
}

// =====================================================================================================================
// Visits "bitcast" instruction.
//
// @param bitCastInst : The instruction
void BufferOpLowering::visitBitCastInst(BitCastInst &bitCastInst) {
  Type *const destType = bitCastInst.getType();

  // If the type is not a pointer type, bail.
  if (!destType->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (destType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_typeLowering.replaceInstruction(&bitCastInst, m_typeLowering.getValue(bitCastInst.getOperand(0)));
}

// =====================================================================================================================
// Visits "buffer.desc.to.ptr" instruction.
//
// @param descToPtr : The instruction
void BufferOpLowering::visitBufferDescToPtr(BufferDescToPtrOp &descToPtr) {
  m_builder.SetInsertPoint(&descToPtr);

  Constant *const nullPointer = ConstantPointerNull::get(m_offsetType);
  m_typeLowering.replaceInstruction(&descToPtr, {descToPtr.getDesc(), nullPointer});

  auto &di = m_descriptors[descToPtr.getDesc()];

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  di.divergent = m_uniformityInfo.isDivergent(*descToPtr.getDesc());
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  di.divergent = m_uniformityInfo.isDivergent(descToPtr.getDesc());
#endif
}

// =====================================================================================================================
// Visits "buffer.length" instruction.
//
// @param length : The instruction
void BufferOpLowering::visitBufferLength(BufferLengthOp &length) {
  m_builder.SetInsertPoint(&length);

  auto values = m_typeLowering.getValue(length.getPointer());

  // Extract element 2 which is the NUM_RECORDS field from the buffer descriptor.
  Value *const bufferDesc = values[0];
  Value *numRecords = m_builder.CreateExtractElement(bufferDesc, 2);
  Value *offset = length.getOffset();

  // If null descriptors are allowed, we must guarantee a 0 result for a null buffer descriptor.
  //
  // What we implement here is in fact more robust: ensure that the subtraction of the offset is clamped to 0.
  // The backend should be able to achieve this with a single additional ALU instruction (e.g. s_max_u32).
  if (m_pipelineState.getOptions().allowNullDescriptor) {
    Value *const underflow = m_builder.CreateICmpUGT(offset, numRecords);
    numRecords = m_builder.CreateSelect(underflow, offset, numRecords);
  }

  numRecords = m_builder.CreateSub(numRecords, offset);

  // Record the call instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&length);

  length.replaceAllUsesWith(numRecords);
}

// =====================================================================================================================
// Visits "buffer.ptr.diff" instruction.
//
// @param ptrDiff : The instruction
void BufferOpLowering::visitBufferPtrDiff(BufferPtrDiffOp &ptrDiff) {
  m_builder.SetInsertPoint(&ptrDiff);

  Value *const lhs = ptrDiff.getLhs();
  Value *const rhs = ptrDiff.getRhs();

  Value *const lhsPtrToInt = m_builder.CreatePtrToInt(m_typeLowering.getValue(lhs)[1], m_builder.getInt32Ty());
  Value *const rhsPtrToInt = m_builder.CreatePtrToInt(m_typeLowering.getValue(rhs)[1], m_builder.getInt32Ty());

  copyMetadata(lhsPtrToInt, lhs);
  copyMetadata(rhsPtrToInt, rhs);

  Value *difference = m_builder.CreateSub(lhsPtrToInt, rhsPtrToInt);
  difference = m_builder.CreateSExt(difference, m_builder.getInt64Ty());

  // Record the call instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&ptrDiff);

  ptrDiff.replaceAllUsesWith(difference);
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtrInst : The instruction
void BufferOpLowering::visitGetElementPtrInst(GetElementPtrInst &getElemPtrInst) {
  // If the type we are GEPing into is not a fat pointer, bail.
  if (getElemPtrInst.getAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder.SetInsertPoint(&getElemPtrInst);

  auto values = m_typeLowering.getValue(getElemPtrInst.getPointerOperand());

  SmallVector<Value *, 8> indices(getElemPtrInst.idx_begin(), getElemPtrInst.idx_end());

  Value *newGetElemPtr = nullptr;
  auto getElemPtrPtr = values[1];
  auto getElemPtrEltTy = getElemPtrInst.getSourceElementType();

  if (getElemPtrInst.isInBounds())
    newGetElemPtr = m_builder.CreateInBoundsGEP(getElemPtrEltTy, getElemPtrPtr, indices);
  else
    newGetElemPtr = m_builder.CreateGEP(getElemPtrEltTy, getElemPtrPtr, indices);

  copyMetadata(newGetElemPtr, &getElemPtrInst);

  m_typeLowering.replaceInstruction(&getElemPtrInst, {values[0], newGetElemPtr});
}

// =====================================================================================================================
// Visits "load" instruction.
//
// @param loadInst : The instruction
void BufferOpLowering::visitLoadInst(LoadInst &loadInst) {
  const unsigned addrSpace = loadInst.getPointerAddressSpace();

  if (addrSpace != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_postVisitInsts.push_back(&loadInst);
}

// =====================================================================================================================
// Visits "load" instruction after the initial pass, when phi nodes have been fixed up and potentially simplified.
//
// @param loadInst : the instruction
void BufferOpLowering::postVisitLoadInst(LoadInst &loadInst) {
  Value *const newLoad = replaceLoadStore(loadInst);

  // Record the load instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&loadInst);

  loadInst.replaceAllUsesWith(newLoad);
}

// =====================================================================================================================
// Visits "memcpy" instruction.
//
// @param memCpyInst : The memcpy instruction
void BufferOpLowering::visitMemCpyInst(MemCpyInst &memCpyInst) {
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
void BufferOpLowering::visitMemMoveInst(MemMoveInst &memMoveInst) {
  Value *const dest = memMoveInst.getArgOperand(0);
  Value *const src = memMoveInst.getArgOperand(1);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();
  const unsigned srcAddrSpace = src->getType()->getPointerAddressSpace();

  // If either of the address spaces are not fat pointers, bail.
  if (destAddrSpace != ADDR_SPACE_BUFFER_FAT_POINTER && srcAddrSpace != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder.SetInsertPoint(&memMoveInst);

  const MaybeAlign destAlignment = memMoveInst.getParamAlign(0);
  const MaybeAlign srcAlignment = memMoveInst.getParamAlign(1);

  // We assume LLVM is not introducing variable length mem moves.
  ConstantInt *const length = cast<ConstantInt>(memMoveInst.getArgOperand(2));

  // Get a vector type that is the length of the memmove.
  VectorType *const memoryType = FixedVectorType::get(m_builder.getInt8Ty(), length->getZExtValue());

  LoadInst *const srcLoad = m_builder.CreateAlignedLoad(memoryType, src, srcAlignment);
  copyMetadata(srcLoad, &memMoveInst);

  StoreInst *const destStore = m_builder.CreateAlignedStore(srcLoad, dest, destAlignment);
  copyMetadata(destStore, &memMoveInst);

  // Record the memmove instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&memMoveInst);

  // Visit the load and store instructions to fold away fat pointer load/stores we might have just created.
  visitLoadInst(*srcLoad);
  visitStoreInst(*destStore);
}

// =====================================================================================================================
// Visits "memset" instruction.
//
// @param memSetInst : The memset instruction
void BufferOpLowering::visitMemSetInst(MemSetInst &memSetInst) {
  Value *const dest = memSetInst.getArgOperand(0);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();

  // If the address spaces is a fat pointer.
  if (destAddrSpace == ADDR_SPACE_BUFFER_FAT_POINTER) {
    // Handling memset requires us to modify the CFG, so we need to do it after the initial visit pass.
    m_postVisitInsts.push_back(&memSetInst);
  }
}

// =====================================================================================================================
// Visits phi node.
//
// The bulk of phi handling is done by TypeLowering. We just note divergent phi nodes here to handle sync divergence
// (i.e., phi nodes that are divergent due to divergent control flow).
//
// We do this because:
//
//  - phi nodes of fat pointers are very often divergent, but the descriptor part is actually uniform; only the offset
//    part that is divergent. So we do our own mini-divergence analysis on the descriptor values after the first visitor
//    pass.
//  - TypeLowering helps us by automatically eliminating descriptor phi nodes in typical cases where they're redundant.
//
// @param phi : The instruction
void BufferOpLowering::visitPhiInst(llvm::PHINode &phi) {
  if (!phi.getType()->isPointerTy() || phi.getType()->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  if (m_uniformityInfo.isDivergent(phi))
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  if (m_uniformityInfo.isDivergent(&phi))
#endif
    m_divergentPhis.push_back(&phi);
}

// =====================================================================================================================
// Visits "store" instruction.
//
// @param storeInst : The instruction
void BufferOpLowering::visitStoreInst(StoreInst &storeInst) {
  // If the address space of the store pointer is not a buffer fat pointer, bail.
  if (storeInst.getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_postVisitInsts.push_back(&storeInst);
}

// =====================================================================================================================
// Visits "store" instruction after the initial pass, when phi nodes have been fixed up and potentially simplified.
//
// @param storeInst : the instruction
void BufferOpLowering::postVisitStoreInst(StoreInst &storeInst) {
  // TODO: Unify with loads?
  replaceLoadStore(storeInst);

  // Record the store instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&storeInst);
}

// =====================================================================================================================
// Visits "icmp" instruction.
//
// @param icmpInst : The instruction
void BufferOpLowering::visitICmpInst(ICmpInst &icmpInst) {
  Type *const type = icmpInst.getOperand(0)->getType();

  // If the type is not a pointer type, bail.
  if (!type->isPointerTy())
    return;

  // If the pointer is not a fat pointer, bail.
  if (type->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  m_builder.SetInsertPoint(&icmpInst);

  SmallVector<Value *, 2> bufferDescs;
  SmallVector<Value *, 2> indices;
  for (int i = 0; i < 2; ++i) {
    auto values = m_typeLowering.getValue(icmpInst.getOperand(i));
    bufferDescs.push_back(values[0]);
    indices.push_back(m_builder.CreatePtrToInt(values[1], m_builder.getInt32Ty()));
  }

  assert(icmpInst.getPredicate() == ICmpInst::ICMP_EQ || icmpInst.getPredicate() == ICmpInst::ICMP_NE);

  Value *const bufferDescEqual = m_builder.CreateICmpEQ(bufferDescs[0], bufferDescs[1]);

  Value *bufferDescICmp = m_builder.CreateExtractElement(bufferDescEqual, static_cast<uint64_t>(0));
  for (unsigned i = 1; i < 4; ++i) {
    Value *bufferDescElemEqual = m_builder.CreateExtractElement(bufferDescEqual, i);
    bufferDescICmp = m_builder.CreateAnd(bufferDescICmp, bufferDescElemEqual);
  }

  Value *indexICmp = m_builder.CreateICmpEQ(indices[0], indices[1]);

  Value *newICmp = m_builder.CreateAnd(bufferDescICmp, indexICmp);

  if (icmpInst.getPredicate() == ICmpInst::ICMP_NE)
    newICmp = m_builder.CreateNot(newICmp);

  copyMetadata(newICmp, &icmpInst);

  // Record the icmp instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&icmpInst);

  icmpInst.replaceAllUsesWith(newICmp);
}

// =====================================================================================================================
// Visits invariant start intrinsic.
//
// @param intrinsic : The intrinsic
void BufferOpLowering::visitInvariantStart(llvm::IntrinsicInst &intrinsic) {
  Value *ptr = intrinsic.getArgOperand(1);
  if (ptr->getType()->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
    return;

  auto values = m_typeLowering.getValue(ptr);
  Value *desc = values[0];

  m_descriptors[desc].invariant = true;

  m_typeLowering.eraseInstruction(&intrinsic);
}

// =====================================================================================================================
// Post-process visits "memcpy" instruction.
//
// @param memCpyInst : The memcpy instruction
void BufferOpLowering::postVisitMemCpyInst(MemCpyInst &memCpyInst) {
  Value *const dest = memCpyInst.getArgOperand(0);
  Value *const src = memCpyInst.getArgOperand(1);

  m_builder.SetInsertPoint(&memCpyInst);

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
      memoryType = FixedVectorType::get(m_builder.getInt32Ty(), 4);
    } else {
      assert(stride <= 8);
      memoryType = m_builder.getIntNTy(stride * 8);
    }

    Value *length = memCpyInst.getArgOperand(2);

    Type *const lengthType = length->getType();

    Value *const index =
        makeLoop(ConstantInt::get(lengthType, 0), length, ConstantInt::get(lengthType, stride), &memCpyInst);

    // Get the current index into our source pointer.
    Value *const srcPtr = m_builder.CreateGEP(m_builder.getInt8Ty(), src, index);
    copyMetadata(srcPtr, &memCpyInst);

    // Perform a load for the value.
    LoadInst *const srcLoad = m_builder.CreateLoad(memoryType, srcPtr);
    copyMetadata(srcLoad, &memCpyInst);

    // Get the current index into our destination pointer.
    Value *const destPtr = m_builder.CreateGEP(m_builder.getInt8Ty(), dest, index);
    copyMetadata(destPtr, &memCpyInst);

    // And perform a store for the value at this byte.
    StoreInst *const destStore = m_builder.CreateStore(srcLoad, destPtr);
    copyMetadata(destStore, &memCpyInst);

    // Visit the newly added instructions to turn them into fat pointer variants.
    if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(srcPtr))
      visitGetElementPtrInst(*getElemPtr);

    if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(destPtr))
      visitGetElementPtrInst(*getElemPtr);

    if (srcPtr->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
      postVisitLoadInst(*srcLoad);
    if (destPtr->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
      postVisitStoreInst(*destStore);
  } else {
    // Get an vector type that is the length of the memcpy.
    VectorType *const memoryType = FixedVectorType::get(m_builder.getInt8Ty(), lengthConstant->getZExtValue());

    LoadInst *const srcLoad = m_builder.CreateAlignedLoad(memoryType, src, srcAlignment);
    copyMetadata(srcLoad, &memCpyInst);

    StoreInst *const destStore = m_builder.CreateAlignedStore(srcLoad, dest, destAlignment);
    copyMetadata(destStore, &memCpyInst);

    if (src->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
      postVisitLoadInst(*srcLoad);
    if (dest->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
      postVisitStoreInst(*destStore);
  }

  // Record the memcpy instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&memCpyInst);
}

// =====================================================================================================================
// Post-process visits "memset" instruction.
//
// @param memSetInst : The memset instruction
void BufferOpLowering::postVisitMemSetInst(MemSetInst &memSetInst) {
  Value *const dest = memSetInst.getArgOperand(0);

  const unsigned destAddrSpace = dest->getType()->getPointerAddressSpace();

  m_builder.SetInsertPoint(&memSetInst);

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
      castDestType = FixedVectorType::get(m_builder.getInt32Ty(), 4);
    else {
      assert(stride <= 8);
      castDestType = m_builder.getIntNTy(stride * 8);
    }

    Value *newValue = nullptr;

    if (Constant *const constVal = dyn_cast<Constant>(value)) {
      newValue = ConstantVector::getSplat(ElementCount::get(stride, false), constVal);
      newValue = m_builder.CreateBitCast(newValue, castDestType);
      copyMetadata(newValue, &memSetInst);
    } else {
      Value *const memoryPointer = m_builder.CreateAlloca(castDestType);
      copyMetadata(memoryPointer, &memSetInst);

      Value *const memSet = m_builder.CreateMemSet(memoryPointer, value, stride, Align());
      copyMetadata(memSet, &memSetInst);

      newValue = m_builder.CreateLoad(castDestType, memoryPointer);
      copyMetadata(newValue, &memSetInst);
    }

    Value *const length = memSetInst.getArgOperand(2);

    Type *const lengthType = length->getType();

    Value *const index =
        makeLoop(ConstantInt::get(lengthType, 0), length, ConstantInt::get(lengthType, stride), &memSetInst);

    // Get the current index into our destination pointer.
    Value *const destPtr = m_builder.CreateGEP(m_builder.getInt8Ty(), dest, index);
    copyMetadata(destPtr, &memSetInst);

    Value *const castDest = m_builder.CreateBitCast(destPtr, castDestType->getPointerTo(destAddrSpace));
    copyMetadata(castDest, &memSetInst);

    // And perform a store for the value at this byte.
    StoreInst *const destStore = m_builder.CreateStore(newValue, destPtr);
    copyMetadata(destStore, &memSetInst);

    if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(destPtr))
      visitGetElementPtrInst(*getElemPtr);

    postVisitStoreInst(*destStore);
  } else {
    // Get a vector type that is the length of the memset.
    VectorType *const memoryType = FixedVectorType::get(m_builder.getInt8Ty(), lengthConstant->getZExtValue());

    Value *newValue = nullptr;

    if (Constant *const constVal = dyn_cast<Constant>(value))
      newValue = ConstantVector::getSplat(cast<VectorType>(memoryType)->getElementCount(), constVal);
    else {
      Value *const memoryPointer = m_builder.CreateAlloca(memoryType);
      copyMetadata(memoryPointer, &memSetInst);

      Type *const int8PtrTy = m_builder.getInt8Ty()->getPointerTo(ADDR_SPACE_PRIVATE);
      Value *const castMemoryPointer = m_builder.CreateBitCast(memoryPointer, int8PtrTy);
      copyMetadata(castMemoryPointer, &memSetInst);

      Value *const memSet = m_builder.CreateMemSet(castMemoryPointer, value,
                                                   cast<FixedVectorType>(memoryType)->getNumElements(), Align());
      copyMetadata(memSet, &memSetInst);

      newValue = m_builder.CreateLoad(memoryType, memoryPointer);
      copyMetadata(newValue, &memSetInst);
    }

    StoreInst *const destStore = m_builder.CreateAlignedStore(newValue, dest, destAlignment);
    copyMetadata(destStore, &memSetInst);
    postVisitStoreInst(*destStore);
  }

  // Record the memset instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&memSetInst);
}

// =====================================================================================================================
// Extract the 64-bit address from a buffer descriptor.
//
// @param bufferDesc : The buffer descriptor to extract the address from
Value *BufferOpLowering::getBaseAddressFromBufferDesc(Value *const bufferDesc) {
  Type *const descType = bufferDesc->getType();

  // Get the base address of our buffer by extracting the two components with the 48-bit address, and masking.
  Value *baseAddr = m_builder.CreateShuffleVector(bufferDesc, PoisonValue::get(descType), ArrayRef<int>{0, 1});
  Value *const baseAddrMask = ConstantVector::get({m_builder.getInt32(0xFFFFFFFF), m_builder.getInt32(0xFFFF)});
  baseAddr = m_builder.CreateAnd(baseAddr, baseAddrMask);
  baseAddr = m_builder.CreateBitCast(baseAddr, m_builder.getInt64Ty());
  return m_builder.CreateIntToPtr(baseAddr, m_builder.getInt8Ty()->getPointerTo(ADDR_SPACE_GLOBAL));
}

// =====================================================================================================================
// Copy all metadata from one value to another.
//
// @param [in/out] dest : The destination to copy metadata onto.
// @param src : The source to copy metadata from.
void BufferOpLowering::copyMetadata(Value *const dest, const Value *const src) const {
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
// Replace a fat pointer load or store with the required intrinsics.
//
// @param inst : The instruction to replace.
Value *BufferOpLowering::replaceLoadStore(Instruction &inst) {
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

  m_builder.SetInsertPoint(&inst);

  auto pointerValues = m_typeLowering.getValue(pointerOperand);
  Value *const bufferDesc = pointerValues[0];

  const DataLayout &dataLayout = m_builder.GetInsertBlock()->getModule()->getDataLayout();

  const unsigned bytesToHandle = static_cast<unsigned>(dataLayout.getTypeStoreSize(type));

  bool isInvariant = false;
  if (isLoad) {
    isInvariant =
        getDescriptorInfo(bufferDesc).invariant.value() || loadInst->getMetadata(LLVMContext::MD_invariant_load);
  }

  const bool isNonTemporal = inst.getMetadata(LLVMContext::MD_nontemporal);
  const bool isGlc = ordering != AtomicOrdering::NotAtomic;
  const bool isDlc = isGlc; // For buffer load on GFX10+, we set DLC = GLC

  Value *const baseIndex = m_builder.CreatePtrToInt(pointerValues[1], m_builder.getInt32Ty());

  // If our buffer descriptor is divergent, need to handle that differently.
  if (getDescriptorInfo(bufferDesc).divergent.value()) {
    Value *const baseAddr = getBaseAddressFromBufferDesc(bufferDesc);

    // The 2nd element in the buffer descriptor is the byte bound, we do this to support robust buffer access.
    Value *const bound = m_builder.CreateExtractElement(bufferDesc, 2);
    Value *const inBound = m_builder.CreateICmpULT(baseIndex, bound);
    Value *const newBaseIndex = m_builder.CreateSelect(inBound, baseIndex, m_builder.getInt32(0));

    // Add on the index to the address.
    Value *pointer = m_builder.CreateGEP(m_builder.getInt8Ty(), baseAddr, newBaseIndex);

    pointer = m_builder.CreateBitCast(pointer, type->getPointerTo(ADDR_SPACE_GLOBAL));

    if (isLoad) {
      LoadInst *const newLoad = m_builder.CreateAlignedLoad(type, pointer, alignment, loadInst->isVolatile());
      newLoad->setOrdering(ordering);
      newLoad->setSyncScopeID(syncScopeID);
      copyMetadata(newLoad, loadInst);

      if (isInvariant)
        newLoad->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));

      return newLoad;
    }
    StoreInst *const newStore =
        m_builder.CreateAlignedStore(storeInst->getValueOperand(), pointer, alignment, storeInst->isVolatile());
    newStore->setOrdering(ordering);
    newStore->setSyncScopeID(syncScopeID);
    copyMetadata(newStore, storeInst);

    return newStore;
  }

  switch (ordering) {
  case AtomicOrdering::Release:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    m_builder.CreateFence(AtomicOrdering::Release, syncScopeID);
    break;
  default:
    break;
  }

  SmallVector<Value *, 8> parts;
  Type *smallestType = nullptr;
  unsigned smallestByteSize = 4;

  if (alignment < 2 || (bytesToHandle & 0x1) != 0) {
    smallestByteSize = 1;
    smallestType = m_builder.getInt8Ty();
  } else if (alignment < 4 || (bytesToHandle & 0x3) != 0) {
    smallestByteSize = 2;
    smallestType = m_builder.getInt16Ty();
  } else {
    smallestByteSize = 4;
    smallestType = m_builder.getInt32Ty();
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
      Value *castValue = PoisonValue::get(castType);
      for (unsigned elemIdx = 0; elemIdx != elemCount; ++elemIdx) {
        Value *elem = m_builder.CreateExtractValue(storeValue, elemIdx);
        elem = m_builder.CreateBitCast(elem, smallestType);
        castValue = m_builder.CreateInsertElement(castValue, elem, elemIdx);
      }
      storeValue = castValue;
      copyMetadata(storeValue, storeInst);
    } else {
      if (storeTy->isPointerTy()) {
        storeValue = m_builder.CreatePtrToInt(storeValue, m_builder.getIntNTy(bytesToHandle * 8));
        copyMetadata(storeValue, storeInst);
      }

      storeValue = m_builder.CreateBitCast(storeValue, castType);
      copyMetadata(storeValue, storeInst);
    }
  }

  // The index in storeValue which we use next
  unsigned storeIndex = 0;

  unsigned remainingBytes = bytesToHandle;
  while (remainingBytes > 0) {
    const unsigned offset = bytesToHandle - remainingBytes;
    Value *offsetVal = offset == 0 ? baseIndex : m_builder.CreateAdd(baseIndex, m_builder.getInt32(offset));

    Type *intAccessType = nullptr;
    unsigned accessSize = 0;

    // Handle the greatest possible size
    if (alignment >= 4 && remainingBytes >= 4) {
      if (remainingBytes >= 16) {
        intAccessType = FixedVectorType::get(m_builder.getInt32Ty(), 4);
        accessSize = 16;
      } else if (remainingBytes >= 12 && !isInvariant) {
        intAccessType = FixedVectorType::get(m_builder.getInt32Ty(), 3);
        accessSize = 12;
      } else if (remainingBytes >= 8) {
        intAccessType = FixedVectorType::get(m_builder.getInt32Ty(), 2);
        accessSize = 8;
      } else {
        // remainingBytes >= 4
        intAccessType = m_builder.getInt32Ty();
        accessSize = 4;
      }
    } else if (alignment >= 2 && remainingBytes >= 2) {
      intAccessType = m_builder.getInt16Ty();
      accessSize = 2;
    } else {
      intAccessType = m_builder.getInt8Ty();
      accessSize = 1;
    }
    assert(intAccessType);
    assert(accessSize != 0);

    Value *part = nullptr;

    CoherentFlag coherent = {};
    if (m_pipelineState.getTargetInfo().getGfxIpVersion().major <= 11) {
      coherent.bits.glc = isGlc;
      if (!isInvariant)
        coherent.bits.slc = isNonTemporal;
    }

    if (isLoad) {
      if (m_pipelineState.getTargetInfo().getGfxIpVersion().major >= 10 &&
          m_pipelineState.getTargetInfo().getGfxIpVersion().major <= 11) {
        // TODO For stores?
        coherent.bits.dlc = isDlc;
      }
      if (isInvariant && accessSize >= 4) {
        CallInst *call = m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_buffer_load, intAccessType,
                                                   {bufferDesc, offsetVal, m_builder.getInt32(coherent.u32All)});
        call->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));
        part = call;
      } else {
        unsigned intrinsicID = Intrinsic::amdgcn_raw_buffer_load;
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Atomic load loses memory semantics
#else
        if (ordering != AtomicOrdering::NotAtomic)
          intrinsicID = Intrinsic::amdgcn_raw_atomic_buffer_load;
#endif
        part = m_builder.CreateIntrinsic(
            intrinsicID, intAccessType,
            {bufferDesc, offsetVal, m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
      }
    } else {
      // Store
      unsigned compCount = accessSize / smallestByteSize;
      part = PoisonValue::get(FixedVectorType::get(smallestType, compCount));

      for (unsigned i = 0; i < compCount; i++) {
        Value *const storeElem = m_builder.CreateExtractElement(storeValue, storeIndex++);
        part = m_builder.CreateInsertElement(part, storeElem, i);
      }
      part = m_builder.CreateBitCast(part, intAccessType);
      copyMetadata(part, &inst);
      part = m_builder.CreateIntrinsic(
          Intrinsic::amdgcn_raw_buffer_store, intAccessType,
          {part, bufferDesc, offsetVal, m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
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
      newInst = PoisonValue::get(FixedVectorType::get(smallestType, bytesToHandle / smallestByteSize));

      unsigned index = 0;

      for (Value *part : parts) {
        // Get the byte size of our load part.
        const unsigned byteSize = static_cast<unsigned>(dataLayout.getTypeStoreSize(part->getType()));

        // Bitcast it to a vector of the smallest load type.
        FixedVectorType *const castType = FixedVectorType::get(smallestType, byteSize / smallestByteSize);
        part = m_builder.CreateBitCast(part, castType);
        copyMetadata(part, &inst);

        // Run through the elements of our bitcasted type and insert them into the main load.
        for (unsigned i = 0, compCount = static_cast<unsigned>(castType->getNumElements()); i < compCount; i++) {
          Value *const elem = m_builder.CreateExtractElement(part, i);
          copyMetadata(elem, &inst);
          newInst = m_builder.CreateInsertElement(newInst, elem, index++);
          copyMetadata(newInst, &inst);
        }
      }
    }
    assert(newInst);

    if (type->isPointerTy()) {
      newInst = m_builder.CreateBitCast(newInst, m_builder.getIntNTy(bytesToHandle * 8));
      copyMetadata(newInst, &inst);
      newInst = m_builder.CreateIntToPtr(newInst, type);
      copyMetadata(newInst, &inst);
    } else {
      newInst = m_builder.CreateBitCast(newInst, type);
      copyMetadata(newInst, &inst);
    }
  }

  switch (ordering) {
  case AtomicOrdering::Acquire:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    m_builder.CreateFence(AtomicOrdering::Acquire, syncScopeID);
    break;
  default:
    break;
  }

  return newInst;
}

// =====================================================================================================================
// Make a loop, returning the value of the loop counter. This modifies the insertion point of the builder.
//
// @param loopStart : The start index of the loop.
// @param loopEnd : The end index of the loop.
// @param loopStride : The stride of the loop.
// @param insertPos : The position to insert the loop in the instruction stream.
Instruction *BufferOpLowering::makeLoop(Value *const loopStart, Value *const loopEnd, Value *const loopStride,
                                        Instruction *const insertPos) {
  Value *const initialCond = m_builder.CreateICmpNE(loopStart, loopEnd);

  BasicBlock *const origBlock = insertPos->getParent();

  Instruction *const terminator = SplitBlockAndInsertIfThen(initialCond, insertPos, false);

  m_builder.SetInsertPoint(terminator);

  // Create a phi node for the loop counter.
  PHINode *const loopCounter = m_builder.CreatePHI(loopStart->getType(), 2);
  copyMetadata(loopCounter, insertPos);

  // Set the loop counter to start value (initialization).
  loopCounter->addIncoming(loopStart, origBlock);

  // Calculate the next value of the loop counter by doing loopCounter + loopStride.
  Value *const loopNextValue = m_builder.CreateAdd(loopCounter, loopStride);
  copyMetadata(loopNextValue, insertPos);

  // And set the loop counter to the next value.
  loopCounter->addIncoming(loopNextValue, terminator->getParent());

  // Our loop condition is just whether the next value of the loop counter is less than the end value.
  Value *const cond = m_builder.CreateICmpULT(loopNextValue, loopEnd);
  copyMetadata(cond, insertPos);

  // And our replacement terminator just branches back to the if body if there is more loop iterations to be done.
  Instruction *const newTerminator = m_builder.CreateCondBr(cond, terminator->getParent(), terminator->getSuccessor(0));
  copyMetadata(newTerminator, insertPos);

  terminator->eraseFromParent();

  m_builder.SetInsertPoint(newTerminator);

  return loopCounter;
}
