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
  SmallVector<PHINode *> phis;
  SmallVector<Use *> outputs;
  std::optional<Shape> shape;
};

class CooperativeMatrixCombiner {
public:
  CooperativeMatrixCombiner(Function &function) : m_function(function), b(function.getContext()) {}

  bool run();

private:
  Shape getShapeOfTranspose(CallInst *transpose);
  void foldTo(Value *from, Value *to);
  bool tryFold(CallInst *op);
  bool tryFoldComponentContaining(Value *start);

  Function &m_function;
  BuilderCommon b;
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

  // Step 1: Collect transposes and converts
  std::vector<WeakVH> ops;

  for (Function &fn : m_function.getParent()->functions()) {
    if (!fn.isDeclaration())
      continue;

    if (fn.getName().startswith(lgcName::CooperativeMatrixTranspose)) {
      for (User *user : fn.users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          if (call->getFunction() == &m_function)
            ops.push_back(call);
        }
      }
    } else if (fn.getName().startswith(lgcName::CooperativeMatrixConvert)) {
      for (User *user : fn.users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          if (call->getFunction() == &m_function)
            ops.push_back(call);
        }
      }
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

  if (auto *fromInst = dyn_cast<Instruction>(from))
    m_eraseList.push_back(fromInst);
}

// =====================================================================================================================
// Try to fold / combine around a given transpose or convert operation.
//
// @param [in] op : the operation to try to fold
// @returns : whether a change was made
bool CooperativeMatrixCombiner::tryFold(CallInst *op) {
  Value *src;
  bool isConvert = false;
  if (op->getCalledFunction()->getName().startswith(lgcName::CooperativeMatrixConvert)) {
    src = op->getArgOperand(1);
    isConvert = true;
  } else {
    assert(op->getCalledFunction()->getName().startswith(lgcName::CooperativeMatrixTranspose));
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
  SmallVector<Value *> worklist;

  if (auto *phi = dyn_cast<PHINode>(start))
    component.phis.push_back(phi);
  else
    component.inputs.push_back(start);
  worklist.push_back(start);

  do {
    Value *current = worklist.pop_back_val();

    auto foundPhi = [&](PHINode *phi) {
      if (llvm::any_of(component.phis, [=](auto elem) { return elem == phi; }))
        return;
      component.phis.push_back(phi);
      worklist.push_back(phi);
    };

    for (Use &use : current->uses()) {
      if (auto *phi = dyn_cast<PHINode>(use.getUser())) {
        foundPhi(phi);
        continue;
      }

      component.outputs.push_back(&use);
    }

    if (auto *phi = dyn_cast<PHINode>(current)) {
      for (Value *incoming : phi->incoming_values()) {
        if (auto *parentPhi = dyn_cast<PHINode>(incoming)) {
          foundPhi(parentPhi);
        } else {
          if (llvm::any_of(component.inputs, [=](auto elem) { return elem == incoming; }))
            continue;
          if (!isa<Constant>(incoming)) {
            component.inputs.push_back(incoming);
            worklist.push_back(incoming);
          }
        }
      }
    }
  } while (!worklist.empty());

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
        if (callee->getName().startswith(lgcName::CooperativeMatrixLoad))
          continue; // loads can be adjusted at zero cost
        if (callee->getName().startswith(lgcName::CooperativeMatrixTranspose)) {
          foundComponentShape(getShapeOfTranspose(call));
          ++numTransposeInputs;
          continue;
        }
        if (callee->getName().startswith(lgcName::CooperativeMatrixConvert)) {
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
        if (callee->getName().startswith(lgcName::CooperativeMatrixStore))
          continue; // stores can be adapted at zero cost
        if (callee->getName().startswith(lgcName::CooperativeMatrixTranspose)) {
          foundComponentShape(getShapeOfTranspose(call));
          transposeOutputs.insert(use->get());
          continue;
        }
        if (callee->getName().startswith(lgcName::CooperativeMatrixConvert)) {
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
          if (callee->getName().startswith(lgcName::CooperativeMatrixTranspose)) {
            Value *src = call->getArgOperand(0);
            foldTo(input, src);

            // Prepopulate the transpose cache to re-use the old transpose operation instead of creating a new one.
            outTransposed.try_emplace(src, input);
            continue;
          }
          if (callee->getName().startswith(lgcName::CooperativeMatrixLoad)) {
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
          if (callee->getName().startswith(lgcName::CooperativeMatrixTranspose)) {
            foldTo(call, use->get());
            continue;
          }
          if (callee->getName().startswith(lgcName::CooperativeMatrixStore)) {
            bool colMajor = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
            call->setArgOperand(2, b.getInt1(!colMajor));
            continue;
          }
        }
      }

      // Handle generic outputs that need to be transposed explicitly.
      Value *&transposed = outTransposed[use->get()];
      if (!transposed) {
        if (auto *phi = cast<PHINode>(use->get())) {
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

    // Force-override phi types if necessary
    if (!component.phis.empty() && component.phis[0]->getType() != otherType) {
      for (PHINode *phi : component.phis) {
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

    for (Value *input : component.inputs) {
      // Handle inputs for which the relayout can be folded or absorbed.
      if (auto *call = dyn_cast<CallInst>(input)) {
        if (auto *callee = call->getCalledFunction()) {
          if (callee->getName().startswith(lgcName::CooperativeMatrixConvert)) {
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
          if (callee->getName().startswith(lgcName::CooperativeMatrixLoad)) {
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
          if (callee->getName().startswith(lgcName::CooperativeMatrixConvert)) {
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
          if (callee->getName().startswith(lgcName::CooperativeMatrixStore)) {
            call->setArgOperand(4, b.getInt32((unsigned)*otherLayout));
            continue;
          }
        }
      }

      // Handle generic outputs that need a new convert operation inserted.
      Value *&relayouted = outRelayouted[use->get()];
      if (!relayouted) {
        if (auto *phi = cast<PHINode>(use->get())) {
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

// =====================================================================================================================
// Run the pass on a function.
//
// @param [in/out] function :  LLVM function to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The Analyses that are still valid after this pass)
PreservedAnalyses CombineCooperativeMatrix::run(Function &function, FunctionAnalysisManager &analysisManager) {
  CooperativeMatrixCombiner combiner{function};

  if (combiner.run()) {
    PreservedAnalyses PA;
    PA.preserveSet<CFGAnalyses>();
    return PA;
  }
  return PreservedAnalyses::all();
}
