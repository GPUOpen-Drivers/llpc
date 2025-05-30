/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderImpl.cpp
 * @brief LLPC source file: implementation of lgc::BuilderImpl
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderImpl.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// BuilderImpl constructor
//
// @param pipeline : PipelineState (as public superclass Pipeline)
BuilderImpl::BuilderImpl(Pipeline *pipeline)
    : BuilderDefs(pipeline->getContext()), m_pipelineState(static_cast<PipelineState *>(pipeline)),
      m_builderContext(pipeline->getLgcContext()) {
}

// =====================================================================================================================
// Get the ShaderModes object.
ShaderModes *BuilderImpl::getShaderModes() {
  return m_pipelineState->getShaderModes();
}

// =====================================================================================================================
// Get the type elementTy, turned into a vector of the same vector width as maybeVecTy if the latter
// is a vector type.
//
// @param elementTy : Element type
// @param maybeVecTy : Possible vector type to get number of elements from
Type *BuilderBase::getConditionallyVectorizedTy(Type *elementTy, Type *maybeVecTy) {
  if (auto vecTy = dyn_cast<FixedVectorType>(maybeVecTy))
    return FixedVectorType::get(elementTy, vecTy->getNumElements());
  return elementTy;
}

// =====================================================================================================================
// Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
//
// @param vector1 : The float vector 1
// @param vector2 : The float vector 2
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateDotProduct(Value *const vector1, Value *const vector2, const Twine &instName) {
  if (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 12) {
    // Use a chain of v_dot2_f16_f16/v_dot2_bf16_bf16 on gfx12+.
    //
    // Note: GFX11 has this instruction, but its precision doesn't satisfy Vulkan requirements.
    //
    // Note: GFX10 chips may have v_dot2_f32_f16, which we could consider generating in cases where bitexact results
    //       are not required.
    //
    // Note: v_dot2_f16_f16/v_dot2_bf16_bf16 only respects RTE mode according to HW spec. We must check the
    //       specified rounding mode before using it. Also, v_dot2_f16_f16/v_dot2_bf16_bf16 is not IEEE compliant
    //       so we must check NSZ as well.
    const auto fp16RoundMode =
        getPipelineState()->getShaderModes()->getCommonShaderMode(m_shaderStage.value()).fp16RoundMode;
    const auto vectorTy = dyn_cast<FixedVectorType>(vector1->getType());
    if (vectorTy && (vectorTy->getScalarSizeInBits() == 16) &&
        (fp16RoundMode == FpRoundMode::DontCare || fp16RoundMode == FpRoundMode::Even) &&
        getFastMathFlags().noSignedZeros()) {
      int compCount = vectorTy->getNumElements();
      Value *result = nullptr;
      Type *basicType = getHalfTy();
      Intrinsic::AMDGCNIntrinsics inst = Intrinsic::amdgcn_fdot2_f16_f16;
      if (vectorTy->getScalarType()->isBFloatTy()) {
        basicType = getBFloatTy();
        inst = Intrinsic::amdgcn_fdot2_bf16_bf16;
      }

      if (compCount % 2 == 0) {
        result = ConstantFP::get(basicType, 0.0);
      } else {
        // If the component count is odd, prefer feeding the last product (odd one out) as initial value.
        Value *lhs = CreateExtractElement(vector1, compCount - 1);
        Value *rhs = CreateExtractElement(vector2, compCount - 1);
        result = CreateFMul(lhs, rhs);
      }

      for (int i = 0; i + 1 < compCount; i += 2) {
        Value *lhs = CreateShuffleVector(vector1, {i, i + 1});
        Value *rhs = CreateShuffleVector(vector2, {i, i + 1});
        result = CreateIntrinsic(basicType, inst, {lhs, rhs, result});
      }
      return result;
    }
  }

  Value *product = CreateFMul(vector1, vector2);
  if (!isa<VectorType>(product->getType()))
    return product;

  const unsigned compCount = cast<FixedVectorType>(product->getType())->getNumElements();
  Value *scalar = CreateExtractElement(product, uint64_t(0));

  for (unsigned i = 1; i < compCount; ++i)
    scalar = CreateFAdd(scalar, CreateExtractElement(product, i));

  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Create code to calculate the dot product of two integer vectors, with optional accumulator, using hardware support
// where available. The factor inputs are always <N x iM> of the same type, N can be arbitrary and M must be 4, 8, 16,
// 32, or 64 Use a value of 0 for no accumulation and the value type is consistent with the result type. The result is
// saturated if there is an accumulator. Only the final addition to the accumulator needs to be saturated.
// Intermediate overflows of the dot product can lead to an undefined result.
//
// @param vector1 : The integer Vector 1
// @param vector2 : The integer Vector 2
// @param accumulator : The accumulator to the scalar of dot product
// @param flags : The first bit marks whether Vector 1 is signed and the second bit marks whether Vector 2 is signed
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateIntegerDotProduct(Value *vector1, Value *vector2, Value *accumulator, unsigned flags,
                                            const Twine &instName) {
  if (flags == SecondVectorSigned) {
    std::swap(vector1, vector2);
    flags = FirstVectorSigned;
  }
  const bool isBothSigned = (flags == (FirstVectorSigned | SecondVectorSigned));
  const bool isMixedSigned = (flags == FirstVectorSigned);
  const bool isSigned = isBothSigned || isMixedSigned;

  // The factor inputs are always <N x iM> of the same type
  Type *inputTy = vector1->getType();
  assert(inputTy->isVectorTy() && inputTy->getScalarType()->isIntegerTy() && inputTy == vector2->getType());
  const unsigned compCount = cast<FixedVectorType>(inputTy)->getNumElements();

  // The supported size of M
  const unsigned compBitWidth = inputTy->getScalarSizeInBits();
  assert(compBitWidth == 4 || compBitWidth == 8 || compBitWidth == 16 || compBitWidth == 32 || compBitWidth == 64);

  // The result type is given by accumulator, which must be greater than or equal to that of the components of Vector 1
  Type *expectedTy = accumulator->getType();
  const bool hasAccumulator = !(isa<ConstantInt>(accumulator) && cast<ConstantInt>(accumulator)->isNullValue());
  const unsigned expectedWidth = expectedTy->getScalarSizeInBits();
  assert(expectedWidth == 4 || expectedWidth == 8 || expectedWidth == 16 || expectedWidth == 32 || expectedWidth == 64);

  // Check if there is a native intrinsic that can do the entire operation (dot product and saturating accumulate) in a
  // single instruction. They must meet the two conditions:
  // 1. The required native intrinsic is supported by the specified hardware
  // 2. The factor inputs must be <2 x i16> or <N x i8> (N <= 4) or <N x i4> (N <= 8)
  const auto &supportIntegerDotFlag = getPipelineState()->getTargetInfo().getGpuProperty().supportIntegerDotFlag;
  const bool isSupportCompBitwidth = (supportIntegerDotFlag.compBitwidth16 && compBitWidth == 16) ||
                                     (supportIntegerDotFlag.compBitwidth8 && compBitWidth == 8) ||
                                     (supportIntegerDotFlag.compBitwidth4 && compBitWidth == 4);
  const bool isSupportSignedness =
      isMixedSigned ? supportIntegerDotFlag.diffSignedness : supportIntegerDotFlag.sameSignedness;
  const bool isDot2 = (compCount == 2 && compBitWidth == 16);
  const bool isDot4 = (compCount <= 4 && compBitWidth == 8);
  const bool isDot8 = (compCount <= 8 && compBitWidth == 4);
  const bool hasNativeIntrinsic =
      isSupportCompBitwidth && isSupportSignedness && (isDot2 || isDot4 || isDot8) && (expectedWidth <= 32);
  const bool hasSudot = getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 11;

  auto input1 = vector1;
  auto input2 = vector2;
  Value *computedResult = nullptr;
  if (hasNativeIntrinsic) {
    int supportedN = InvalidValue;
    int intrinsic = InvalidValue;
    if (isDot2) {
      intrinsic = isBothSigned ? Intrinsic::amdgcn_sdot2 : Intrinsic::amdgcn_udot2;
      supportedN = 2;
    } else if (isDot4) {
      if (hasSudot)
        intrinsic = isSigned ? Intrinsic::amdgcn_sudot4 : Intrinsic::amdgcn_udot4;
      else
        intrinsic = isBothSigned ? Intrinsic::amdgcn_sdot4 : Intrinsic::amdgcn_udot4;
      supportedN = 4;
    } else {
      assert(isDot8);
      if (hasSudot)
        intrinsic = isSigned ? Intrinsic::amdgcn_sudot8 : Intrinsic::amdgcn_udot8;
      else
        intrinsic = isBothSigned ? Intrinsic::amdgcn_sdot8 : Intrinsic::amdgcn_udot8;
      supportedN = 8;
    }
    assert(intrinsic != InvalidValue);
    // Do null-extension
    SmallVector<int, 8> shuffleMask;
    for (int i = 0; i < supportedN; ++i)
      shuffleMask.push_back(std::min(i, static_cast<int>(compCount)));
    input1 = CreateShuffleVector(input1, Constant::getNullValue(inputTy), shuffleMask);
    input2 = CreateShuffleVector(input2, Constant::getNullValue(inputTy), shuffleMask);

    // Cast to i32 for dot4 and dot8
    if (compBitWidth == 4 || compBitWidth == 8) {
      input1 = CreateBitCast(input1, getInt32Ty());
      input2 = CreateBitCast(input2, getInt32Ty());
    }

    Value *clamp = hasAccumulator ? getTrue() : getFalse();
    accumulator = isSigned ? CreateSExt(accumulator, getInt32Ty()) : CreateZExt(accumulator, getInt32Ty());
    if (hasSudot && isSigned) {
      computedResult = CreateIntrinsic(
          intrinsic, {}, {getTrue(), input1, getInt1(isBothSigned), input2, accumulator, clamp}, nullptr, instName);
    } else {
      computedResult = CreateIntrinsic(intrinsic, {}, {input1, input2, accumulator, clamp}, nullptr, instName);
    }
  } else {
    Value *sum = nullptr;
    const bool canUseDot2 = isSupportCompBitwidth && isSupportSignedness && !isDot4 && !isDot8;
    if (canUseDot2) {
      sum = getInt32(0);
      // Iterator over two components at a time and perform shuffle vectors and then use the intrinsic
      unsigned intrinsic = isBothSigned ? Intrinsic::amdgcn_sdot2 : Intrinsic::amdgcn_udot2;
      for (int compIdx = 0; compIdx < compCount; compIdx += 2) {
        input1 = CreateShuffleVector(vector1, Constant::getNullValue(inputTy), ArrayRef<int>{compIdx, compIdx + 1});
        input2 = CreateShuffleVector(vector2, Constant::getNullValue(inputTy), ArrayRef<int>{compIdx, compIdx + 1});
        sum = CreateIntrinsic(intrinsic, {}, {input1, input2, sum, getFalse()}, nullptr, instName);
      }
    } else {
      sum = getIntN(expectedWidth, 0);
      for (unsigned compIdx = 0; compIdx < compCount; ++compIdx) {
        Value *elem1 = CreateExtractElement(vector1, compIdx);
        elem1 = isSigned ? CreateSExt(elem1, expectedTy) : CreateZExt(elem1, expectedTy);
        Value *elem2 = CreateExtractElement(vector2, compIdx);
        elem2 = isBothSigned ? CreateSExt(elem2, expectedTy) : CreateZExt(elem2, expectedTy);
        sum = CreateAdd(sum, CreateMul(elem1, elem2));
      }
    }
    if (hasAccumulator) {
      if (sum->getType()->getScalarSizeInBits() > expectedWidth)
        sum = CreateTrunc(sum, expectedTy);
      else if (sum->getType()->getScalarSizeInBits() < expectedWidth)
        sum = isSigned ? CreateSExt(sum, expectedTy) : CreateZExt(sum, expectedTy);

      Intrinsic::ID addIntrinsic = isSigned ? Intrinsic::sadd_sat : Intrinsic::uadd_sat;
      sum = CreateBinaryIntrinsic(addIntrinsic, sum, accumulator, nullptr, instName);
    }
    computedResult = sum;
  }

  // Do clampping or truncation
  Type *computedTy = computedResult->getType();
  const unsigned computedWidth = computedTy->getScalarSizeInBits();
  if (expectedWidth < computedWidth) {
    if (hasAccumulator) {
      // Compute the clamp range based on the
      unsigned long long unsignedMax = (2ULL << (expectedWidth - 1)) - 1;
      long long signedMax = unsignedMax >> 1;
      long long signedMin = -1LL - signedMax;

      Value *minimum = isSigned ? ConstantInt::getSigned(computedTy, signedMin) : getIntN(computedWidth, 0);
      Value *maximum = isSigned ? ConstantInt::getSigned(computedTy, signedMax) : getIntN(computedWidth, unsignedMax);
      Intrinsic::ID minIntrinsic = isSigned ? Intrinsic::smin : Intrinsic::umin;
      Intrinsic::ID maxIntrinsic = isSigned ? Intrinsic::smax : Intrinsic::umax;

      computedResult = CreateBinaryIntrinsic(maxIntrinsic, computedResult, minimum, nullptr, instName);
      computedResult = CreateBinaryIntrinsic(minIntrinsic, computedResult, maximum, nullptr, instName);
    }
    computedResult = CreateTrunc(computedResult, expectedTy);
  }

  computedResult->setName(instName);
  return computedResult;
}

// =====================================================================================================================
// Get whether the context we are building in supports ds_bpermute or v_bpermute across all lanes in the wave
//
// @param shaderStage : shader stage enum.
bool BuilderImpl::supportWaveWideBPermute(ShaderStageEnum shaderStage) const {
  auto gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion().major;
  auto supportBPermute = gfxIp == 8 || gfxIp == 9 || gfxIp == 12;
  auto waveSize = getPipelineState()->getShaderWaveSize(shaderStage);
  supportBPermute = supportBPermute || waveSize == 32;
  return supportBPermute;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane 64 DPP operations.
bool BuilderImpl::supportPermLane64Dpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 11;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane var operations.
bool BuilderImpl::supportPermLaneVar() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 12;
}

// =====================================================================================================================
// Create an "if..endif" or "if..else..endif" structure. The current basic block becomes the "endif" block, and all
// instructions in that block before the insert point are moved to the "if" block. The insert point is moved to
// the start of the "then" block; the caller can save the insert point before calling this method then restore it
// afterwards to restore the insert point to where it was just after the endif, and still keep its debug location.
// The method returns the branch instruction, whose first branch target is the "then" block and second branch
// target is the "else" block, or "endif" block if no "else" block.
//
// @param condition : The "if" condition
// @param wantElse : Whether to generate an "else" block
// @param instName : Base of name for new basic blocks
BranchInst *BuilderCommon::CreateIf(Value *condition, bool wantElse, const Twine &instName) {
  // Create "if" block and move instructions in current block to it.
  BasicBlock *endIfBlock = GetInsertBlock();
  BasicBlock *ifBlock = BasicBlock::Create(getContext(), "", endIfBlock->getParent(), endIfBlock);
  ifBlock->takeName(endIfBlock);
  endIfBlock->setName(instName + ".endif");
  ifBlock->splice(ifBlock->end(), endIfBlock, endIfBlock->begin(), GetInsertPoint());

  // Replace non-phi uses of the original block with the new "if" block.
  SmallVector<Use *, 4> nonPhiUses;
  for (auto &use : endIfBlock->uses()) {
    if (!isa<PHINode>(use.getUser()))
      nonPhiUses.push_back(&use);
  }
  for (auto use : nonPhiUses)
    use->set(ifBlock);

  // Create "then" and "else" blocks.
  BasicBlock *thenBlock = BasicBlock::Create(getContext(), instName + ".then", endIfBlock->getParent(), endIfBlock);
  BasicBlock *elseBlock = nullptr;
  if (wantElse) {
    elseBlock = BasicBlock::Create(getContext(), instName + ".else", endIfBlock->getParent(), endIfBlock);
  }

  // Create the branches.
  BranchInst *branch = BranchInst::Create(thenBlock, elseBlock ? elseBlock : endIfBlock, condition, ifBlock);
  branch->setDebugLoc(getCurrentDebugLocation());
  BranchInst::Create(endIfBlock, thenBlock)->setDebugLoc(getCurrentDebugLocation());
  if (elseBlock)
    BranchInst::Create(endIfBlock, elseBlock)->setDebugLoc(getCurrentDebugLocation());

  // Set Builder's insert point to the branch at the end of the "then" block.
  SetInsertPoint(thenBlock->getTerminator());
  return branch;
}

#if defined(LLVM_HAVE_BRANCH_AMD_GFX)
// =====================================================================================================================
// Track a small number of instructions, giving each of them an index by which they can easily be identified.
class TinyInstructionTracker {
  unsigned m_indexCounter = 0;
  // List of all instructions we've encountered.
  SmallVector<Instruction *> m_instructions;
  // Index we've assigned to instructions.
  DenseMap<Instruction *, unsigned> m_indexForInstruction;

public:
  TinyInstructionTracker() {}
  size_t size() const { return m_instructions.size(); }
  unsigned indexForInstruction(Instruction *inst);
  Instruction *instructionForIndex(unsigned idx) const { return m_instructions[idx]; }

  bool contains(Instruction *instr) const { return m_indexForInstruction.contains(instr); }

  void init(Instruction *instr) {
    m_instructions.push_back(instr);
    m_indexForInstruction[instr] = m_indexCounter;
    m_indexCounter++;
  }
};

// Return the index for the given instruction, adding it to the tracker if necessary.
unsigned TinyInstructionTracker::indexForInstruction(Instruction *inst) {
  auto [it, inserted] = m_indexForInstruction.try_emplace(inst, m_instructions.size());
  if (inserted)
    m_instructions.push_back(inst);
  return it->second;
}

// =====================================================================================================================
// A simple memory efficient container that holds the dependencies of the instructions.
class TinyInstructionSet {
  BitVector m_bits;

public:
  class const_iterator {
    BitVector::const_set_bits_iterator m_it;
    const TinyInstructionTracker &m_tracker;

  public:
    const_iterator(BitVector::const_set_bits_iterator it, const TinyInstructionTracker &tracker)
        : m_it(it), m_tracker(tracker) {}
    const_iterator &operator++() {
      ++m_it;
      return *this;
    }

    Instruction *operator*() {
      unsigned index = *m_it;
      return m_tracker.instructionForIndex(index);
    }

    bool operator!=(const const_iterator &otherIt) {
      assert(&otherIt.m_tracker == &m_tracker && "Iterators of different objects.");
      return otherIt.m_it != m_it;
    }
  };

  const_iterator begin(const TinyInstructionTracker &tracker) const {
    return const_iterator(m_bits.set_bits_begin(), tracker);
  }

  const_iterator end(const TinyInstructionTracker &tracker) const {
    return const_iterator(m_bits.set_bits_end(), tracker);
  }

  void insert(unsigned index) {
    if (index >= m_bits.size())
      m_bits.resize(index + 1);
    m_bits.set(index);
  }

  void insert(Instruction *instr, TinyInstructionTracker &tracker) { insert(tracker.indexForInstruction(instr)); }

  bool contains(unsigned index) const { return index < m_bits.size() && m_bits[index]; }

  bool contains(Instruction *instr, TinyInstructionTracker &tracker) const {
    return contains(tracker.indexForInstruction(instr));
  }

  unsigned size() const { return m_bits.count(); }

  bool empty() const { return !m_bits.any(); }

  TinyInstructionSet &operator|=(const TinyInstructionSet &rhs) {
    m_bits |= rhs.m_bits;
    return *this;
  }
};

// =====================================================================================================================
// Traverse the instructions to find the non-uniform index. In case of scalarization of descriptor loads, we also
// collect the dependencies of the instructions.
class TraceNonUniformIndex {
  TinyInstructionTracker m_tracker;
  // For each value, the set of instructions that depend on it.
  DenseMap<Value *, TinyInstructionSet> m_dependentInstructions;
  SmallVector<Value *> nonUniformIndices;
  void init(Instruction *instr) { m_tracker.init(instr); }

public:
  TraceNonUniformIndex() : m_tracker() {}

  // Non-uniform index calculation.
  Value *run(Value *);

  // Helper functions for non-uniform index.
  void setNonUniformIndex(Value *nonUniformIndex) { nonUniformIndices.push_back(nonUniformIndex); }

  SmallVector<Value *> &getNonUniformIndexes() { return nonUniformIndices; }

  unsigned getNumOfNonUniformIndexes() { return nonUniformIndices.size(); }

  // Return true if there are not any non-uniform indexes.
  bool empty() { return nonUniformIndices.empty(); }

  // Helper functions for reading/writing the instruction dependencies.
  auto getDependentInstructions(Value *value) {
    TinyInstructionSet &dependents = m_dependentInstructions[value];
    return llvm::make_range(dependents.begin(m_tracker), dependents.end(m_tracker));
  }

  bool hasDependentInstructions(Value *value) { return !m_dependentInstructions[value].empty(); }

  void addDependents(Value *newValue, Instruction *dependent) {
    init(cast<Instruction>(newValue));
    TinyInstructionSet &dst = m_dependentInstructions[newValue];
    for (Instruction *dep : getDependentInstructions(dependent)) {
      dst.insert(dep, m_tracker);
      auto it = m_dependentInstructions.find(dep);
      if (it != m_dependentInstructions.end())
        dst |= it->second;
    }
    if (dependent)
      dst.insert(dependent, m_tracker);
  }
};

// For a non-uniform input, try and trace back through descriptor pointer to find the non-uniform index used in it. If
// that fails, we just use the operand value as the index.
//
// Note that this function may return null, which means that the given value has been shown to be uniform.
//
// This uses a fairly simple heuristic that nevertheless allows temporary expansion of the search breadth to handle
// the common case where a base pointer is assembled from separate high and low halves.
//
// In case of scalarization, while it traverses all use-def predecessors of the nonUniformVal, it adds the  instructions
// to instrDeps map (addDependents()). These dependencies are the instructions that will be cloned and moved
// inside the waterfall loop.
//
// @param nonUniformVal : Value representing non-uniform descriptor
// @return : Value representing the non-uniform index, or null if nonUniformVal could be proven to be uniform
Value *TraceNonUniformIndex::run(Value *nonUniformVal) {
  auto inst = dyn_cast<Instruction>(nonUniformVal);
  if (!inst) {
    // Could plausibly be a constant or a function argument. Either way, we don't have to search any further.
    return isa<Constant>(nonUniformVal) ? nullptr : nonUniformVal;
  }

  auto getSize = [](Value *value) -> uint64_t {
    uint64_t size = value->getType()->getPrimitiveSizeInBits().getFixedValue();
    return size ? size : std::numeric_limits<uint64_t>::max();
  };

  uint64_t nonUniformValSize = getSize(nonUniformVal);

  // Loop until all nonUniforms have been found to be uniform or a heuristic abort criterion has been reached.
  Value *candidateIndex = nullptr;
  SmallVector<Instruction *, 2> nonUniforms;
  nonUniforms.push_back(inst);

  auto propagate = [&](Value *currentOp, Instruction *current) {
    if (auto instOp = dyn_cast<Instruction>(currentOp)) {
      if (nonUniforms.size() >= 2)
        return false;
      nonUniforms.push_back(instOp);
      addDependents(instOp, current);
      return true;
    }
    return isa<Constant>(currentOp);
  };

  do {
    Instruction *current = nonUniforms.pop_back_val();

    // Immediately replace the current nonUniformVal by a strictly smaller one if possible.
    if (!candidateIndex && nonUniforms.empty() && current != nonUniformVal) {
      uint64_t size = getSize(current);
      if (size < nonUniformValSize) {
        nonUniformVal = current;
        nonUniformValSize = size;
      }
    }

    // See if we can propagate the search further.
    if (current->isCast() || current->isUnaryOp()) {
      if (!propagate(current->getOperand(0), current))
        return nonUniformVal;
      continue;
    }

    if (current->isBinaryOp()) {
      if (!propagate(current->getOperand(0), current) || !propagate(current->getOperand(1), current))
        return nonUniformVal;
      continue;
    }

    if (auto *load = dyn_cast<LoadInst>(current)) {
      Value *ptr = load->getPointerOperand();
      unsigned as = ptr->getType()->getPointerAddressSpace();
      if (as == ADDR_SPACE_FLAT || as == ADDR_SPACE_PRIVATE)
        return nonUniformVal; // load is a source of divergence, can't propagate

      if (!propagate(ptr, current))
        return nonUniformVal;
      continue;
    }

    if (auto gep = dyn_cast<GetElementPtrInst>(current)) {
      if (gep->hasAllConstantIndices()) {

        if (!propagate(gep->getPointerOperand(), current))
          return nonUniformVal;
        continue;
      }

      // Variable GEP, assume that the index is non-uniform.
      if (candidateIndex || gep->getNumIndices() != 1)
        return nonUniformVal;

      if (!propagate(gep->getPointerOperand(), current))
        return nonUniformVal;

      candidateIndex = *gep->idx_begin();
      if (getSize(candidateIndex) > nonUniformValSize)
        return nonUniformVal; // propagating further is worthless

      if (inst = dyn_cast<Instruction>(candidateIndex)) {
        if (inst->isCast() || inst->isUnaryOp()) {
          candidateIndex = inst->getOperand(0);
          addDependents(candidateIndex, inst);
        }
      }
      addDependents(candidateIndex, gep);
      continue;
    }

    if (auto extract = dyn_cast<ExtractValueInst>(current)) {
      if (!propagate(extract->getAggregateOperand(), current))
        return nonUniformVal;
      continue;
    }
    if (auto insert = dyn_cast<InsertValueInst>(current)) {
      if (!propagate(insert->getAggregateOperand(), current) || !propagate(insert->getInsertedValueOperand(), current))
        return nonUniformVal;
      continue;
    }
    if (auto extract = dyn_cast<ExtractElementInst>(current)) {
      if (!isa<Constant>(extract->getIndexOperand()) || !propagate(extract->getVectorOperand(), current))
        return nonUniformVal;
      continue;
    }
    if (auto insert = dyn_cast<InsertElementInst>(current)) {
      if (!isa<Constant>(insert->getOperand(2)) || !propagate(insert->getOperand(0), current) ||
          !propagate(insert->getOperand(1), current))
        return nonUniformVal;
      continue;
    }

    if (auto call = dyn_cast<CallInst>(current)) {
      if (auto intrinsic = dyn_cast<IntrinsicInst>(call)) {
        unsigned id = intrinsic->getIntrinsicID();
        if (id == Intrinsic::amdgcn_readfirstlane || id == Intrinsic::amdgcn_s_getpc ||
            id == Intrinsic::amdgcn_reloc_constant)
          continue; // is always uniform, no need to propagate
        return nonUniformVal;
      }

      if (isa<UserDataOp>(call) || isa<LoadUserDataOp>(call))
        continue; // is always uniform, no need to propagate

      return nonUniformVal;
    }

    // If we reach this point, it means we don't understand the instruction. It's likely a fairly complex instruction
    // and we should heuristically abort the propagation anyway. It may even be a source of divergence, in which case
    // propagating further would be incorrect.
    return nonUniformVal;
  } while (!nonUniforms.empty());

  return candidateIndex;
}

// =====================================================================================================================
// Test whether two instructions are identical or are the same operation on identical operands.
// @param lhs : First instruction
// @param rhs : Second instruction
// @return Result of equally test
static bool instructionsEqual(Instruction *lhs, Instruction *rhs) {
  if (lhs->isIdenticalTo(rhs))
    return true;

  if (!lhs->isSameOperationAs(rhs))
    return false;

  for (unsigned idx = 0, end = lhs->getNumOperands(); idx != end; ++idx) {
    Value *lhsVal = lhs->getOperand(idx);
    Value *rhsVal = rhs->getOperand(idx);
    if (lhsVal == rhsVal)
      continue;
    Instruction *lhsInst = dyn_cast<Instruction>(lhsVal);
    Instruction *rhsInst = dyn_cast<Instruction>(rhsVal);
    if (!lhsInst || !rhsInst)
      return false;
    if (!lhsInst->isIdenticalTo(rhsInst))
      return false;
  }

  return true;
}

// =====================================================================================================================
// Check if the non-uniform indexes are identical.
//
// @param traceNonUniformIndex : non-uniform index information
Instruction *getSharedIndex(TraceNonUniformIndex &traceNonUniformIndex) {
  // FIXME: these do not actually need to be identical if we introduce multiple waterfall
  // begin and readfirstlane intrinsics for these.
  Instruction *sharedIndex = nullptr;
  bool identicalIndexes = false;
  for (auto nonUniformVal : traceNonUniformIndex.getNonUniformIndexes()) {
    Instruction *nuInst = dyn_cast<Instruction>(nonUniformVal);
    if (!nuInst)
      return nullptr;

    identicalIndexes = sharedIndex && instructionsEqual(nuInst, sharedIndex);
    if (sharedIndex && !identicalIndexes)
      return nullptr;

    if (!sharedIndex)
      sharedIndex = nuInst;
  }
  return sharedIndex;
}

#endif

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.

// @param nonUniformInst : The instruction to put in a waterfall loop
// @param operandIdxs : The operand index for non-uniform inputs that need to be uniform
// @param useVgprForOperands : Non-uniform inputs should be put in VGPRs
// @param instName : Name to give instruction(s)
Instruction *BuilderImpl::createWaterfallLoop(Instruction *nonUniformInst, unsigned operandIdx, bool useVgprForOperands,
                                              const Twine &instName) {
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Waterfall feature disabled
  errs() << "Generating invalid waterfall loop code\n";
  return nonUniformInst;
#else
  assert(nonUniformInst && "nonUniformInstOperand must be provided");
  Value *nonUniformInstOperand = nonUniformInst->getOperand(operandIdx);
  assert(nonUniformInstOperand->getType()->isIntegerTy(32));
  TraceNonUniformIndex traceNonUniformIndex;
  Value *nonUniformIndex = traceNonUniformIndex.run(nonUniformInstOperand);
  if (!nonUniformIndex)
    return nonUniformInst;
  if (!nonUniformIndex->getType()->isIntegerTy(32))
    nonUniformIndex = nonUniformInstOperand;

  // Generate the waterfall loop intrinisics and implement the scalarization of the descriptor loads.
  // Save Builder's insert point
  IRBuilder<>::InsertPointGuard guard(*this);
  // Insert new code just before nonUniformInst.
  SetInsertPoint(nonUniformInst);

  // Emit waterfall.begin intrinsics. The first begin contains a null token for the previous token argument.
  Value *waterfallBegin = ConstantInt::get(getInt32Ty(), 0);

  assert(nonUniformIndex->getType()->isIntegerTy(32));
  waterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, getInt32Ty(), {waterfallBegin, nonUniformIndex},
                                   nullptr, instName);

  Value *newIntrinsic = nonUniformIndex;
  // When the non-uniform use is in a VGPR, we can save a v_mov by not inserting the amdgcn_waterfall_readfirstlane
  if (!useVgprForOperands)
    newIntrinsic = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {getInt32Ty(), getInt32Ty()},
                                   {waterfallBegin, newIntrinsic}, nullptr, instName);
  if (nonUniformInst->getType()->isVoidTy()) {
    // we are waterfalling is a store with no return value. Use
    // llvm.amdgcn.waterfall.last.use on the descriptor.
    newIntrinsic = CreateIntrinsic(useVgprForOperands ? Intrinsic::amdgcn_waterfall_last_use_vgpr
                                                      : Intrinsic::amdgcn_waterfall_last_use,
                                   getInt32Ty(), {waterfallBegin, newIntrinsic}, nullptr, instName);
  }
  // Replace the instruction operand.
  nonUniformInst->setOperand(operandIdx, newIntrinsic);

  if (nonUniformInst->getType()->isVoidTy())
    return nonUniformInst;

  return cast<Instruction>(endWaterfallLoop(nonUniformInst, waterfallBegin));
#endif
}

// =====================================================================================================================
// Create waterfallLoop begin and readfirstlane on the 32-bit indices transformed from descriptor pointers. Return the
// corresponding waterfallLoop begins if they are created.
//
// This is done in three steps:
// 1. Calculate the non-uniform indexes : Collect the non-uniform indexes that correspond to the operands of
// the descriptor pointer from a Gep. In addition, we need to collect all the instructions that need
// to be moved inside the loop. All these are done by traceNonUniformIndex.
//
// 2. Process the non-uniform indexes : Check if the non-uniform indexes are identical.
//
// 3. Generate the waterfall loop intrinsics and generate the code that is needed to be moved inside the loop for shared
// case. For shared index case, the insert point is after the shared index. Otherwise, the current insert point is used.

// @param nonUniforms : [in/out] The array of 32-bit value that is used to be applied waterfall intrinsics. The value
// will be updated either waterfall_readfirstlane or the new created descriptor pointer inside waterfall loop.
// @param insertPt : The current insert point
// @param instName : Name to give instruction(s)
Value *BuilderImpl::beginWaterfallLoop(SmallVectorImpl<Value *> &nonUniforms, Instruction *insertPt,
                                       const Twine &instName) {
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Waterfall feature disabled
  errs() << "Generating invalid waterfall loop code\n";
  return nonUniformInst;
#else
  if (nonUniforms.empty())
    return nullptr;
  IRBuilder<>::InsertPointGuard guard(*this);
  SetInsertPoint(insertPt);

  Value *sharedIndex = nullptr;
  TraceNonUniformIndex traceNonUniformIndex;

  // Trace down to find if the non-uniform indexes are identical.
  if (nonUniforms.size() > 1) {
    // Non-uniform index calculation
    for (auto index : nonUniforms) {
      assert(isa<PtrToIntInst>(index) && "Expecting PtrToInt instruction");
      Value *descPtr = cast<PtrToIntInst>(index)->getPointerOperand();
      Value *nonUniformIndex = traceNonUniformIndex.run(descPtr);
      if (nonUniformIndex && nonUniformIndex != descPtr)
        traceNonUniformIndex.setNonUniformIndex(nonUniformIndex);
    }

    // Check if the non-uniform indexes are identical.
    if (traceNonUniformIndex.getNumOfNonUniformIndexes() > 1)
      sharedIndex = getSharedIndex(traceNonUniformIndex);
  }

  if (sharedIndex && !sharedIndex->getType()->isIntegerTy(32))
    sharedIndex = nullptr;

  Value *waterfallBegin = ConstantInt::get(getInt32Ty(), 0);
  if (sharedIndex) {
    waterfallBegin =
        CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, getInt32Ty(), {waterfallBegin, sharedIndex}, nullptr);
    Value *readFirstLane = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {getInt32Ty(), getInt32Ty()},
                                           {waterfallBegin, sharedIndex}, nullptr);
    // Clone and emit the instructions that we want to push inside the waterfall loop.
    for (auto [id, nonUniformIndex] : enumerate(traceNonUniformIndex.getNonUniformIndexes())) {
      Instruction *prevInst = insertPt;
      std::map<Instruction *, Instruction *> origClonedValuesMap;
      // Get the instruction chain of the non-uniform index.
      auto instrsToClone = traceNonUniformIndex.getDependentInstructions(nonUniformIndex);
      for (Instruction *origInst : instrsToClone) {
        auto *newInst = origInst->clone();
        newInst->insertBefore(prevInst->getIterator());
        origClonedValuesMap[origInst] = newInst;
        prevInst = newInst;
      }

      // Update the operands of the cloned instructions.
      for (auto [origInst, newInst] : origClonedValuesMap) {
        for (Use &use : newInst->operands()) {
          Value *op = use.get();
          if (auto *opI = dyn_cast<Instruction>(op)) {
            auto it = origClonedValuesMap.find(opI);
            if (it == origClonedValuesMap.end())
              continue;
            Instruction *clonedI = it->second;
            use.set(clonedI);
          }
        }
      }
      nonUniformIndex->replaceUsesWithIf(readFirstLane, [waterfallBegin, readFirstLane, insertPt](Use &use) {
        Instruction *userInst = cast<Instruction>(use.getUser());
        return userInst != waterfallBegin && userInst != readFirstLane &&
               userInst->getParent() == insertPt->getParent() && userInst->comesBefore(insertPt) &&
               !userInst->comesBefore(cast<Instruction>(waterfallBegin));
      });

      // Update the descriptor pointers
      auto &newDescPtr = nonUniforms[id];
      auto descPtr = cast<PtrToIntInst>(nonUniforms[id])->getPointerOperand();
      auto iter = origClonedValuesMap.find(cast<Instruction>(descPtr));
      assert(iter != origClonedValuesMap.end());
      newDescPtr = iter->second;
    }
  } else {
    SmallVector<Value *> waterfallBegins;
    for (auto nonUniformIndex : nonUniforms) {
      waterfallBegin =
          CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, getInt32Ty(), {waterfallBegin, nonUniformIndex}, nullptr);
      waterfallBegins.push_back(waterfallBegin);
    }

    for (auto &nonUniformIndex : nonUniforms) {
      Value *readFirstLane = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {getInt32Ty(), getInt32Ty()},
                                             {waterfallBegins.back(), nonUniformIndex}, nullptr);
      nonUniformIndex = readFirstLane;
    }
    waterfallBegin = waterfallBegins.front();
  }
  return waterfallBegin;

#endif
}

// =====================================================================================================================
//
// Create waterfallEnd at the given non-uniform instruction with the given waterfallBegin
Value *BuilderImpl::endWaterfallLoop(Instruction *nonUniformInst, Value *waterfallBegin) {
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Waterfall feature disabled
  errs() << "Generating invalid waterfall loop code\n";
  return nonUniformInst;
#else
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateWaterfallEnd(mappedArgs[0], passthroughArgs[0]);
  };

  SetInsertPoint(nonUniformInst->getNextNode());
  if (auto intrinsic = dyn_cast<IntrinsicInst>(cast<Instruction>(waterfallBegin)->getNextNode())) {
    if (intrinsic->getIntrinsicID() == Intrinsic::amdgcn_waterfall_begin)
      waterfallBegin = cast<Instruction>(waterfallBegin)->getNextNode();
  }
  return CreateMapToSimpleType(mapFunc, nonUniformInst, waterfallBegin, MapToSimpleMode::SimpleVector);
#endif
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector unary operation
//
// @param value : Input value
// @param callback : Callback function
Value *BuilderImpl::scalarize(Value *value, const std::function<Value *(Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    Value *result0 = callback(CreateExtractElement(value, uint64_t(0)));
    Value *result = PoisonValue::get(FixedVectorType::get(result0->getType(), vecTy->getNumElements()));
    result = CreateInsertElement(result, result0, uint64_t(0));
    for (unsigned idx = 1, end = vecTy->getNumElements(); idx != end; ++idx)
      result = CreateInsertElement(result, callback(CreateExtractElement(value, idx)), idx);
    return result;
  }
  Value *result = callback(value);
  return result;
}

// =====================================================================================================================
// Helper method to scalarize in pairs a possibly vector unary operation. The callback function is called
// with vec2 input, even if the input here is scalar.
//
// @param value : Input value
// @param callback : Callback function
Value *BuilderImpl::scalarizeInPairs(Value *value, const std::function<Value *(Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    Value *inComps = CreateShuffleVector(value, value, ArrayRef<int>{0, 1});
    Value *resultComps = callback(inComps);
    Value *result =
        PoisonValue::get(FixedVectorType::get(resultComps->getType()->getScalarType(), vecTy->getNumElements()));
    result = CreateInsertElement(result, CreateExtractElement(resultComps, uint64_t(0)), uint64_t(0));
    if (vecTy->getNumElements() > 1)
      result = CreateInsertElement(result, CreateExtractElement(resultComps, 1), 1);

    for (int idx = 2, end = vecTy->getNumElements(); idx < end; idx += 2) {
      int indices[2] = {idx, idx + 1};
      inComps = CreateShuffleVector(value, value, indices);
      resultComps = callback(inComps);
      result = CreateInsertElement(result, CreateExtractElement(resultComps, uint64_t(0)), idx);
      if (idx + 1 < end)
        result = CreateInsertElement(result, CreateExtractElement(resultComps, 1), idx + 1);
    }
    return result;
  }

  // For the scalar case, we need to create a vec2.
  Value *inComps = PoisonValue::get(FixedVectorType::get(value->getType(), 2));
  inComps = CreateInsertElement(inComps, value, uint64_t(0));
  inComps = CreateInsertElement(inComps, Constant::getNullValue(value->getType()), 1);
  Value *result = callback(inComps);
  return CreateExtractElement(result, uint64_t(0));
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector binary operation
//
// @param value0 : Input value 0
// @param value1 : Input value 1
// @param callback : Callback function
Value *BuilderImpl::scalarize(Value *value0, Value *value1, const std::function<Value *(Value *, Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value0->getType())) {
    Value *result0 = callback(CreateExtractElement(value0, uint64_t(0)), CreateExtractElement(value1, uint64_t(0)));
    Value *result = PoisonValue::get(FixedVectorType::get(result0->getType(), vecTy->getNumElements()));
    result = CreateInsertElement(result, result0, uint64_t(0));
    for (unsigned idx = 1, end = vecTy->getNumElements(); idx != end; ++idx) {
      result = CreateInsertElement(result,
                                   callback(CreateExtractElement(value0, idx), CreateExtractElement(value1, idx)), idx);
    }
    return result;
  }
  Value *result = callback(value0, value1);
  return result;
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector trinary operation
//
// @param value0 : Input value 0
// @param value1 : Input value 1
// @param value2 : Input value 2
// @param callback : Callback function
Value *BuilderImpl::scalarize(Value *value0, Value *value1, Value *value2,
                              const std::function<Value *(Value *, Value *, Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value0->getType())) {
    Value *result0 = callback(CreateExtractElement(value0, uint64_t(0)), CreateExtractElement(value1, uint64_t(0)),
                              CreateExtractElement(value2, uint64_t(0)));
    Value *result = PoisonValue::get(FixedVectorType::get(result0->getType(), vecTy->getNumElements()));
    result = CreateInsertElement(result, result0, uint64_t(0));
    for (unsigned idx = 1, end = vecTy->getNumElements(); idx != end; ++idx) {
      result = CreateInsertElement(result,
                                   callback(CreateExtractElement(value0, idx), CreateExtractElement(value1, idx),
                                            CreateExtractElement(value2, idx)),
                                   idx);
    }
    return result;
  }
  Value *result = callback(value0, value1, value2);
  return result;
}

// =====================================================================================================================
// Create code to get the lane number within the wave. This depends on whether the shader is wave32 or wave64,
// and thus on the shader stage it is used from.
Value *BuilderImpl::CreateGetLaneNumber() {
  Value *result = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {getInt32(-1), getInt32(0)});
  if (getPipelineState()->getShaderWaveSize(m_shaderStage.value()) == 64)
    result = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {getInt32(-1), result});
  return result;
}
