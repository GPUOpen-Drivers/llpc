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
#include "lgc/LgcDialect.h"
#include "lgc/state/Defs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>

#define DEBUG_TYPE "lgc-combine-cooperative-matrix"

using namespace llvm;
using namespace lgc;

namespace lgc {

class CooperativeMatrixConvertOp;
class CooperativeMatrixTransposeOp;
class CooperativeMatrixMulAddOp;

struct Shape {
  CooperativeMatrixElementType elementType;
  CooperativeMatrixLayout layout;

  Shape(CooperativeMatrixElementType elementType_, CooperativeMatrixLayout layout_)
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
  Shape getShapeOfTranspose(CooperativeMatrixTransposeOp &transpose);
  void foldTo(Value *from, Value *to);
  bool tryFold(CallInst *op);
  bool tryFoldComponentContaining(Value *start);
  Instruction *findFirstUser(Instruction *instruction);
  Value *tryFoldTimesScalar(CallInst *timesScalarLo, CallInst *timesScalarHi, Value *packedMatrix);
  bool tryFoldMuladd(SmallVector<CooperativeMatrixMulAddOp *> muladds);

  Function &m_function;
  BuilderCommon b;
  GfxIpVersion m_gfxIpVersion;
  std::vector<Instruction *> m_eraseList;
  std::vector<WeakVH> m_ops;
  MapVector<BasicBlock *, SmallVector<CooperativeMatrixMulAddOp *>> m_muladds;
};

// =====================================================================================================================
// Run the combiner.
//
// @returns : True if the function was modified by the transformation and false otherwise
bool CooperativeMatrixCombiner::run() {
  LLVM_DEBUG(dbgs() << "Running the cooperative matrix combiner on " << m_function.getName() << '\n');

  bool changed = false;

  // Step 1: Collect transposes, converts and muladds
  static const auto visitor = llvm_dialects::VisitorBuilder<CooperativeMatrixCombiner>()
                                  .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                  .addSet<CooperativeMatrixConvertOp, CooperativeMatrixTransposeOp>(
                                      [](auto &self, auto &op) { self.m_ops.push_back(&op); })
                                  .add<CooperativeMatrixMulAddOp>([](auto &self, auto &op) {
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 479080
                                    auto accumElemType = op.getAccuElemType();
                                    bool isPackable = accumElemType == CooperativeMatrixElementType::Float16;
                                    if ((self.m_gfxIpVersion.major == 11) && isPackable) {
                                      self.m_muladds[op.getParent()].push_back(&op);
                                    }
#endif
                                  })
                                  .build();
  visitor.visit(*this, m_function);

  // Step 2: Attempt folds.
  for (const WeakVH &handle : m_ops) {
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
  for (auto muladdsPerBB : m_muladds) {
    changed |= tryFoldMuladd(std::move(muladdsPerBB.second));

    for (Instruction *inst : llvm::reverse(m_eraseList)) {
      if (inst->use_empty())
        inst->eraseFromParent();
    }
    m_eraseList.clear();
  }

  m_muladds.clear();
#endif

  m_ops.clear();

  return changed;
}

// =====================================================================================================================
// Determine the shape of the given transpose operation.
//
// @param [in] transpose : the transpose operation
// @returns : the cooperative matrix shape
Shape CooperativeMatrixCombiner::getShapeOfTranspose(CooperativeMatrixTransposeOp &transpose) {
  auto elemType = transpose.getElemType();
  auto layout = transpose.getLayout();
  return {(CooperativeMatrixElementType)elemType, (CooperativeMatrixLayout)layout};
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
  if (auto *convertOp = dyn_cast<CooperativeMatrixConvertOp>(op)) {
    src = convertOp->getSource();
    isConvert = true;
  } else if (auto *transposeOp = dyn_cast<CooperativeMatrixTransposeOp>(op)) {
    src = transposeOp->getMatrix();
  } else {
    llvm_unreachable("the operation is not supported here.");
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
        auto srcElementType = cast<CooperativeMatrixConvertOp>(op)->getSrcElemType();
        auto dstElementType = cast<CooperativeMatrixConvertOp>(op)->getDstElemType();
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

    if (auto *timesScalarOp = dyn_cast<CooperativeMatrixTimesScalarOp>(val)) {
      if (is_contained(component.inner.timesScalars, val))
        return true;

      component.inner.timesScalars.push_back(cast<CallInst>(val));
      worklistForward.push_back(val);
      worklistBackward.push_back(timesScalarOp->getMatrix());
      return true;
    }
    if (auto *binOp = dyn_cast<CooperativeMatrixBinaryOp>(val)) {
      if (is_contained(component.inner.binOps, val))
        return true;

      component.inner.binOps.push_back(cast<CallInst>(val));
      worklistForward.push_back(val);
      worklistBackward.push_back(binOp->getLhs());
      worklistBackward.push_back(binOp->getRhs());
      return true;
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
  std::optional<CooperativeMatrixLayout> otherLayout;
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

  auto foundOtherLayout = [&](CooperativeMatrixLayout layout, Type *type) {
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

    if (isa<CooperativeMatrixLoadOp>(input))
      continue; // loads can be adjusted at zero cost
    if (auto *transposeOp = dyn_cast<CooperativeMatrixTransposeOp>(input)) {
      foundComponentShape(getShapeOfTranspose(*transposeOp));
      ++numTransposeInputs;
      continue;
    }
    if (auto *convertOp = dyn_cast<CooperativeMatrixConvertOp>(input)) {
      auto srcElemType = convertOp->getSrcElemType();
      auto dstElemType = convertOp->getDstElemType();
      if (srcElemType != dstElemType) {
        LLVM_DEBUG(dbgs() << "  unhandled element type input conversion: " << *input << '\n');
        ++numUnhandledInputs;
        continue;
      }

      auto srcLayout = convertOp->getSrcLayout();
      auto dstLayout = convertOp->getDstLayout();
      foundComponentShape({dstElemType, dstLayout});
      foundOtherLayout(srcLayout, convertOp->getSource()->getType());

      ++numRelayoutInputs;
      continue;
    }

    ++numUnhandledInputs;
  }

  for (Use *use : component.outputs) {
    if (dyn_cast<CooperativeMatrixStoreOp>(use->getUser()))
      continue; // stores can be adapted at zero cost
    if (auto *transposeOp = dyn_cast<CooperativeMatrixTransposeOp>(use->getUser())) {
      foundComponentShape(getShapeOfTranspose(*transposeOp));
      transposeOutputs.insert(use->get());
      continue;
    }
    if (auto *convertOp = dyn_cast<CooperativeMatrixConvertOp>(use->getUser())) {
      auto srcElemType = convertOp->getSrcElemType();
      auto dstElemType = convertOp->getDstElemType();
      if (srcElemType != dstElemType) {
        LLVM_DEBUG(dbgs() << "  unhandled element type output conversion: " << *use->getUser() << '\n');
        ++numUnhandledInputs;
        continue;
      }

      auto srcLayout = convertOp->getSrcLayout();
      auto dstLayout = convertOp->getDstLayout();
      foundComponentShape({srcElemType, srcLayout});
      foundOtherLayout(dstLayout, use->getUser()->getType());

      relayoutOutputs.insert(use->get());
      continue;
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
      if (auto *transposeOp = dyn_cast<CooperativeMatrixTransposeOp>(input)) {
        Value *src = transposeOp->getMatrix();
        foldTo(input, src);

        // Prepopulate the transpose cache to re-use the old transpose operation instead of creating a new one.
        outTransposed.try_emplace(src, input);
        continue;
      }
      if (auto *loadOp = dyn_cast<CooperativeMatrixLoadOp>(input)) {
        bool colMajor = loadOp->getColMajor();
        loadOp->setColMajor(!colMajor);
        continue;
      }

      // Handle generic inputs that need to be transposed explicitly.
      if (auto *inst = dyn_cast<Instruction>(input)) {
        b.SetInsertPoint(inst->getNextNode());
      } else {
        assert(isa<Argument>(input));
        b.SetInsertPointPastAllocas(&m_function);
      }

      Type *resultMatrixTy = b.getCooperativeMatrixTy(component.shape->elementType, component.shape->layout);
      auto *transposed = b.create<CooperativeMatrixTransposeOp>(resultMatrixTy, PoisonValue::get(input->getType()),
                                                                component.shape->elementType, component.shape->layout);
      foldTo(input, transposed);
      transposed->setMatrix(input);
    }

    for (Use *use : component.outputs) {
      // Handle outputs that can be folded away / absorbed.
      if (isa<CooperativeMatrixTransposeOp>(use->getUser())) {
        foldTo(use->getUser(), use->get());
        continue;
      }
      if (auto *storeOp = dyn_cast<CooperativeMatrixStoreOp>(use->getUser())) {
        bool colMajor = storeOp->getColMajor();
        storeOp->setColMajor(!colMajor);
        continue;
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

        Type *resultMatrixTy = b.getCooperativeMatrixTy(component.shape->elementType, component.shape->layout);
        transposed = b.create<CooperativeMatrixTransposeOp>(resultMatrixTy, use->get(), component.shape->elementType,
                                                            component.shape->layout);
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
      cast<CooperativeMatrixTimesScalarOp>(timesScalar)->setLayout(*otherLayout);
      continue;
    }

    for (CallInst *binOp : component.inner.binOps) {
      binOp->mutateType(otherType);
      cast<CooperativeMatrixBinaryOp>(binOp)->setLayout(*otherLayout);
      continue;
    }

    for (Value *input : component.inputs) {
      // Handle inputs for which the relayout can be folded or absorbed.
      if (auto *convertOp = dyn_cast<CooperativeMatrixConvertOp>(input)) {
        auto srcElemType = convertOp->getSrcElemType();
        auto dstElemType = convertOp->getDstElemType();

        if (srcElemType == dstElemType) {
          assert(convertOp->getSrcLayout() == *otherLayout);

          Value *src = convertOp->getSource();
          foldTo(input, src);

          // Pre-populate the cache to re-use the relayout operation instead of creating a new one.
          outRelayouted.try_emplace(src, input);
          continue;
        }

        // Integrate the relayouting into a merged conversion op.
        convertOp->setDstLayout(*otherLayout);
        continue;
      }
      if (auto *loadOp = dyn_cast<CooperativeMatrixLoadOp>(input)) {
        loadOp->setLayout(*otherLayout);
        continue;
      }

      // Handle generic inputs that need a new convert operation inserted.
      if (auto *inst = dyn_cast<Instruction>(input)) {
        b.SetInsertPoint(inst->getNextNode());
      } else {
        assert(isa<Argument>(input));
        b.SetInsertPointPastAllocas(&m_function);
      }

      Type *resultMatrixTy = b.getCooperativeMatrixTy(component.shape->elementType, *otherLayout);
      CooperativeMatrixConvertOp *convert = b.create<CooperativeMatrixConvertOp>(
          resultMatrixTy, (CastInst::CastOps)0, PoisonValue::get(input->getType()), component.shape->elementType,
          component.shape->elementType, component.shape->layout, *otherLayout);
      foldTo(input, convert);
      convert->setSource(input);
    }

    for (Use *use : component.outputs) {
      // Handle outputs for which the relayout can be folded or absorbed.
      if (auto *convertOp = dyn_cast<CooperativeMatrixConvertOp>(use->getUser())) {
        auto srcElemType = convertOp->getSrcElemType();
        auto dstElemType = convertOp->getDstElemType();

        if (srcElemType == dstElemType) {
          assert(convertOp->getDstLayout() == *otherLayout);

          foldTo(use->getUser(), use->get());
          continue;
        }
      }
      if (auto *storeOp = dyn_cast<CooperativeMatrixStoreOp>(use->getUser())) {
        storeOp->setLayout(*otherLayout);
        continue;
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

        Type *resultMatrixTy = b.getCooperativeMatrixTy(component.shape->elementType, component.shape->layout);
        relayouted = b.create<CooperativeMatrixConvertOp>(resultMatrixTy, (CastInst::CastOps)0, use->get(),
                                                          component.shape->elementType, component.shape->elementType,
                                                          *otherLayout, component.shape->layout);
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

    if (isa<PHINode>(userInst))
      continue;

    if (!earliestUser || userInst->comesBefore(earliestUser))
      earliestUser = userInst;
  }
  return earliestUser;
}

bool CooperativeMatrixCombiner::tryFoldMuladd(SmallVector<CooperativeMatrixMulAddOp *> muladds) {
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
    SmallVector<CooperativeMatrixMulAddOp *> muladdChain;

    auto *matCLo = muladdLo->getMatrixC();

    muladdChain.push_back(muladdLo);
    muladdLo->setIsSatOrOpsel(false);
    while (muladdLo->hasOneUse()) {
      auto *next = dyn_cast<CooperativeMatrixMulAddOp>(*muladdLo->users().begin());

      if (!is_contained(muladds, next))
        break;
      next->setIsSatOrOpsel(false);
      muladdChain.push_back(next);
      muladdLo = next;
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 478769
      llvm::erase_value(muladds, muladdLo);
#else
      llvm::erase(muladds, muladdLo);
#endif
    }

    Instruction *firstLoUser = findFirstUser(muladdLo);

    CooperativeMatrixMulAddOp *muladdHi = nullptr;
    for (auto *candidate : llvm::reverse(muladds)) {
      if (firstLoUser && firstLoUser->comesBefore(candidate))
        continue;

      if (auto *matCHi = dyn_cast<Instruction>(candidate->getMatrixC())) {
        if (matCHi->getParent() == muladdLo->getParent() && packInsertPoint->comesBefore(matCHi)) {
          continue;
        }
      }

      muladdHi = candidate;
      break;
    }

    if (!muladdHi)
      continue;

    auto *matCHi = muladdHi->getMatrixC();

    muladdChain.push_back(muladdHi);
    muladdHi->setIsSatOrOpsel(true);
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 478769
    llvm::erase_value(muladds, muladdLo);
#else
    llvm::erase(muladds, muladdHi);
#endif
    while (muladdHi->hasOneUse()) {
      auto *next = dyn_cast<CooperativeMatrixMulAddOp>(*muladdHi->users().begin());
      if (!is_contained(muladds, next)) {
        break;
      }

      if (firstLoUser && firstLoUser->comesBefore(next)) {
        break;
      }
      next->setIsSatOrOpsel(true);
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
    Type *packedTy = FixedVectorType::get(b.getFloatTy(), 8);
    Value *curAccum = nullptr;
    if (phiLo && phiHi && phiLo->getParent() == phiHi->getParent()) {
      for (BasicBlock *incoming : phiLo->blocks()) {
        b.SetInsertPoint(incoming->getTerminator());
        auto *matCLo = phiLo->getIncomingValueForBlock(incoming);
        auto *matCHi = phiHi->getIncomingValueForBlock(incoming);
        auto *packed = b.create<CooperativeMatrixPackOp>(packedTy, matCLo, matCHi);
        phiLo->setIncomingValueForBlock(incoming, packed);
        phiHi->setIncomingValueForBlock(incoming, packed);
      }
      curAccum = phiHi;
      worklist.push_back({phiLo, phiHi, phiHi});
    } else {
      // otherwise, we pack just before the first muladd
      b.SetInsertPoint(packInsertPoint);
      curAccum = b.create<CooperativeMatrixPackOp>(packedTy, matCLo, matCHi);
    }

    for (auto *next : muladdChain) {
      next->setMatrixC(curAccum);
      next->setIsTied(true);
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

        if (auto *call = dyn_cast<CooperativeMatrixTimesScalarOp>(use.getUser())) {
          auto *candidate = llvm::find_if(unpackedUses, [&](auto pair) {
            if (auto *timesScalarOp = dyn_cast<CooperativeMatrixTimesScalarOp>(pair.first.getUser())) {
              if (timesScalarOp->getMatrix() == current.matrixLo) {
                return true;
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

        unpackedUses.push_back({use, true});
      }

      for (auto use : unpackedUses) {
        if (is_contained(m_eraseList, use.first.getUser()))
          continue;

        if (auto *packOp = dyn_cast<CooperativeMatrixPackOp>(use.first.getUser())) {
          if (packOp->getMatrixCLo() == current.matrixLo && packOp->getMatrixCHi() == current.matrixHi) {
            foldTo(use.first.getUser(), current.packedAccum);
            continue;
          }
        }

        if (auto *phi = dyn_cast<PHINode>(use.first.getUser())) {
          auto *predecessor = phi->getIncomingBlock(use.first);
          b.SetInsertPoint(predecessor->getTerminator());
        } else {
          b.SetInsertPoint(cast<Instruction>(use.first.getUser()));
        }
        Type *unpackedTy = FixedVectorType::get(b.getFloatTy(), 8);
        auto unpacked = b.create<CooperativeMatrixUnPackOp>(unpackedTy, current.packedAccum, use.second);
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
  auto *loScalar = cast<CooperativeMatrixTimesScalarOp>(*timesScalarLo).getScalar();
  auto *hiScalar = cast<CooperativeMatrixTimesScalarOp>(*timesScalarHi).getScalar();
  scalarVec = b.CreateInsertElement(scalarVec, loScalar, b.getInt32(0));
  scalarVec = b.CreateInsertElement(scalarVec, hiScalar, b.getInt32(1));
  auto *timesScalarPacked = b.create<CooperativeMatrixTimesScalarOp>(packedMatrix->getType(), packedMatrix, scalarVec,
                                                                     CooperativeMatrixElementType::Float16Packed,
                                                                     CooperativeMatrixLayout::AccumulatorMatrixLayout);
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
} // namespace lgc
