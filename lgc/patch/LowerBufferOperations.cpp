/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerBufferOperations.cpp
 * @brief LLPC source file: contains implementation of class lgc::LowerBufferOperations.
 ***********************************************************************************************************************
 */
#include "lgc/patch/LowerBufferOperations.h"
#include "lgc/CommonDefs.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "lgc-lower-buffer-operations"

using namespace CompilerUtils;
using namespace llvm;
using namespace lgc;

namespace {

struct LowerBufferOperationsImpl {
  LowerBufferOperationsImpl(LLVMContext &context, PipelineState &pipelineState, UniformityInfo &uniformityInfo);

  bool run(Function &function);

  TypeLowering m_typeLowering;
  BufferOpLowering m_bufferOpLowering;
};

} // anonymous namespace

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(LowerBufferOperationsImpl, m_typeLowering)
LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(LowerBufferOperationsImpl, m_bufferOpLowering)

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in/out] function : LLVM function to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerBufferOperations::run(Function &function, FunctionAnalysisManager &analysisManager) {
  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();
  UniformityInfo &uniformityInfo = analysisManager.getResult<UniformityInfoAnalysis>(function);

  LowerBufferOperationsImpl impl(function.getContext(), *pipelineState, uniformityInfo);
  if (impl.run(function))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Construct the per-run temporaries of the LowerBufferOperations pass.
LowerBufferOperationsImpl::LowerBufferOperationsImpl(LLVMContext &context, PipelineState &pipelineState,
                                                     UniformityInfo &uniformityInfo)
    : m_typeLowering(context), m_bufferOpLowering(m_typeLowering, pipelineState, uniformityInfo) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM function.
//
// @param [in,out] function : LLVM function to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LowerBufferOperationsImpl::run(Function &function) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Buffer-Op on: " << function.getName() << '\n');

  static const auto visitor = llvm_dialects::VisitorBuilder<LowerBufferOperationsImpl>()
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
    auto &context = type->getContext();
    switch (pointerType->getAddressSpace()) {
    case ADDR_SPACE_BUFFER_FAT_POINTER:
      types.push_back(FixedVectorType::get(Type::getInt32Ty(context), 4)); // the concrete 128-bit descriptor
      types.push_back(PointerType::get(context, ADDR_SPACE_CONST_32BIT));
      types.push_back(Type::getIntNTy(context, 1)); // whether indexed access is possible
      types.push_back(Type::getInt32Ty(context));   // the index, if an indexed access is possible; and poison otherwise
      break;
    case ADDR_SPACE_BUFFER_STRIDED_POINTER:
      types.push_back(FixedVectorType::get(Type::getInt32Ty(context), 4));
      types.push_back(PointerType::get(context, ADDR_SPACE_CONST_32BIT));
      types.push_back(Type::getInt32Ty(context));
      types.push_back(Type::getIntNTy(context, 1)); // whether indexed access is possible
      types.push_back(Type::getInt32Ty(context));   // the index, if an indexed access is possible; and poison otherwise
      break;
    default:
      break;
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
BufferOpLowering::BufferOpLowering(TypeLowering &typeLowering, PipelineState &pipelineState,
                                   UniformityInfo &uniformityInfo)
    : m_typeLowering(typeLowering), m_builder(&pipelineState), m_pipelineState(pipelineState),
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
  builder.add(&BufferOpLowering::visitBufferAddrToPtr);
  builder.add(&BufferOpLowering::visitBufferDescToPtr);
  builder.add(&BufferOpLowering::visitConvertToStridedBufferPointer);
  builder.add(&BufferOpLowering::visitStridedBufferDescToPtr);
  builder.add(&BufferOpLowering::visitBufferLoadDescToPtr);
  builder.add(&BufferOpLowering::visitStridedBufferLoadDescToPtr);
  builder.add(&BufferOpLowering::visitStridedBufferAddrAndStrideToPtr);
  builder.add(&BufferOpLowering::visitStridedIndexAdd);
  builder.add(&BufferOpLowering::visitBufferLength);
  builder.add(&BufferOpLowering::visitBufferPtrDiff);
  builder.add(&BufferOpLowering::visitGetElementPtrInst);
  builder.add(&BufferOpLowering::visitLoadTfeOp);
  builder.add(&BufferOpLowering::visitLoadInst);
  builder.add(&BufferOpLowering::visitMemCpyInst);
  builder.add(&BufferOpLowering::visitMemMoveInst);
  builder.add(&BufferOpLowering::visitMemSetInst);
  builder.add(&BufferOpLowering::visitPhiInst);
  builder.add(&BufferOpLowering::visitStoreInst);
  builder.add(&BufferOpLowering::visitICmpInst);
  builder.addIntrinsic(Intrinsic::invariant_start, &BufferOpLowering::visitInvariantStart);
  builder.addIntrinsic(Intrinsic::amdgcn_readfirstlane, &BufferOpLowering::visitReadFirstLane);
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
        LLVM_DEBUG(dbgs() << "Divergent PHI of descriptor: " << *newPhi << '\n');
      }
    }
  }

  static const auto visitor = llvm_dialects::VisitorBuilder<BufferOpLowering>()
                                  .add(&BufferOpLowering::postVisitLoadInst)
                                  .add(&BufferOpLowering::postVisitLoadTfeOp)
                                  .add(&BufferOpLowering::postVisitMemCpyInst)
                                  .add(&BufferOpLowering::postVisitMemSetInst)
                                  .add(&BufferOpLowering::postVisitStoreInst)
                                  .build();

  SmallVector<Instruction *> instructions;
  std::swap(instructions, m_postVisitInsts);
  for (Instruction *inst : llvm::reverse(instructions))
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
          if (m_uniformityInfo.isDivergentUse(select->getOperandUse(0))) {
            di.divergent = true;
            LLVM_DEBUG(dbgs() << "Divergent descriptor: " << *select << '\n');
          }

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
            LLVM_DEBUG(dbgs() << (di.divergent.value() ? "Divergent" : "Uniform") << " descriptor: " << *current
                              << '\n');
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
        LLVM_DEBUG(dbgs() << "Variant descriptor: " << *user << '\n');
        userDi.invariant = false;
        propagate = true;
      }
      if (!userDi.divergent.has_value() && di.divergent.value_or(false)) {
        LLVM_DEBUG(dbgs() << "Divergent descriptor: " << *user << '\n');
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
    if (!di.invariant.has_value()) {
      di.invariant = true;
      LLVM_DEBUG(dbgs() << "Invariant descriptor: " << *current << '\n');
    }
    if (!di.divergent.has_value()) {
      di.divergent = false;
      LLVM_DEBUG(dbgs() << "Uniform descriptor: " << *current << '\n');
    }
  }

  return m_descriptors.find(desc)->second;
}

// =====================================================================================================================
// Determine if a value is a buffer pointer. A buffer pointer is either a BUFFER_FAT_POINTER or
// a BUFFER_STRIDED_POINTER
//
// @param value : The value to check
bool BufferOpLowering::isAnyBufferPointer(const Value *const value) {
  return value->getType() == m_builder.getPtrTy(ADDR_SPACE_BUFFER_FAT_POINTER) ||
         value->getType() == m_builder.getPtrTy(ADDR_SPACE_BUFFER_STRIDED_POINTER);
}

// =====================================================================================================================
// Visits "cmpxchg" instruction.
//
// @param atomicCmpXchgInst : The instruction
void BufferOpLowering::visitAtomicCmpXchgInst(AtomicCmpXchgInst &atomicCmpXchgInst) {
  // If the type we are doing an atomic operation on is not a buffer pointer, bail.
  if (!isAnyBufferPointer(atomicCmpXchgInst.getPointerOperand()))
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
    auto createAtomicCmpXchgFunc = [&](Value *pointer) {
      const AtomicOrdering successOrdering = atomicCmpXchgInst.getSuccessOrdering();
      const AtomicOrdering failureOrdering = atomicCmpXchgInst.getFailureOrdering();

      Value *const compareValue = atomicCmpXchgInst.getCompareOperand();
      Value *const newValue = atomicCmpXchgInst.getNewValOperand();
      AtomicCmpXchgInst *const newAtomicCmpXchg = m_builder.CreateAtomicCmpXchg(
          pointer, compareValue, newValue, MaybeAlign(), successOrdering, failureOrdering);
      newAtomicCmpXchg->setVolatile(atomicCmpXchgInst.isVolatile());
      newAtomicCmpXchg->setSyncScopeID(atomicCmpXchgInst.getSyncScopeID());
      newAtomicCmpXchg->setWeak(atomicCmpXchgInst.isWeak());
      copyMetadata(newAtomicCmpXchg, &atomicCmpXchgInst);
      return newAtomicCmpXchg;
    };
    // The index should be used when a strided pointer is converted to offset mode.
    Value *index = nullptr;
    if (atomicCmpXchgInst.getPointerOperand()->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER)
      index = values[2];
    Value *result =
        createGlobalPointerAccess(bufferDesc, baseIndex, index, storeType, atomicCmpXchgInst, createAtomicCmpXchgFunc);

    // Record the atomic instruction so we remember to delete it later.
    m_typeLowering.eraseInstruction(&atomicCmpXchgInst);

    atomicCmpXchgInst.replaceAllUsesWith(result);
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

    Value *atomicCall;
    if (atomicCmpXchgInst.getPointerAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER) {
      Value *const index = values[2];
      atomicCall = m_builder.CreateIntrinsic(storeType, Intrinsic::amdgcn_struct_buffer_atomic_cmpswap,
                                             {atomicCmpXchgInst.getNewValOperand(),
                                              atomicCmpXchgInst.getCompareOperand(), bufferDesc, index, baseIndex,
                                              m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
    } else {
      atomicCall = m_builder.CreateIntrinsic(storeType, Intrinsic::amdgcn_raw_buffer_atomic_cmpswap,
                                             {atomicCmpXchgInst.getNewValOperand(),
                                              atomicCmpXchgInst.getCompareOperand(), bufferDesc, baseIndex,
                                              m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
    }

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
  if (isAnyBufferPointer(atomicRmwInst.getPointerOperand())) {
    m_builder.SetInsertPoint(&atomicRmwInst);

    auto values = m_typeLowering.getValue(atomicRmwInst.getPointerOperand());

    Type *const storeType = atomicRmwInst.getValOperand()->getType();

    const bool isNonTemporal = atomicRmwInst.getMetadata(LLVMContext::MD_nontemporal);

    Value *const bufferDesc = values[0];
    Value *const baseIndex = m_builder.CreatePtrToInt(values[1], m_builder.getInt32Ty());
    copyMetadata(baseIndex, &atomicRmwInst);

    // If our buffer descriptor is divergent, need to handle it differently.
    if (getDescriptorInfo(bufferDesc).divergent.value()) {
      auto createAtomicRmwFunc = [&](Value *pointer) {
        AtomicRMWInst *const newAtomicRmw =
            m_builder.CreateAtomicRMW(atomicRmwInst.getOperation(), pointer, atomicRmwInst.getValOperand(),
                                      atomicRmwInst.getAlign(), atomicRmwInst.getOrdering());
        newAtomicRmw->setVolatile(atomicRmwInst.isVolatile());
        newAtomicRmw->setSyncScopeID(atomicRmwInst.getSyncScopeID());
        copyMetadata(newAtomicRmw, &atomicRmwInst);
        return newAtomicRmw;
      };
      // The index should be used when a strided pointer is converted to offset mode.
      Value *index = nullptr;
      if (atomicRmwInst.getPointerOperand()->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER)
        index = values[2];
      Value *result =
          createGlobalPointerAccess(bufferDesc, baseIndex, index, storeType, atomicRmwInst, createAtomicRmwFunc);

      // Record the atomic instruction so we remember to delete it later.
      m_typeLowering.eraseInstruction(&atomicRmwInst);

      atomicRmwInst.replaceAllUsesWith(result);
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
      bool isStructBuffer = atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER;
      switch (atomicRmwInst.getOperation()) {
      case AtomicRMWInst::Xchg:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_swap : Intrinsic::amdgcn_raw_buffer_atomic_swap;
        break;
      case AtomicRMWInst::Add:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_add : Intrinsic::amdgcn_raw_buffer_atomic_add;
        break;
      case AtomicRMWInst::Sub:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_sub : Intrinsic::amdgcn_raw_buffer_atomic_sub;
        break;
      case AtomicRMWInst::And:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_and : Intrinsic::amdgcn_raw_buffer_atomic_and;
        break;
      case AtomicRMWInst::Or:
        intrinsic = isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_or : Intrinsic::amdgcn_raw_buffer_atomic_or;
        break;
      case AtomicRMWInst::Xor:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_xor : Intrinsic::amdgcn_raw_buffer_atomic_xor;
        break;
      case AtomicRMWInst::Max:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_smax : Intrinsic::amdgcn_raw_buffer_atomic_smax;
        break;
      case AtomicRMWInst::Min:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_smin : Intrinsic::amdgcn_raw_buffer_atomic_smin;
        break;
      case AtomicRMWInst::UMax:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_umax : Intrinsic::amdgcn_raw_buffer_atomic_umax;
        break;
      case AtomicRMWInst::UMin:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_umin : Intrinsic::amdgcn_raw_buffer_atomic_umin;
        break;
      case AtomicRMWInst::FAdd:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_fadd : Intrinsic::amdgcn_raw_buffer_atomic_fadd;
        break;
      case AtomicRMWInst::FMax:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_fmax : Intrinsic::amdgcn_raw_buffer_atomic_fmax;
        break;
      case AtomicRMWInst::FMin:
        intrinsic =
            isStructBuffer ? Intrinsic::amdgcn_struct_buffer_atomic_fmin : Intrinsic::amdgcn_raw_buffer_atomic_fmin;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }

      CoherentFlag coherent = {};
      if (m_pipelineState.getTargetInfo().getGfxIpVersion().major <= 11) {
        coherent.bits.slc = isNonTemporal ? 1 : 0;
      }

      Value *atomicCall;
      if (isStructBuffer) {
        Value *const index = values[2];
        atomicCall = m_builder.CreateIntrinsic(storeType, intrinsic,
                                               {atomicRmwInst.getValOperand(), bufferDesc, index, baseIndex,
                                                m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
      } else {
        atomicCall = m_builder.CreateIntrinsic(storeType, intrinsic,
                                               {atomicRmwInst.getValOperand(), bufferDesc, baseIndex,
                                                m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
      }
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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 502630
  } else if (atomicRmwInst.getPointerAddressSpace() == ADDR_SPACE_GLOBAL) {
    AtomicRMWInst::BinOp op = atomicRmwInst.getOperation();
    Type *const storeType = atomicRmwInst.getValOperand()->getType();
    if (op == AtomicRMWInst::FMin || op == AtomicRMWInst::FMax) {
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
    if (op == AtomicRMWInst::FMin || op == AtomicRMWInst::FMax) {
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
#endif
  }
}

// =====================================================================================================================
// Visits "bitcast" instruction.
//
// @param bitCastInst : The instruction
void BufferOpLowering::visitBitCastInst(BitCastInst &bitCastInst) {
  // If the pointer is not a buffer pointer, bail.
  if (!isAnyBufferPointer(&bitCastInst))
    return;

  m_typeLowering.replaceInstruction(&bitCastInst, m_typeLowering.getValue(bitCastInst.getOperand(0)));
}

// =====================================================================================================================
// Lower a buffer.addr.to.ptr op, to convert an i64 address to a buffer fat pointer.
void BufferOpLowering::visitBufferAddrToPtr(BufferAddrToPtrOp &op) {
  m_builder.SetInsertPoint(&op);

  Value *address = m_builder.CreatePtrToInt(op.getAddress(), m_builder.getInt64Ty());
  address = m_builder.CreateBitCast(address, FixedVectorType::get(m_builder.getInt32Ty(), 2));
  Value *descriptor = m_builder.buildBufferCompactDesc(address, nullptr);

  m_typeLowering.replaceInstruction(&op, {descriptor, ConstantPointerNull::get(m_offsetType), m_builder.getFalse(),
                                          PoisonValue::get(m_builder.getInt32Ty())});

  auto &di = m_descriptors[descriptor];

  di.divergent = m_uniformityInfo.isDivergent(op.getAddress());
  LLVM_DEBUG(dbgs() << (di.divergent.value() ? "Divergent" : "Uniform") << " descriptor: " << *descriptor << '\n');

  di.globallyCoherent = op.getGloballyCoherent();
}

// =====================================================================================================================
// Visits "buffer.desc.to.ptr" instruction.
//
// @param descToPtr : The instruction
void BufferOpLowering::visitBufferDescToPtr(BufferDescToPtrOp &descToPtr) {
  m_builder.SetInsertPoint(&descToPtr);

  auto *descriptor = descToPtr.getDesc();
  m_typeLowering.replaceInstruction(&descToPtr, {descriptor, ConstantPointerNull::get(m_offsetType),
                                                 m_builder.getFalse(), PoisonValue::get(m_builder.getInt32Ty())});

  auto &di = m_descriptors[descriptor];

  di.divergent = m_uniformityInfo.isDivergent(descToPtr.getDesc());
  LLVM_DEBUG(dbgs() << (di.divergent.value() ? "Divergent" : "Uniform") << " descriptor: " << *descriptor << '\n');

  di.globallyCoherent = descToPtr.getGloballyCoherent();
}

// =====================================================================================================================
// Visits "convert.to.strided.buffer.pointer" instruction.
//
// @param convertToStrided : The instruction
void BufferOpLowering::visitConvertToStridedBufferPointer(ConvertToStridedBufferPointerOp &convertToStrided) {
  auto values = m_typeLowering.getValue(convertToStrided.getPtr());

  m_builder.SetInsertPoint(&convertToStrided);

  auto *oldDescriptor = values[0];

  auto *currentDword1 = m_builder.CreateExtractElement(oldDescriptor, 1);
  auto *stride = m_builder.getInt32(convertToStrided.getStride());
  auto *newDword1 = m_builder.CreateAnd(currentDword1, ~0x3FFF0000);
  newDword1 = m_builder.CreateOr(newDword1, m_builder.CreateShl(stride, 16));
  auto *newDescriptor = m_builder.CreateInsertElement(oldDescriptor, newDword1, 1);

  auto *currentNumRecords = m_builder.CreateExtractElement(newDescriptor, 2);
  auto *newNumRecords = m_builder.CreateUDiv(currentNumRecords, stride);
  newDescriptor = m_builder.CreateInsertElement(newDescriptor, newNumRecords, 2);

  auto *currentDword3 = m_builder.CreateExtractElement(newDescriptor, 3);
  currentDword3 = m_builder.CreateAnd(currentDword3, 0xCFFFFFFF);
  currentDword3 = m_builder.CreateOr(currentDword3, 0x10000000);
  newDescriptor = m_builder.CreateInsertElement(newDescriptor, currentDword3, 3);

  m_typeLowering.replaceInstruction(&convertToStrided,
                                    {newDescriptor, values[1], m_builder.getInt32(0), m_builder.getFalse(),
                                     PoisonValue::get(m_builder.getInt32Ty())});

  DescriptorInfo di = m_descriptors.lookup(oldDescriptor);
  m_descriptors.insert({newDescriptor, di});
  m_stridedDescriptors.insert({newDescriptor, {oldDescriptor, stride}});
}

// =====================================================================================================================
// Visits "strided.buffer.desc.to.ptr" instruction.
//
// @param descToPtr : The instruction
void BufferOpLowering::visitStridedBufferDescToPtr(StridedBufferDescToPtrOp &descToPtr) {
  m_builder.SetInsertPoint(&descToPtr);

  auto *descriptor = descToPtr.getDesc();
  m_typeLowering.replaceInstruction(&descToPtr,
                                    {descriptor, ConstantPointerNull::get(m_offsetType), m_builder.getInt32(0),
                                     m_builder.getFalse(), PoisonValue::get(m_builder.getInt32Ty())});

  auto &di = m_descriptors[descriptor];

  di.divergent = m_uniformityInfo.isDivergent(descriptor);
  LLVM_DEBUG(dbgs() << (di.divergent.value() ? "Divergent" : "Uniform") << " descriptor: " << *descriptor << '\n');

  di.globallyCoherent = descToPtr.getGloballyCoherent();
}

// =====================================================================================================================
// Visits "strided.buffer.addr.and.stride.to.ptr" instruction.
//
// @param addrAndStrideToPtr : The instruction
void BufferOpLowering::visitStridedBufferAddrAndStrideToPtr(StridedBufferAddrAndStrideToPtrOp &addrAndStrideToPtr) {
  m_builder.SetInsertPoint(&addrAndStrideToPtr);

  Value *address = m_builder.CreatePtrToInt(addrAndStrideToPtr.getAddress(), m_builder.getInt64Ty());
  address = m_builder.CreateBitCast(address, FixedVectorType::get(m_builder.getInt32Ty(), 2));
  Value *bufDesc = m_builder.buildBufferCompactDesc(address, addrAndStrideToPtr.getStride());

  Constant *const nullPointerOff = ConstantPointerNull::get(m_offsetType);
  m_typeLowering.replaceInstruction(
      &addrAndStrideToPtr,
      {bufDesc, nullPointerOff, m_builder.getInt32(0), m_builder.getFalse(), PoisonValue::get(m_builder.getInt32Ty())});

  auto &di = m_descriptors[bufDesc];

  di.divergent = m_uniformityInfo.isDivergent(addrAndStrideToPtr.getAddress());
  di.globallyCoherent = addrAndStrideToPtr.getGloballyCoherent();
}

// =====================================================================================================================
// Visits "buffer.load.desc.to.ptr" instruction.
//
// @param loadDescToPtr : The instruction
void BufferOpLowering::visitBufferLoadDescToPtr(BufferLoadDescToPtrOp &loadDescToPtr) {
  m_builder.SetInsertPoint(&loadDescToPtr);
  bool needLoadDesc = true;
  // NOTE: Rely on later cleanup passes to handle the case where we create descriptor load instructions that end up
  // being unnecessary due to indexed loads
  Value *descriptor =
      createLoadDesc(loadDescToPtr.getDescPtr(), loadDescToPtr.getForceRawView(), loadDescToPtr.getIsCompact());
  if (needLoadDesc) {
    if (loadDescToPtr.getIsCompact())
      descriptor = m_builder.buildBufferCompactDesc(descriptor, nullptr);

    m_typeLowering.replaceInstruction(&loadDescToPtr, {descriptor, ConstantPointerNull::get(m_offsetType),
                                                       m_builder.getFalse(), PoisonValue::get(m_builder.getInt32Ty())});
  } else {
    Value *index = m_builder.CreatePtrToInt(loadDescToPtr.getDescPtr(), m_builder.getInt64Ty());
    index = m_builder.CreateBitCast(index, FixedVectorType::get(m_builder.getInt32Ty(), 2));
    index = m_builder.CreateExtractElement(index, m_builder.getInt64(0));
    m_typeLowering.replaceInstruction(&loadDescToPtr,
                                      {descriptor, ConstantPointerNull::get(m_offsetType), m_builder.getTrue(), index});
  }

  auto &di = m_descriptors[descriptor];

  // The loadInst isn't computed by UniformityAnalysis so that we should use its source for divergent check
  Value *loadSrc = loadDescToPtr.getDescPtr();

  di.divergent = m_uniformityInfo.isDivergent(loadSrc);
  LLVM_DEBUG(dbgs() << (di.divergent.value() ? "Divergent" : "Uniform") << " descriptor: " << *descriptor << '\n');

  di.globallyCoherent = loadDescToPtr.getGloballyCoherent();
}

// =====================================================================================================================
// Visits "strided.buffer.load.desc.to.ptr" instruction.
//
// @param loadDescToPtr : The instruction
void BufferOpLowering::visitStridedBufferLoadDescToPtr(StridedBufferLoadDescToPtrOp &loadDescToPtr) {
  m_builder.SetInsertPoint(&loadDescToPtr);
  bool needLoadDesc = true;
  Value *descriptor =
      createLoadDesc(loadDescToPtr.getDescPtr(), loadDescToPtr.getForceRawView(), loadDescToPtr.getIsCompact());
  if (needLoadDesc) {
    if (loadDescToPtr.getIsCompact())
      descriptor = m_builder.buildBufferCompactDesc(descriptor, loadDescToPtr.getStride());

    m_typeLowering.replaceInstruction(&loadDescToPtr,
                                      {descriptor, ConstantPointerNull::get(m_offsetType), m_builder.getInt32(0),
                                       m_builder.getFalse(), PoisonValue::get(m_builder.getInt32Ty())});
  } else {
    Value *index = m_builder.CreateBitCast(loadDescToPtr.getDescPtr(), m_builder.getInt32Ty());
    m_typeLowering.replaceInstruction(&loadDescToPtr, {descriptor, ConstantPointerNull::get(m_offsetType),
                                                       m_builder.getInt32(0), m_builder.getTrue(), index});
  }

  auto &di = m_descriptors[descriptor];

  // The loadInst isn't computed by UniformityAnalysis so that we should use its source for divergent check
  Value *loadSrc = loadDescToPtr.getDescPtr();

  di.divergent = m_uniformityInfo.isDivergent(loadSrc);
  LLVM_DEBUG(dbgs() << (di.divergent.value() ? "Divergent" : "Uniform") << " descriptor: " << *descriptor << '\n');

  di.globallyCoherent = loadDescToPtr.getGloballyCoherent();
}

// =====================================================================================================================
// Visits "strided.index.add" instruction.
//
// @param indexAdd : The instruction
void BufferOpLowering::visitStridedIndexAdd(StridedIndexAddOp &indexAdd) {
  auto values = m_typeLowering.getValue(indexAdd.getPtr());
  auto deltaIndex = indexAdd.getDeltaIdx();

  if (auto deltaIndexInt = dyn_cast<ConstantInt>(deltaIndex); deltaIndexInt && deltaIndexInt->isZero()) {
    m_typeLowering.replaceInstruction(&indexAdd, values);
    return;
  }

  // If the old index zero, we can skip the addition and just take the delta index
  // Otherwise, we need to add the delta index to the old one.
  if (auto oldIndexInt = dyn_cast<ConstantInt>(values[2]); !oldIndexInt || !(oldIndexInt->isZero())) {
    m_builder.SetInsertPoint(&indexAdd);
    deltaIndex = m_builder.CreateAdd(values[2], deltaIndex);
  }

  m_typeLowering.replaceInstruction(&indexAdd, {values[0], values[1], deltaIndex, values[3], values[4]});
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
  // If the type we are GEPing into is not a fat or strided pointer, bail.
  if (!isAnyBufferPointer(getElemPtrInst.getPointerOperand()))
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

  if (getElemPtrInst.getAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER)
    m_typeLowering.replaceInstruction(&getElemPtrInst, {values[0], newGetElemPtr, values[2], values[3], values[4]});
  else
    m_typeLowering.replaceInstruction(&getElemPtrInst, {values[0], newGetElemPtr, values[2], values[3]});
}

// =====================================================================================================================
// Visits "load" instruction.
//
// @param loadInst : The instruction
void BufferOpLowering::visitLoadInst(LoadInst &loadInst) {
  const auto pointerOperand = loadInst.getPointerOperand();

  if (!isAnyBufferPointer(pointerOperand))
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
  // Replace the mapping.
  m_typeLowering.replaceValue(&loadInst, newLoad);

  loadInst.replaceAllUsesWith(newLoad);
}

// =====================================================================================================================
// Visits "memcpy" instruction.
//
// @param memCpyInst : The memcpy instruction
void BufferOpLowering::visitMemCpyInst(MemCpyInst &memCpyInst) {
  Value *const dest = memCpyInst.getArgOperand(0);
  Value *const src = memCpyInst.getArgOperand(1);

  // If either of the address spaces are buffer pointers.
  if (isAnyBufferPointer(src) || isAnyBufferPointer(dest)) {
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

  // If either of the address spaces are not buffer pointers, bail.
  if (!isAnyBufferPointer(dest) || !isAnyBufferPointer(src))
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

  // If the address spaces is a buffer pointer.
  if (isAnyBufferPointer(dest)) {
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
//  - phi nodes of buffer pointers are very often divergent, but the descriptor part is actually uniform; only the
//  offset
//    part that is divergent. So we do our own mini-divergence analysis on the descriptor values after the first visitor
//    pass.
//  - TypeLowering helps us by automatically eliminating descriptor phi nodes in typical cases where they're redundant.
//
// @param phi : The instruction
void BufferOpLowering::visitPhiInst(llvm::PHINode &phi) {
  if (!isAnyBufferPointer(&phi))
    return;

  if (m_uniformityInfo.isDivergent(&phi))
    m_divergentPhis.push_back(&phi);
}

// =====================================================================================================================
// Visits "store" instruction.
//
// @param storeInst : The instruction
void BufferOpLowering::visitStoreInst(StoreInst &storeInst) {
  // If the address space of the store pointer is not a buffer pointer, bail.
  if (!isAnyBufferPointer(storeInst.getPointerOperand()))
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
  Value *const pointer = icmpInst.getOperand(0);

  // If the pointer is not a fat pointer, bail.
  if (!isAnyBufferPointer(pointer))
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
  if (!isAnyBufferPointer(ptr))
    return;

  auto values = m_typeLowering.getValue(ptr);
  Value *desc = values[0];

  m_descriptors[desc].invariant = true;

  m_typeLowering.eraseInstruction(&intrinsic);
}

// =====================================================================================================================
// Visits read first lane intrinsic.
//
// @param intrinsic : The intrinsic
void BufferOpLowering::visitReadFirstLane(llvm::IntrinsicInst &intrinsic) {
  if (!isAnyBufferPointer(&intrinsic))
    return;

  auto values = m_typeLowering.getValue(intrinsic.getArgOperand(0));
  Value *ptr = values[1];
  ptr = m_builder.CreateIntrinsic(ptr->getType(), Intrinsic::amdgcn_readfirstlane, ptr);

  m_typeLowering.replaceInstruction(&intrinsic, {values[0], ptr, values[2], values[3]});
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

    if (isAnyBufferPointer(srcPtr))
      postVisitLoadInst(*srcLoad);
    if (isAnyBufferPointer(destPtr))
      postVisitStoreInst(*destStore);
  } else {
    // Get an vector type that is the length of the memcpy.
    VectorType *const memoryType = FixedVectorType::get(m_builder.getInt8Ty(), lengthConstant->getZExtValue());

    LoadInst *const srcLoad = m_builder.CreateAlignedLoad(memoryType, src, srcAlignment);
    copyMetadata(srcLoad, &memCpyInst);

    StoreInst *const destStore = m_builder.CreateAlignedStore(srcLoad, dest, destAlignment);
    copyMetadata(destStore, &memCpyInst);

    if (isAnyBufferPointer(src))
      postVisitLoadInst(*srcLoad);
    if (isAnyBufferPointer(dest))
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

    Value *const castDest = m_builder.CreateBitCast(destPtr, m_builder.getPtrTy(destAddrSpace));
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

      Type *const int8PtrTy = m_builder.getPtrTy(ADDR_SPACE_PRIVATE);
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
// Visits "load.tfe" instruction.
//
// @param loadTfe : The instruction
void BufferOpLowering::visitLoadTfeOp(LoadTfeOp &loadTfe) {
  assert(isAnyBufferPointer(loadTfe.getPointer()));
  m_postVisitInsts.push_back(&loadTfe);
}

// =====================================================================================================================
// Visits "load.tfe" instruction after the initial pass, when phi nodes have been fixed up and potentially simplified.
//
// @param loadTfe : the instruction
void BufferOpLowering::postVisitLoadTfeOp(LoadTfeOp &loadTfe) {
  Value *pointerOperand = loadTfe.getPointer();

  m_builder.SetInsertPoint(&loadTfe);
  auto pointerValues = m_typeLowering.getValue(pointerOperand);
  Value *bufferDesc = pointerValues[0];
  Value *const offset = m_builder.CreatePtrToInt(pointerValues[1], m_builder.getInt32Ty());
  Instruction *bufferLoad = nullptr;

  if (pointerOperand->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
    bufferLoad = m_builder.CreateIntrinsic(loadTfe.getType(), Intrinsic::amdgcn_raw_buffer_load,
                                           {bufferDesc, offset, m_builder.getInt32(0), m_builder.getInt32(0)});
  } else {
    Value *index = pointerValues[2];
    bufferLoad = m_builder.CreateIntrinsic(loadTfe.getType(), Intrinsic::amdgcn_struct_buffer_load,
                                           {bufferDesc, index, offset, m_builder.getInt32(0), m_builder.getInt32(0)});
  }
  if (getDescriptorInfo(bufferDesc).divergent.value())
    bufferLoad = m_builder.createWaterfallLoop(bufferLoad, 0, false);

  // Record the load instruction so we remember to delete it later.
  m_typeLowering.eraseInstruction(&loadTfe);
  // Replace the mapping.
  m_typeLowering.replaceValue(&loadTfe, bufferLoad);
  loadTfe.replaceAllUsesWith(bufferLoad);
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
  return m_builder.CreateIntToPtr(baseAddr, m_builder.getPtrTy(ADDR_SPACE_GLOBAL));
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

  const bool isStridedPointer =
      pointerOperand->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER;
  auto pointerValues = m_typeLowering.getValue(pointerOperand);
  unsigned id = isStridedPointer ? 3 : 2;
  Value *bufferDesc = pointerValues[0];
  bool isIndexedDesc = false;
  if (isa<ConstantInt>(pointerValues[id])) {
    isIndexedDesc = cast<ConstantInt>(pointerValues[id])->isOne();
    if (isIndexedDesc)
      bufferDesc = pointerValues[id + 1];
  }

  const DataLayout &dataLayout = m_builder.GetInsertBlock()->getModule()->getDataLayout();

  const unsigned bytesToHandle = static_cast<unsigned>(dataLayout.getTypeStoreSize(type));

  bool isInvariant = false;
  if (isLoad) {
    isInvariant =
        getDescriptorInfo(bufferDesc).invariant.value() || loadInst->getMetadata(LLVMContext::MD_invariant_load);
  }

  const bool isNonTemporal = inst.getMetadata(LLVMContext::MD_nontemporal);
  const bool isGlc =
      ordering != AtomicOrdering::NotAtomic || m_descriptors[bufferDesc].globallyCoherent.value_or(false);
  const bool isDlc = isGlc; // For buffer load on GFX10+, we set DLC = GLC

  Value *const baseIndex = m_builder.CreatePtrToInt(pointerValues[1], m_builder.getInt32Ty());
  const bool isDivergentDesc = getDescriptorInfo(bufferDesc).divergent.value();

  if (!isIndexedDesc && isDivergentDesc) {
    // If our buffer descriptor is divergent, need to handle that differently in non resource indexing mode.
    auto createLoadStoreFunc = [&](Value *pointer) {
      Value *result = nullptr;
      if (isLoad) {
        LoadInst *const newLoad = m_builder.CreateAlignedLoad(type, pointer, alignment, loadInst->isVolatile());
        newLoad->setOrdering(ordering);
        newLoad->setSyncScopeID(syncScopeID);
        copyMetadata(newLoad, loadInst);

        if (isInvariant)
          newLoad->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));
        result = newLoad;
      } else {
        StoreInst *const newStore =
            m_builder.CreateAlignedStore(storeInst->getValueOperand(), pointer, alignment, storeInst->isVolatile());
        newStore->setOrdering(ordering);
        newStore->setSyncScopeID(syncScopeID);
        copyMetadata(newStore, storeInst);
        result = newStore;
      }
      return result;
    };
    // The index should be used when a strided pointer is converted to offset mode.
    Value *index = nullptr;
    if (pointerOperand->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_STRIDED_POINTER)
      index = pointerValues[2];
    return createGlobalPointerAccess(bufferDesc, baseIndex, index, type, inst, createLoadStoreFunc);
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

    Value *indexValue = isStridedPointer ? pointerValues[2] : nullptr;
    if (isLoad) {
      bool accessSizeAllowed = true;
      if (m_pipelineState.getTargetInfo().getGfxIpVersion().major <= 11) {
        // TODO For stores?
        coherent.bits.dlc = isDlc;
        accessSizeAllowed = accessSize >= 4;
      }

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
      // Old version of the code
      const bool isDivergentPtr = m_uniformityInfo.isDivergent(*pointerOperand);
#else
      // New version of the code (also handles unknown version, which we treat as latest)
      const bool isDivergentPtr = m_uniformityInfo.isDivergent(pointerOperand);
#endif
      const bool haveNonStridedDescriptor = !isStridedPointer || m_stridedDescriptors.contains(bufferDesc);
      if (isInvariant && !isDivergentDesc && accessSizeAllowed && haveNonStridedDescriptor &&
          (!indexValue || isa<ConstantInt>(indexValue) || !isDivergentPtr)) {
        // create s.buffer.load
        Value *desc = bufferDesc;
        if (isIndexedDesc)
          desc = m_builder.CreateLoad(FixedVectorType::get(m_builder.getInt32Ty(), 4), bufferDesc);
        if (isStridedPointer) {
          // Especially when the index is a constant, and the stride is known at compile-time,
          // we should create s_buffer_load instructions with constant offsets: index * stride + offset
          Value *stride;
          if (m_stridedDescriptors.contains(desc)) {
            std::tie(desc, stride) = m_stridedDescriptors[desc];
          } else {
            Value *desc1 = m_builder.CreateExtractElement(desc, 1);
            // stride is 61:48 bits in descriptor, which will always be constantInt when create BufferDesc
            stride =
                m_builder.CreateAnd(m_builder.CreateLShr(desc1, m_builder.getInt32(16)), m_builder.getInt32(0x3fff));
          }
          Value *indexOffsetVal = m_builder.CreateMul(indexValue, stride);
          offsetVal = m_builder.CreateAdd(offsetVal, indexOffsetVal);
        }

        CallInst *call = m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_buffer_load, intAccessType,
                                                   {desc, offsetVal, m_builder.getInt32(coherent.u32All)});
        call->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));
        part = call;
      } else {
        if (indexValue) {
          Intrinsic::ID intrinsic = Intrinsic::amdgcn_struct_buffer_load;
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 506212
          if (ordering != AtomicOrdering::NotAtomic)
            intrinsic = Intrinsic::amdgcn_struct_atomic_buffer_load;
#endif
          part = m_builder.CreateIntrinsic(
              intAccessType, intrinsic,
              {bufferDesc, indexValue, offsetVal, m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
        } else {
          unsigned intrinsicID = Intrinsic::amdgcn_raw_buffer_load;
          if (ordering != AtomicOrdering::NotAtomic)
            intrinsicID = Intrinsic::amdgcn_raw_atomic_buffer_load;
          part = m_builder.CreateIntrinsic(
              intAccessType, intrinsicID,
              {bufferDesc, offsetVal, m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
        }
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
      if (isStridedPointer) {
        part = m_builder.CreateIntrinsic(
            m_builder.getVoidTy(), Intrinsic::amdgcn_struct_buffer_store,
            {part, bufferDesc, indexValue, offsetVal, m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
      } else {
        part = m_builder.CreateIntrinsic(
            m_builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
            {part, bufferDesc, offsetVal, m_builder.getInt32(0), m_builder.getInt32(coherent.u32All)});
      }
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
      assert(!isAnyBufferPointer(&inst));
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

// =====================================================================================================================
// Create global pointer access.
//
// @param bufferDesc: The buffer descriptor
// @param offset: The offset on the global memory
// @param strideIndex: The index of strided load
// @param type: The accessed data type
// @param inst: The instruction to be executed on the buffer
// @param callback: The callback function to perform the specific global access
Value *BufferOpLowering::createGlobalPointerAccess(Value *const bufferDesc, Value *const offset,
                                                   Value *const strideIndex, Type *const type, Instruction &inst,
                                                   const function_ref<Value *(Value *)> callback) {
  // The 2nd element (NUM_RECORDS) in the buffer descriptor is byte bound.
  Value *bound = m_builder.CreateExtractElement(bufferDesc, 2);
  Value *newOffset = offset;

  // index is for strided load which we need to handle the stride of the SRD.
  if (strideIndex || m_pipelineState.getOptions().checkRawBufferAccessDescStride) {
    Value *desc1 = m_builder.CreateExtractElement(bufferDesc, 1);
    Value *stride =
        m_builder.CreateAnd(m_builder.CreateLShr(desc1, m_builder.getInt32(16)), m_builder.getInt32(0x3fff));
    Value *byteBound = m_builder.CreateMul(bound, stride);

    if (strideIndex) {
      bound = byteBound;
      newOffset = m_builder.CreateAdd(m_builder.CreateMul(strideIndex, stride), newOffset);
    } else {
      // It is not a strided load, but it is possible that the application/client binds a strided descriptor so if
      // the stride is not zero, use bound in bytes to avoid wrong OOB check.
      stride = m_builder.CreateICmpNE(stride, m_builder.getInt32(0));
      bound = m_builder.CreateSelect(stride, byteBound, bound);
    }
  }

  Value *inBound = m_builder.CreateICmpULT(newOffset, bound);

  // If null descriptor or extended robust buffer access is allowed, we will create a branch to perform normal global
  // access based on the valid check.
  Value *isValidAccess = m_builder.getTrue();
  BasicBlock *const origBlock = inst.getParent();
  Instruction *terminator = nullptr;
  if (m_pipelineState.getOptions().allowNullDescriptor ||
      m_pipelineState.getOptions().enableExtendedRobustBufferAccess) {
    Value *isNonNullDesc = m_builder.getTrue();
    if (m_pipelineState.getOptions().allowNullDescriptor) {
      // Check dword2 against 0 for null descriptor
      isNonNullDesc = m_builder.CreateICmpNE(bound, m_builder.getInt32(0));
    }
    Value *isInBound = m_pipelineState.getOptions().enableExtendedRobustBufferAccess ? inBound : m_builder.getTrue();
    isValidAccess = m_builder.CreateAnd(isNonNullDesc, isInBound);

    terminator = SplitBlockAndInsertIfThen(isValidAccess, &inst, false);
    m_builder.SetInsertPoint(terminator);
  }
  // Global pointer access
  Value *baseAddr = getBaseAddressFromBufferDesc(bufferDesc);
  // NOTE: The offset of out-of-bound overridden as 0 may cause unexpected result when the extended robustness access
  // is disabled.
  if (!m_pipelineState.getOptions().enableExtendedRobustBufferAccess)
    newOffset = m_builder.CreateSelect(inBound, newOffset, m_builder.getInt32(0));

  // Add on the index to the address.
  Value *pointer = m_builder.CreateGEP(m_builder.getInt8Ty(), baseAddr, newOffset);
  pointer = m_builder.CreateBitCast(pointer, m_builder.getPtrTy(ADDR_SPACE_GLOBAL));
  Value *newValue = callback(pointer);

  // Store inst doesn't need return a value from a phi node
  if (!dyn_cast<StoreInst>(&inst)) {
    // Return early if the block is not split
    if (!terminator)
      return newValue;

    m_builder.SetInsertPoint(&inst);
    assert(!type->isVoidTy());
    auto phi = m_builder.CreatePHI(type, 2, "newValue");
    phi->addIncoming(Constant::getNullValue(type), origBlock);
    phi->addIncoming(newValue, terminator->getParent());
    return phi;
  }
  return nullptr;
}

// =====================================================================================================================
// Create a load from the given buffer address
//
// @param buffAddress : The buffer address
// @param forceView : Whether to force a raw buffer view
// @param isCompact : Whether to load a compact buffer
Value *BufferOpLowering::createLoadDesc(Value *buffAddress, bool forceRawView, bool isCompact) {
  Type *descTy = FixedVectorType::get(m_builder.getInt32Ty(), isCompact ? 2 : 4);
  Value *descriptor = m_builder.CreateLoad(descTy, buffAddress);
  {
    // Force convert the buffer view to raw view.
    if (forceRawView) {
      Value *desc1 = m_builder.CreateExtractElement(descriptor, 1);
      Value *desc2 = m_builder.CreateExtractElement(descriptor, 2);
      Value *desc3 = m_builder.CreateExtractElement(descriptor, 3);
      // stride is 14 bits in dword1[29:16]
      Value *stride =
          m_builder.CreateAnd(m_builder.CreateLShr(desc1, m_builder.getInt32(16)), m_builder.getInt32(0x3fff));
      stride = m_builder.CreateBinaryIntrinsic(Intrinsic::smax, stride, m_builder.getInt32(1));
      // set srd with new stride = 0 and new num_record = stride * num_record, num_record is dword2[31:0]
      descriptor =
          m_builder.CreateInsertElement(descriptor, m_builder.CreateAnd(desc1, m_builder.getInt32(0xc000ffff)), 1);
      descriptor = m_builder.CreateInsertElement(descriptor, m_builder.CreateMul(stride, desc2), 2);
      // gfx10 and gfx11 have oob fields with 2 bits in dword3[29:28] here force to set to 3 as OOB_COMPLETE mode.
      descriptor =
          m_builder.CreateInsertElement(descriptor, m_builder.CreateOr(desc3, m_builder.getInt32(0x30000000)), 3);
    }
  }
  return descriptor;
}
