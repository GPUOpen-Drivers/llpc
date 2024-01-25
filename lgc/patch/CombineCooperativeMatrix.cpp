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
 * @file  CombineCooperativeMatrix.cpp
 * @brief Pass and helpers for combining cooperative matrix operations.
 *
 * This pass is the place for combining / optimizing high-level cooperative matrix ops (@lgc.cooperative.matrix.*).
 *
 * In particular, this pass reduces the number of transpose and convert operations.
 ***********************************************************************************************************************
 */
#include "lgc/patch/CombineCooperativeMatrix.h"
#include "lgc/Builder.h"
#include "lgc/state/Defs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>

#define DEBUG_TYPE "lgc-combine-cooperative-matrix"

using namespace llvm;
using namespace lgc;

namespace {

struct Shape {
  Builder::CooperativeMatrixElementType elementType;
  Builder::CooperativeMatrixLayout layout;

  Shape(Builder::CooperativeMatrixElementType elementType_, Builder::CooperativeMatrixLayout layout_)
      : elementType(elementType_), layout(layout_) {}

  bool operator==(const Shape &rhs) const { return elementType == rhs.elementType && layout == rhs.layout; }
};

// A component of the data flow graph that starts at inputs (definitions by operations and function arguments)
// and ends at uses of the value. There are no operations inside the component, but there can be arbitrarily complex
// networks of phi nodes.
struct DataFlowComponent {
  SmallVector<Value *> inputs;
  // need to track in- and outputs of these nodes
  struct {
    SmallVector<PHINode *> phis;
    SmallVector<CallInst *> timesScalars;
    SmallVector<CallInst *> binOps;
  } inner;
  SmallVector<Use *> outputs;
  std::optional<Shape> shape;
};

class CooperativeMatrixCombiner {
public:
  CooperativeMatrixCombiner(Function &function, GfxIpVersion gfxIpVersion)
      : m_function(function), b(function.getContext()), m_gfxIpVersion(gfxIpVersion) {}

  bool run();

private:
  Shape getShapeOfTranspose(CallInst *transpose);
  void foldTo(Value *from, Value *to);
  bool tryFold(CallInst *op);
  bool tryFoldComponentContaining(Value *start);
  Instruction *findFirstUser(Instruction *instruction);
  Value *tryFoldTimesScalar(CallInst *timesScalarLo, CallInst *timesScalarHi, Value *packedMatrix);
  bool tryFoldMuladd(SmallVector<CallInst *> muladds);

  Function &m_function;
  BuilderCommon b;
  GfxIpVersion m_gfxIpVersion;
  std::vector<Instruction *> m_eraseList;
};

} // anonymous namespace

// =====================================================================================================================
// Run the combiner.
//
// @returns : True if the function was modified by the transformation and false otherwise
bool CooperativeMatrixCombiner::run() {
  LLVM_DEBUG(dbgs() << "Running the cooperative matrix combiner on " << m_function.getName() << '\n');

  bool changed = false;

  // Step 1: Collect transposes, converts and muladds
  std::vector<WeakVH> ops;
  MapVector<BasicBlock *, SmallVector<CallInst *>> muladds;

  for (Function &fn : m_function.getParent()->functions()) {
    if (!fn.isDeclaration())
      continue;

    if (fn.getName().starts_with(lgcName::CooperativeMatrixTranspose)) {
      for (User *user : fn.users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          if (call->getFunction() == &m_function)
            ops.push_back(call);
        }
      }
    } else if (fn.getName().starts_with(lgcName::CooperativeMatrixConvert)) {
      for (User *user : fn.users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          if (call->getFunction() == &m_function)
            ops.push_back(call);
        }
      }
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 479080
      // wmma packing on gfx11 only possible with new wmma_f16_tied intrinsic
    } else if (m_gfxIpVersion.major == 11 && fn.getName().starts_with(lgcName::CooperativeMatrixMulAdd)) {
      for (User *user : fn.users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          Builder::CooperativeMatrixElementType accumElemType = static_cast<Builder::CooperativeMatrixElementType>(
              cast<ConstantInt>(call->getOperand(7))->getZExtValue());
          bool isPackable = accumElemType == Builder::CooperativeMatrixElementType::Float16;
          if (call->getFunction() == &m_function && isPackable) {
            muladds[call->getParent()].push_back(call);
          }
        }
      }
#endif
    }
  }

  // Step 2: Attempt folds.
  for (const WeakVH &handle : ops) {
    auto *op = cast_or_null<CallInst>(handle);
    if (!op)
      continue;

    if (tryFold(op)) {
      changed = true;

      for (Instruction *inst : llvm::reverse(m_eraseList)) {
        if (inst->use_empty())
          inst->eraseFromParent();
      }
      m_eraseList.clear();
    }
  }
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 479080
  // wmma packing on gfx11 only possible with new wmma_f16_tied intrinsic
  for (auto muladdsPerBB : muladds) {
    changed |= tryFoldMuladd(std::move(muladdsPerBB.second));

    for (Instruction *inst : llvm::reverse(m_eraseList)) {
      if (inst->use_empty())
        inst->eraseFromParent();
    }
    m_eraseList.clear();
  }

  muladds.clear();
#endif

  ops.clear();

  return changed;
}

// =====================================================================================================================
// Determine the shape of the given transpose operation.
//
// @param [in] transpose : the transpose operation
// @returns : the cooperative matrix shape
Shape CooperativeMatrixCombiner::getShapeOfTranspose(CallInst *transpose) {
  unsigned elemType = cast<ConstantInt>(transpose->getArgOperand(1))->getZExtValue();
  unsigned layout = cast<ConstantInt>(transpose->getArgOperand(2))->getZExtValue();
  return {(Builder::CooperativeMatrixElementType)elemType, (Builder::CooperativeMatrixLayout)layout};
}

// =====================================================================================================================
// Replace all uses of @p from with @p to.
//
// This method queues @p from for possible deletion, but will _not_ delete it immediately. Deletion is deferred to the
// main combiner loop.
//
// Note: This is a separate method since we may eventually add related operations back to a worklist for iterative
// folding, but this is currently not implemented.
//
// @param [in] from : the value to be replaced
// @param [out] to : the replacement value
void CooperativeMatrixCombiner::foldTo(Value *from, Value *to) {
  from->replaceAllUsesWith(to);

  if (auto *fromInst = dyn_cast<Instruction>(from)) {
    m_eraseList.push_back(fromInst);
  }
}

// =====================================================================================================================
// Try to fold / combine around a given transpose or convert operation.
//
// @param [in] op : the operation to try to fold
// @returns : whether a change was made
bool CooperativeMatrixCombiner::tryFold(CallInst *op) {
  Value *src;
  bool isConvert = false;
  if (op->getCalledFunction()->getName().starts_with(lgcName::CooperativeMatrixConvert)) {
    src = op->getArgOperand(1);
    isConvert = true;
  } else {
    assert(op->getCalledFunction()->getName().starts_with(lgcName::CooperativeMatrixTranspose));
    src = op->getArgOperand(0);
  }

  if (auto *constant = dyn_cast<Constant>(src)) {
    if (isa<PoisonValue>(constant)) {
      // tranpose/convert(poison) -> poison
      foldTo(op, PoisonValue::get(op->getType()));
      return true;
    }
    if (isa<UndefValue>(constant)) {
      // transpose/convert(undef) -> undef, if legal
      bool isFoldable = true;
      if (isConvert) {
        auto srcElementType =
            (Builder::CooperativeMatrixElementType)cast<ConstantInt>(op->getArgOperand(2))->getZExtValue();
        auto dstElementType =
            (Builder::CooperativeMatrixElementType)cast<ConstantInt>(op->getArgOperand(3))->getZExtValue();
        if (srcElementType != dstElementType) {
          // This is slightly conservative, but the point here is that e.g. `zext undef(i16) to i32` can't be folded
          // to undef because the result can't truly take all possible bit patterns.
          isFoldable = false;
        }
      }

      if (isFoldable) {
        foldTo(op, UndefValue::get(op->getType()));
        return true;
      }
    }
    if (constant->isNullValue()) {
      // transpose/convert(zeroinitializer) -> zeroinitializer
      foldTo(op, Constant::getNullValue(op->getType()));
      return true;
    }
  } else if (auto *inst = dyn_cast<Instruction>(src)) {
    if (tryFoldComponentContaining(inst))
      return true;
  }

  if (tryFoldComponentContaining(op))
    return true;

  return false;
}

// =====================================================================================================================
// Discover the data flow component involving @p start and try to fold it.
//
// @param [in] start : the starting value for component discovery
// @returns : whether a change was made
bool CooperativeMatrixCombiner::tryFoldComponentContaining(Value *start) {
  LLVM_DEBUG(dbgs() << "tryFoldComponentContaining: " << *start << '\n');

  assert(!isa<Constant>(start));

  // Step 1: Discover the component
  DataFlowComponent component;
  SmallVector<Value *> worklistForward;
  SmallVector<Value *> worklistBackward;

  auto foundInner = [&](Value *val) {
    if (auto *phi = dyn_cast<PHINode>(val)) {
      if (is_contained(component.inner.phis, phi))
        return true;

      component.inner.phis.push_back(phi);
      worklistForward.push_back(phi);
      for (Value *incoming : phi->incoming_values()) {
        worklistBackward.push_back(incoming);
      }
      return true;
    }
    if (auto *call = dyn_cast<CallInst>(val)) {
      if (auto *callee = call->getCalledFunction()) {
        if (callee->getName().starts_with(lgcName::CooperativeMatrixTimesScalar)) {
          if (is_contained(component.inner.timesScalars, call))
            return true;

          component.inner.timesScalars.push_back(call);
          worklistForward.push_back(call);
          worklistBackward.push_back(call->getArgOperand(0));
          return true;
        }
        if (callee->getName().starts_with(lgcName::CooperativeMatrixBinOp)) {
          if (is_contained(component.inner.binOps, call))
            return true;

          component.inner.binOps.push_back(call);
          worklistForward.push_back(call);
          worklistBackward.push_back(call->getArgOperand(1));
          worklistBackward.push_back(call->getArgOperand(2));
          return true;
        }
        return false;
      }
    }
    return false;
  };

  if (!foundInner(start)) {
    component.inputs.push_back(start);
    worklistForward.push_back(start);
  }

  do {
    Value *current = worklistForward.pop_back_val();

    for (Use &use : current->uses()) {
      if (!foundInner(use.getUser())) {
        component.outputs.push_back(&use);
      }
    }

    while (!worklistBackward.empty()) {
      Value *incoming = worklistBackward.pop_back_val();
      if (is_contained(component.inputs, incoming))
        continue;
      if (foundInner(incoming))
        continue;
      if (!isa<Constant>(incoming)) {
        component.inputs.push_back(incoming);
        worklistForward.push_back(incoming);
      }
    }
  } while (!worklistForward.empty());

  // Step 2: Analyze the inputs and outputs.
  std::optional<Builder::CooperativeMatrixLayout> otherLayout;
  Type *otherType = nullptr;
  unsigned numUnhandledInputs = 0;
  unsigned numTransposeInputs = 0;
  unsigned numRelayoutInputs = 0;
  DenseSet<Value *> unhandledOutputs;
  DenseSet<Value *> transposeOutputs;
  DenseSet<Value *> relayoutOutputs;

  auto foundComponentShape = [&](Shape shape) {
    if (!component.shape)
      component.shape = shape;
    else
      assert(*component.shape == shape);
  };

  auto foundOtherLayout = [&](Builder::CooperativeMatrixLayout layout, Type *type) {
    if (!otherLayout) {
      otherLayout = layout;
      otherType = type;
    } else {
      assert(*otherLayout == layout);
      assert(otherType == type);
    }
  };

  for (Value *input : component.inputs) {
    if (auto *constant = dyn_cast<Constant>(input)) {
      if (!constant->isNullValue() && !isa<UndefValue>(constant) && !isa<PoisonValue>(constant)) {
        // We could try to rewrite other constants, or insert transpose/convert operations as required, but we're
        // quite unlikely to encounter this in the first place, so let's not bother with the complexity.
        LLVM_DEBUG(dbgs() << "  bail out due to unhandled constant: " << *input << '\n');
        return false;
      }

      continue;
    }

    if (auto *call = dyn_cast<CallInst>(input)) {
      if (auto *callee = call->getCalledFunction()) {
        if (callee->getName().starts_with(lgcName::CooperativeMatrixLoad))
          continue; // loads can be adjusted at zero cost
        if (callee->getName().starts_with(lgcName::CooperativeMatrixTranspose)) {
          foundComponentShape(getShapeOfTranspose(call));
          ++numTransposeInputs;
          continue;
        }
        if (callee->getName().starts_with(lgcName::CooperativeMatrixConvert)) {
          auto srcElemType =
              (Builder::CooperativeMatrixElementType)cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
          auto dstElemType =
              (Builder::CooperativeMatrixElementType)cast<ConstantInt>(call->getArgOperand(3))->getZExtValue();
          if (srcElemType != dstElemType) {
            LLVM_DEBUG(dbgs() << "  unhandled element type input conversion: " << *call << '\n');
            ++numUnhandledInputs;
            continue;
          }

          auto srcLayout = (Builder::CooperativeMatrixLayout)cast<ConstantInt>(call->getArgOperand(4))->getZExtValue();
          auto dstLayout = (Builder::CooperativeMatrixLayout)cast<ConstantInt>(call->getArgOperand(5))->getZExtValue();
          foundComponentShape({dstElemType, dstLayout});
          foundOtherLayout(srcLayout, call->getArgOperand(1)->getType());

          ++numRelayoutInputs;
          continue;
        }
      }
      ++numUnhandledInputs;
      continue;
    }

    ++numUnhandledInputs;
  }

  for (Use *use : component.outputs) {
    if (auto *call = dyn_cast<CallInst>(use->getUser())) {
      if (auto *callee = call->getCalledFunction()) {
        if (callee->getName().starts_with(lgcName::CooperativeMatrixStore))
          continue; // stores can be adapted at zero cost
        if (callee->getName().starts_with(lgcName::CooperativeMatrixTranspose)) {
          foundComponentShape(getShapeOfTranspose(call));
          transposeOutputs.insert(use->get());
          continue;
        }
        if (callee->getName().starts_with(lgcName::CooperativeMatrixConvert)) {
          auto srcElemType =
              (Builder::CooperativeMatrixElementType)cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
          auto dstElemType =
              (Builder::CooperativeMatrixElementType)cast<ConstantInt>(call->getArgOperand(3))->getZExtValue();
          if (srcElemType != dstElemType) {
            LLVM_DEBUG(dbgs() << "  unhandled element type output conversion: " << *call << '\n');
            ++numUnhandledInputs;
            continue;
          }

          auto srcLayout = (Builder::CooperativeMatrixLayout)cast<ConstantInt>(call->getArgOperand(4))->getZExtValue();
          auto dstLayout = (Builder::CooperativeMatrixLayout)cast<ConstantInt>(call->getArgOperand(5))->getZExtValue();
          foundComponentShape({srcElemType, srcLayout});
          foundOtherLayout(dstLayout, call->getType());

          relayoutOutputs.insert(use->get());
          continue;
        }
      }
    }

    unhandledOutputs.insert(use->get());
  }

  // Step 3: Transpose the component if that is beneficial.
  int transposeCost = -(numTransposeInputs + transposeOutputs.size());
  transposeCost += numUnhandledInputs + numRelayoutInputs + unhandledOutputs.size() + relayoutOutputs.size();

  LLVM_DEBUG(dbgs() << "  transpose cost delta: " << transposeCost << '\n');

  if (transposeCost < 0) {
    // Cache for newly inserted transpose operations.
    DenseMap<Value *, Value *> outTransposed;

    for (Value *input : component.inputs) {
      // Handle inputs that can be folded away / absorbed.
      if (auto *call = dyn_cast<CallInst>(input)) {
        if (auto *callee = call->getCalledFunction()) {
          if (callee->getName().starts_with(lgcName::CooperativeMatrixTranspose)) {
            Value *src = call->getArgOperand(0);
            foldTo(input, src);

            // Prepopulate the transpose cache to re-use the old transpose operation instead of creating a new one.
            outTransposed.try_emplace(src, input);
            continue;
          }
          if (callee->getName().starts_with(lgcName::CooperativeMatrixLoad)) {
            bool colMajor = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
            call->setArgOperand(2, b.getInt1(!colMajor));
            continue;
          }
        }
      }

      // Handle generic inputs that need to be transposed explicitly.
      if (auto *inst = dyn_cast<Instruction>(input)) {
        b.SetInsertPoint(inst->getNextNode());
      } else {
        assert(isa<Argument>(input));
        b.SetInsertPointPastAllocas(&m_function);
      }

      auto *transposed = b.CreateCooperativeMatrixTranspose(PoisonValue::get(input->getType()),
                                                            component.shape->elementType, component.shape->layout);
      foldTo(input, transposed);
      transposed->setArgOperand(0, input);
    }

    for (Use *use : component.outputs) {
      // Handle outputs that can be folded away / absorbed.
      if (auto *call = dyn_cast<CallInst>(use->getUser())) {
        if (auto *callee = call->getCalledFunction()) {
          if (callee->getName().starts_with(lgcName::CooperativeMatrixTranspose)) {
            foldTo(call, use->get());
            continue;
          }
          if (callee->getName().starts_with(lgcName::CooperativeMatrixStore)) {
            bool colMajor = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
            call->setArgOperand(2, b.getInt1(!colMajor));
            continue;
          }
        }
      }

      // Handle generic outputs that need to be transposed explicitly.
      Value *&transposed = outTransposed[use->get()];
      if (!transposed) {
        if (auto *phi = dyn_cast<PHINode>(use->get())) {
          b.SetInsertPoint(phi->getParent(), phi->getParent()->getFirstInsertionPt());
        } else {
          auto *def = cast<Instruction>(use->get());
          b.SetInsertPoint(def->getNextNode());
        }

        transposed =
            b.CreateCooperativeMatrixTranspose(use->get(), component.shape->elementType, component.shape->layout);
      }

      use->set(transposed);
    }

    return true;
  }

  // Step 4: Otherwise, relayout the component if that is beneficial.
  int relayoutCost = -(numRelayoutInputs + relayoutOutputs.size());
  relayoutCost += numUnhandledInputs + numTransposeInputs + unhandledOutputs.size() + transposeOutputs.size();

  LLVM_DEBUG(dbgs() << "  relayout cost delta: " << relayoutCost << '\n');

  if (relayoutCost < 0) {
    // Cache for newly inserted relayout convert operations.
    DenseMap<Value *, Value *> outRelayouted;

    // Force-override inner nodes if necessary
    if (!component.inner.phis.empty() && component.inner.phis[0]->getType() != otherType) {
      for (PHINode *phi : component.inner.phis) {
        phi->mutateType(otherType);

        for (Use &use : phi->incoming_values()) {
          if (auto *constant = dyn_cast<Constant>(use.get())) {
            if (constant->isNullValue()) {
              use.set(Constant::getNullValue(otherType));
            } else if (isa<UndefValue>(constant)) {
              use.set(UndefValue::get(otherType));
            } else if (isa<PoisonValue>(constant)) {
              use.set(PoisonValue::get(otherType));
            } else {
              // We should have bailed out earlier in this case.
              llvm_unreachable("unhandled constant in cooperative matrix phi");
            }
          }
        }
      }
    }

    for (CallInst *timesScalar : component.inner.timesScalars) {
      timesScalar->mutateType(otherType);
      timesScalar->setArgOperand(3, b.getInt32((unsigned)*otherLayout));
      continue;
    }

    for (CallInst *binOp : component.inner.binOps) {
      binOp->mutateType(otherType);
      binOp->setArgOperand(4, b.getInt32((unsigned)*otherLayout));
      continue;
    }

    for (Value *input : component.inputs) {
      // Handle inputs for which the relayout can be folded or absorbed.
      if (auto *call = dyn_cast<CallInst>(input)) {
        if (auto *callee = call->getCalledFunction()) {
          if (callee->getName().starts_with(lgcName::CooperativeMatrixConvert)) {
            unsigned srcElemType = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
            unsigned dstElemType = cast<ConstantInt>(call->getArgOperand(3))->getZExtValue();

            if (srcElemType == dstElemType) {
              unsigned srcLayout =
                  (Builder::CooperativeMatrixLayout)cast<ConstantInt>(call->getArgOperand(4))->getZExtValue();
              assert(srcLayout == *otherLayout);
              (void(srcLayout)); // unused

              Value *src = call->getArgOperand(1);
              foldTo(input, src);

              // Pre-populate the cache to re-use the relayout operation instead of creating a new one.
              outRelayouted.try_emplace(src, input);
              continue;
            }

            // Integrate the relayouting into a merged conversion op.
            call->setArgOperand(5, b.getInt32((unsigned)*otherLayout));
            continue;
          }
          if (callee->getName().starts_with(lgcName::CooperativeMatrixLoad)) {
            call->setArgOperand(4, b.getInt32((unsigned)*otherLayout));
            continue;
          }
        }
      }

      // Handle generic inputs that need a new convert operation inserted.
      if (auto *inst = dyn_cast<Instruction>(input)) {
        b.SetInsertPoint(inst->getNextNode());
      } else {
        assert(isa<Argument>(input));
        b.SetInsertPointPastAllocas(&m_function);
      }

      CallInst *convert = b.CreateCooperativeMatrixConvert((CastInst::CastOps)0, PoisonValue::get(input->getType()),
                                                           component.shape->elementType, component.shape->elementType,
                                                           component.shape->layout, *otherLayout);
      foldTo(input, convert);
      convert->setArgOperand(1, input);
    }

    for (Use *use : component.outputs) {
      // Handle outputs for which the relayout can be folded or absorbed.
      if (auto *call = dyn_cast<CallInst>(use->getUser())) {
        if (auto *callee = call->getCalledFunction()) {
          if (callee->getName().starts_with(lgcName::CooperativeMatrixConvert)) {
            unsigned srcElemType = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
            unsigned dstElemType = cast<ConstantInt>(call->getArgOperand(3))->getZExtValue();

            if (srcElemType == dstElemType) {
              unsigned dstLayout =
                  (Builder::CooperativeMatrixLayout)cast<ConstantInt>(call->getArgOperand(5))->getZExtValue();
              assert(dstLayout == *otherLayout);
              (void(dstLayout)); // unused

              foldTo(call, use->get());
              continue;
            }
          }
          if (callee->getName().starts_with(lgcName::CooperativeMatrixStore)) {
            call->setArgOperand(4, b.getInt32((unsigned)*otherLayout));
            continue;
          }
        }
      }

      // Handle generic outputs that need a new convert operation inserted.
      Value *&relayouted = outRelayouted[use->get()];
      if (!relayouted) {
        if (auto *phi = dyn_cast<PHINode>(use->get())) {
          b.SetInsertPoint(phi->getParent(), phi->getParent()->getFirstInsertionPt());
        } else {
          auto *def = cast<Instruction>(use->get());
          b.SetInsertPoint(def->getNextNode());
        }

        relayouted =
            b.CreateCooperativeMatrixConvert((CastInst::CastOps)0, use->get(), component.shape->elementType,
                                             component.shape->elementType, *otherLayout, component.shape->layout);
      }

      use->set(relayouted);
    }

    return true;
  }

  return false;
}

Instruction *CooperativeMatrixCombiner::findFirstUser(Instruction *instruction) {
  Instruction *earliestUser = nullptr;
  for (auto *user : instruction->users()) {
    auto *userInst = dyn_cast<Instruction>(user);
    // We only pack instructions inside the same basic block.
    // Therefore, users outside the BB don't interfere
    if (instruction->getParent() != userInst->getParent())
      continue;

    if (dyn_cast<PHINode>(userInst))
      continue;

    if (!earliestUser || userInst->comesBefore(earliestUser))
      earliestUser = userInst;
  }
  return earliestUser;
}

bool CooperativeMatrixCombiner::tryFoldMuladd(SmallVector<CallInst *> muladds) {
  bool changed = false;

  auto cmp = [](CallInst *a, CallInst *b) { return b->comesBefore(a); };
  stable_sort(muladds, cmp);

  do {
    auto *muladdLo = muladds.pop_back_val();
    auto *packInsertPoint = cast<Instruction>(muladdLo);

    struct PackingComponents {
      Value *matrixLo;
      Value *matrixHi;
      Value *packedAccum;
    };
    SmallVector<PackingComponents> worklist;
    SmallVector<std::pair<Use &, bool>> unpackedUses;
    SmallVector<CallInst *> muladdChain;

    auto *matCLo = muladdLo->getArgOperand(2);

    muladdChain.push_back(muladdLo);
    muladdLo->setArgOperand(5, b.getInt1(false));
    while (muladdLo->hasOneUse()) {
      auto *next = dyn_cast<CallInst>(*muladdLo->users().begin());

      if (!is_contained(muladds, next))
        break;

      next->setArgOperand(5, b.getInt1(false));
      muladdChain.push_back(next);
      muladdLo = next;
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 478769
      llvm::erase_value(muladds, muladdLo);
#else
      llvm::erase(muladds, muladdLo);
#endif
    }

    Instruction *firstLoUser = findFirstUser(muladdLo);

    CallInst *muladdHi = nullptr;
    for (auto *candidate : llvm::reverse(muladds)) {
      if (firstLoUser && firstLoUser->comesBefore(candidate))
        continue;

      if (auto *matCHi = dyn_cast<Instruction>(candidate->getArgOperand(2))) {
        if (matCHi->getParent() == muladdLo->getParent() && packInsertPoint->comesBefore(matCHi)) {
          continue;
        }
      }

      muladdHi = candidate;
      break;
    }

    if (!muladdHi)
      continue;

    auto *matCHi = muladdHi->getArgOperand(2);

    muladdChain.push_back(muladdHi);
    muladdHi->setArgOperand(5, b.getInt1(true));
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 478769
    llvm::erase_value(muladds, muladdLo);
#else
    llvm::erase(muladds, muladdHi);
#endif
    while (muladdHi->hasOneUse()) {
      auto *next = dyn_cast<CallInst>(*muladdHi->users().begin());
      if (!is_contained(muladds, next)) {
        break;
      }

      if (firstLoUser && firstLoUser->comesBefore(next)) {
        break;
      }
      next->setArgOperand(5, b.getInt1(true));
      muladdChain.push_back(next);
      muladdHi = next;
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 478769
      llvm::erase_value(muladds, muladdLo);
#else
      llvm::erase(muladds, next);
#endif
    }

    auto cmp = [&](CallInst *a, CallInst *b) { return a->comesBefore(b); };
    stable_sort(muladdChain, cmp);

    // if we have a loop, then the accumulator matrices come from the phi nodes.
    // in that case, we need to pack in the predecessor blocks, so all the
    // incoming values are packed accumulators.
    PHINode *const phiLo = dyn_cast<PHINode>(matCLo);
    PHINode *const phiHi = dyn_cast<PHINode>(matCHi);
    Value *curAccum = nullptr;
    if (phiLo && phiHi && phiLo->getParent() == phiHi->getParent()) {
      for (BasicBlock *incoming : phiLo->blocks()) {
        b.SetInsertPoint(incoming->getTerminator());
        auto *matCLo = phiLo->getIncomingValueForBlock(incoming);
        auto *matCHi = phiHi->getIncomingValueForBlock(incoming);
        auto *packed = b.CreateCooperativeMatrixPack(matCLo, matCHi);
        phiLo->setIncomingValueForBlock(incoming, packed);
        phiHi->setIncomingValueForBlock(incoming, packed);
      }
      curAccum = phiHi;
      worklist.push_back({phiLo, phiHi, phiHi});
    } else {
      // otherwise, we pack just before the first muladd
      b.SetInsertPoint(packInsertPoint);
      curAccum = b.CreateCooperativeMatrixPack(matCLo, matCHi);
    }

    for (auto *next : muladdChain) {
      next->setArgOperand(2, curAccum);
      next->setArgOperand(6, b.getInt1(true));
      curAccum = next;
    }

    worklist.push_back({muladdLo, muladdHi, curAccum});
    while (!worklist.empty()) {
      auto current = worklist.pop_back_val();

      for (Use &use : current.matrixLo->uses()) {
        if (is_contained(muladdChain, use.getUser()))
          continue;

        unpackedUses.push_back({use, false});
      }
      for (Use &use : current.matrixHi->uses()) {
        if (is_contained(muladdChain, use.getUser()))
          continue;

        if (auto *call = dyn_cast<CallInst>(use.getUser())) {
          if (auto *callee = call->getCalledFunction()) {
            if (callee->getName().starts_with(lgcName::CooperativeMatrixTimesScalar)) {
              auto *candidate = llvm::find_if(unpackedUses, [&](auto pair) {
                if (auto *call = dyn_cast<CallInst>(pair.first.getUser())) {
                  if (auto *callee = call->getCalledFunction()) {
                    if (callee->getName().starts_with(lgcName::CooperativeMatrixTimesScalar) &&
                        call->getArgOperand(0) == current.matrixLo) {
                      return true;
                    }
                  }
                }
                return false;
              });

              if (candidate == unpackedUses.end()) {
                unpackedUses.push_back({use, true});
                continue;
              }

              auto *timesScalarLo = cast<CallInst>(candidate->first.getUser());
              auto *timesScalarHi = call;
              auto *timesScalarPacked = tryFoldTimesScalar(timesScalarLo, timesScalarHi, current.packedAccum);

              if (timesScalarPacked) {
                worklist.push_back({timesScalarLo, timesScalarHi, timesScalarPacked});
                continue;
              }
            }
          }
        }

        unpackedUses.push_back({use, true});
      }

      for (auto use : unpackedUses) {
        if (is_contained(m_eraseList, use.first.getUser()))
          continue;

        if (auto *call = dyn_cast<CallInst>(use.first.getUser())) {
          if (call->getCalledFunction()->getName().starts_with(lgcName::CooperativeMatrixPack) &&
              call->getArgOperand(0) == current.matrixLo && call->getArgOperand(1) == current.matrixHi) {
            foldTo(call, current.packedAccum);
            continue;
          }
        }

        if (auto *phi = dyn_cast<PHINode>(use.first.getUser())) {
          auto *predecessor = phi->getIncomingBlock(use.first);
          b.SetInsertPoint(predecessor->getTerminator());
        } else {
          b.SetInsertPoint(cast<Instruction>(use.first.getUser()));
        }
        auto unpacked = b.CreateCooperativeMatrixUnpack(current.packedAccum, use.second);
        use.first.set(unpacked);
      }
      unpackedUses.clear();
    }
    if (phiLo && phiHi)
      foldTo(phiLo, phiHi);

    changed = true;
  } while (!muladds.empty());

  return changed;
}

Value *CooperativeMatrixCombiner::tryFoldTimesScalar(CallInst *timesScalarLo, CallInst *timesScalarHi,
                                                     Value *packedMatrix) {
  if (timesScalarLo->getParent() != timesScalarHi->getParent()) {
    return nullptr;
  }

  auto *earlierInst = timesScalarLo->comesBefore(timesScalarHi) ? timesScalarLo : timesScalarHi;
  auto *laterInst = earlierInst == timesScalarLo ? timesScalarHi : timesScalarLo;
  auto *earliestUser = findFirstUser(earlierInst);

  if (earliestUser && earliestUser->comesBefore(laterInst)) {
    return nullptr;
  }

  b.SetInsertPoint(laterInst);

  auto *scalarVec = b.CreateVectorSplat(2, PoisonValue::get(b.getHalfTy()));
  scalarVec = b.CreateInsertElement(scalarVec, timesScalarLo->getArgOperand(1), b.getInt32(0));
  scalarVec = b.CreateInsertElement(scalarVec, timesScalarHi->getArgOperand(1), b.getInt32(1));
  auto *timesScalarPacked =
      b.CreateCoopMatrixTimesScalar(packedMatrix, scalarVec, Builder::CooperativeMatrixElementType::Float16Packed,
                                    Builder::CooperativeMatrixLayout::AccumulatorMatrixLayout);
  m_eraseList.push_back(timesScalarLo);
  m_eraseList.push_back(timesScalarHi);
  return timesScalarPacked;
}

// =====================================================================================================================
// Run the pass on a function.
//
// @param [in/out] function :  LLVM function to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The Analyses that are still valid after this pass)
PreservedAnalyses CombineCooperativeMatrix::run(Function &function, FunctionAnalysisManager &analysisManager) {
  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();
  CooperativeMatrixCombiner combiner{function, pipelineState->getTargetInfo().getGfxIpVersion()};

  if (combiner.run()) {
    PreservedAnalyses PA;
    PA.preserveSet<CFGAnalyses>();
    return PA;
  }
  return PreservedAnalyses::all();
}
