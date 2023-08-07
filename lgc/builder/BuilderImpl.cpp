/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderImpl.cpp
 * @brief LLPC source file: implementation of lgc::BuilderImpl
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderImpl.h"
#include "lgc/LgcContext.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
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
// Get whether the context we are building in supports DPP operations.
bool BuilderImpl::supportDpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 8;
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP ROW_XMASK operations.
bool BuilderImpl::supportDppRowXmask() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 10;
}

// =====================================================================================================================
// Get whether the context we are building in support the bpermute operation.
bool BuilderImpl::supportWaveWideBPermute() const {
  auto gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion().major;
  auto supportBPermute = gfxIp == 8 || gfxIp == 9;
  auto waveSize = getPipelineState()->getShaderWaveSize(getShaderStage(GetInsertBlock()->getParent()));
  supportBPermute = supportBPermute || (gfxIp >= 10 && waveSize == 32);
  return supportBPermute;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane DPP operations.
bool BuilderImpl::supportPermLaneDpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 10;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane 64 DPP operations.
bool BuilderImpl::supportPermLane64Dpp() const {
  auto gfxip = getPipelineState()->getTargetInfo().getGfxIpVersion().major;
  auto waveSize = getPipelineState()->getShaderWaveSize(getShaderStage(GetInsertBlock()->getParent()));
  return gfxip >= 11 && waveSize == 64;
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
BranchInst *BuilderImpl::createIf(Value *condition, bool wantElse, const Twine &instName) {
  // Create "if" block and move instructions in current block to it.
  BasicBlock *endIfBlock = GetInsertBlock();
  BasicBlock *ifBlock = BasicBlock::Create(getContext(), "", endIfBlock->getParent(), endIfBlock);
  ifBlock->takeName(endIfBlock);
  endIfBlock->setName(instName + ".endif");
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 445640
  // Old version of the code
  ifBlock->getInstList().splice(ifBlock->end(), endIfBlock->getInstList(), endIfBlock->begin(), GetInsertPoint());
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  ifBlock->splice(ifBlock->end(), endIfBlock, endIfBlock->begin(), GetInsertPoint());
#endif

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
// For a non-uniform input, try and trace back through a descriptor load to
// find the non-uniform index used in it. If that fails, we just use the
// operand value as the index.
//
// Note: this code has to cope with relocs as well, this is why we have to
// have a worklist of instructions to trace back
// through. Something like this:
// %1 = call .... @lgc.descriptor.set(...)          ;; Known uniform base
// %2 = call .... @llvm.amdgcn.reloc.constant(...)  ;; Known uniform reloc constant
// %3 = ptrtoint ... %1 to i64
// %4 = zext ... %2 to i64
// %5 = add i64 %3, %4
// %6 = inttoptr i64 %5 to ....
// %7 = bitcast .... %6 to ....
// %8 = getelementptr .... %7, i64 %offset
//
// As long as the base pointer %7 can be traced back to a descriptor set and
// reloc we can infer that it is truly uniform and use the gep index as the waterfall index safely.
//
// @param nonUniformVal : Value representing non-uniform descriptor
// @return : Value representing the non-uniform index
static Value *traceNonUniformIndex(Value *nonUniformVal) {
  auto load = dyn_cast<LoadInst>(nonUniformVal);
  if (!load) {
    // Workarounds that modify image descriptor can be peeped through, i.e.
    //   %baseValue = load <8 x i32>, <8 x i32> addrspace(4)* %..., align 16
    //   %rawElement = extractelement <8 x i32> %baseValue, i64 6
    //   %updatedElement = and i32 %rawElement, -1048577
    //   %nonUniform = insertelement <8 x i32> %baseValue, i32 %updatedElement, i64 6
    auto insert = dyn_cast<InsertElementInst>(nonUniformVal);
    if (!insert)
      return nonUniformVal;

    load = dyn_cast<LoadInst>(insert->getOperand(0));
    if (!load)
      return nonUniformVal;

    // We found the load, but must verify the chain.
    // Consider updatedElement as a generic instruction or constant.
    if (auto updatedElement = dyn_cast<Instruction>(insert->getOperand(1))) {
      for (Value *operand : updatedElement->operands()) {
        if (auto extract = dyn_cast<ExtractElementInst>(operand)) {
          // Only dynamic value must be ExtractElementInst based on load.
          if (dyn_cast<LoadInst>(extract->getOperand(0)) != load)
            return nonUniformVal;
        } else if (!isa<Constant>(operand)) {
          return nonUniformVal;
        }
      }
    } else if (!isa<Constant>(insert->getOperand(1))) {
      return nonUniformVal;
    }
  }

  SmallVector<Value *, 2> worklist;
  Value *base = load->getOperand(0);
  Value *index = nullptr;

  // Loop until a descriptor table reference or unexpected operation is reached.
  // In the worst case this may visit all instructions in a function.
  for (;;) {
    if (auto bitcast = dyn_cast<BitCastInst>(base)) {
      base = bitcast->getOperand(0);
      continue;
    }
    if (auto gep = dyn_cast<GetElementPtrInst>(base)) {
      if (gep->hasAllConstantIndices()) {
        base = gep->getPointerOperand();
        continue;
      }
      // Variable GEP, to provide the index for the waterfall.
      if (index || gep->getNumIndices() != 1)
        break;
      index = *gep->idx_begin();
      base = gep->getPointerOperand();
      continue;
    }
    if (auto extract = dyn_cast<ExtractValueInst>(base)) {
      if (extract->getIndices().size() == 1 && extract->getIndices()[0] == 0) {
        base = extract->getAggregateOperand();
        continue;
      }
      break;
    }
    if (auto insert = dyn_cast<InsertValueInst>(base)) {
      if (insert->getIndices()[0] != 0) {
        base = insert->getAggregateOperand();
        continue;
      }
      if (insert->getIndices().size() == 1 && insert->getIndices()[0] == 0) {
        base = insert->getInsertedValueOperand();
        continue;
      }
      break;
    }
    if (auto intToPtr = dyn_cast<IntToPtrInst>(base)) {
      base = intToPtr->getOperand(0);
      continue;
    }
    if (auto ptrToInt = dyn_cast<PtrToIntInst>(base)) {
      base = ptrToInt->getOperand(0);
      continue;
    }
    if (auto zExt = dyn_cast<ZExtInst>(base)) {
      base = zExt->getOperand(0);
      continue;
    }
    if (auto call = dyn_cast<CallInst>(base)) {
      if (index) {
        if (auto calledFunc = call->getCalledFunction()) {
          if (calledFunc->getName().startswith(lgcName::DescriptorTableAddr) ||
              calledFunc->getName().startswith("llvm.amdgcn.reloc.constant")) {
            if (!worklist.empty()) {
              base = worklist.pop_back_val();
              continue;
            }
            nonUniformVal = index;
            break;
          }
        }
      }
    }
    if (auto addInst = dyn_cast<Instruction>(base)) {
      // In this case we have to trace back both operands
      // Set one to base for continued processing and put the other onto the worklist
      // Give up if the worklist already has an entry - too complicated
      if (addInst->isBinaryOp() && addInst->getOpcode() == Instruction::BinaryOps::Add) {
        if (!worklist.empty())
          break;
        base = addInst->getOperand(0);
        worklist.push_back(addInst->getOperand(1));
        continue;
      }
    }
    break;
  }

  return nonUniformVal;
}

// =====================================================================================================================
// Test whether two instructions are identical
// or are the same operation on identical operands.
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
#endif

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.
// This does not use the current insert point; new code is inserted before and after nonUniformInst.
//
// @param nonUniformInst : The instruction to put in a waterfall loop
// @param operandIdxs : The operand index/indices for non-uniform inputs that need to be uniform
// @param scalarizeDescriptorLoads : Attempt to scalarize descriptor loads
// @param useVgprForOperands : Non-uniform inputs should be put in VGPRs
// @param instName : Name to give instruction(s)
Instruction *BuilderImpl::createWaterfallLoop(Instruction *nonUniformInst, ArrayRef<unsigned> operandIdxs,
                                              bool scalarizeDescriptorLoads, bool useVgprForOperands,
                                              const Twine &instName) {
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Waterfall feature disabled
  errs() << "Generating invalid waterfall loop code\n";
  return nonUniformInst;
#else
  assert(operandIdxs.empty() == false);

  SmallVector<Value *, 2> nonUniformIndices;
  for (unsigned operandIdx : operandIdxs) {
    Value *nonUniformIndex = traceNonUniformIndex(nonUniformInst->getOperand(operandIdx));
    nonUniformIndices.push_back(nonUniformIndex);
  }

  // For any index that is 64 bit, change it back to 32 bit for comparison at the top of the
  // waterfall loop.
  for (Value *&nonUniformVal : nonUniformIndices) {
    if (nonUniformVal->getType()->isIntegerTy(64)) {
      auto sExt = dyn_cast<SExtInst>(nonUniformVal);
      // 64-bit index may already be formed from extension of 32-bit value.
      if (sExt && sExt->getOperand(0)->getType()->isIntegerTy(32)) {
        nonUniformVal = sExt->getOperand(0);
      } else {
        nonUniformVal = CreateTrunc(nonUniformVal, getInt32Ty());
      }
    }
  }

  // Find first index instruction and check if index instructions are identical.
  Instruction *firstIndexInst = nullptr;
  if (scalarizeDescriptorLoads) {
    // FIXME: these do not actually need to be identical if we introduce multiple waterfall
    // begin and readlane intrinsics for these.
    bool identicalIndexes = true;
    for (Value *nonUniformVal : nonUniformIndices) {
      Instruction *nuInst = dyn_cast<Instruction>(nonUniformVal);
      // Note: parent check here guards use of comesBefore below
      if (!nuInst || (firstIndexInst && !instructionsEqual(nuInst, firstIndexInst)) ||
          (firstIndexInst && nuInst->getParent() != firstIndexInst->getParent())) {
        identicalIndexes = false;
        break;
      }
      if (!firstIndexInst || nuInst->comesBefore(firstIndexInst))
        firstIndexInst = nuInst;
    }

    // Ensure we do not create a waterfall across blocks.
    // FIXME: we could use dominator check to allow scalarizing descriptor loads on multi-block spans;
    // however, this also requires backend support for multi-block waterfalls to be implemented.
    if (!identicalIndexes || !firstIndexInst ||
        (firstIndexInst && firstIndexInst->getParent() != nonUniformInst->getParent()))
      scalarizeDescriptorLoads = false;
  }

  // Save Builder's insert point
  IRBuilder<>::InsertPointGuard guard(*this);

  Value *waterfallBegin;
  if (scalarizeDescriptorLoads) {
    // Attempt to scalarize descriptor loads.
    assert(firstIndexInst);
    CallInst *firstCallInst = dyn_cast<CallInst>(firstIndexInst);
    if (firstCallInst && firstCallInst->getIntrinsicID() == Intrinsic::amdgcn_waterfall_readfirstlane) {
      // Descriptor loads are already inside a waterfall.
      waterfallBegin = firstCallInst->getArgOperand(0);
    } else {
      // Begin waterfall loop just after shared index is computed.
      // This places all dependent instructions within the waterfall loop, including descriptor loads.
      auto descTy = firstIndexInst->getType();
      SetInsertPoint(firstIndexInst->getNextNonDebugInstruction(false));
      waterfallBegin = ConstantInt::get(getInt32Ty(), 0);
      waterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, descTy, {waterfallBegin, firstIndexInst},
                                       nullptr, instName);

      // Scalarize shared index.
      Value *desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy},
                                    {waterfallBegin, firstIndexInst}, nullptr, instName);

      // Replace all references to shared index within the waterfall loop with scalarized index.
      // (Note: this includes the non-uniform instruction itself.)
      // Loads using scalarized index will become scalar loads.
      for (Value *otherNonUniformVal : nonUniformIndices) {
        otherNonUniformVal->replaceUsesWithIf(desc, [desc, waterfallBegin, nonUniformInst](Use &U) {
          Instruction *userInst = cast<Instruction>(U.getUser());
          return U.getUser() != waterfallBegin && U.getUser() != desc &&
                 userInst->getParent() == nonUniformInst->getParent() &&
                 (userInst == nonUniformInst || userInst->comesBefore(nonUniformInst));
        });
      }
    }
  } else {
    // Insert new code just before nonUniformInst.
    SetInsertPoint(nonUniformInst);

    // The first begin contains a null token for the previous token argument
    waterfallBegin = ConstantInt::get(getInt32Ty(), 0);
    for (auto nonUniformVal : nonUniformIndices) {
      // Start the waterfall loop using the waterfall index.
      waterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, nonUniformVal->getType(),
                                       {waterfallBegin, nonUniformVal}, nullptr, instName);
    }

    // Scalarize each non-uniform operand of the instruction.
    for (unsigned operandIdx : operandIdxs) {
      Value *desc = nonUniformInst->getOperand(operandIdx);
      auto descTy = desc->getType();
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 463892
      // Old version of the code
#else
      // When the non-uniform use is in a VGPR, we can save a v_mov by not inserting the amdgcn_waterfall_readfirstlane
      if (!useVgprForOperands)
#endif
      desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy}, {waterfallBegin, desc},
                             nullptr, instName);
      if (nonUniformInst->getType()->isVoidTy()) {
        // The buffer/image operation we are waterfalling is a store with no return value. Use
        // llvm.amdgcn.waterfall.last.use on the descriptor.
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 463892
        // Old version of the code
        desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_last_use, descTy, {waterfallBegin, desc}, nullptr, instName);
#else
        desc = CreateIntrinsic(useVgprForOperands ? Intrinsic::amdgcn_waterfall_last_use_vgpr
                                                  : Intrinsic::amdgcn_waterfall_last_use,
                               descTy, {waterfallBegin, desc}, nullptr, instName);
#endif
      }
      // Replace the descriptor operand in the buffer/image operation.
      nonUniformInst->setOperand(operandIdx, desc);
    }
  }

  Instruction *resultValue = nonUniformInst;

  // End the waterfall loop (as long as nonUniformInst is not a store with no result).
  if (!nonUniformInst->getType()->isVoidTy()) {
    SetInsertPoint(nonUniformInst->getNextNode());
    SetCurrentDebugLocation(nonUniformInst->getDebugLoc());

    Use *useOfNonUniformInst = nullptr;
    Type *waterfallEndTy = resultValue->getType();
    if (auto vecTy = dyn_cast<FixedVectorType>(waterfallEndTy)) {
      if (vecTy->getElementType()->isIntegerTy(8)) {
        // ISel does not like waterfall.end with vector of i8 type, so cast if necessary.
        assert((vecTy->getNumElements() % 4) == 0);
        waterfallEndTy = getInt32Ty();
        if (vecTy->getNumElements() != 4)
          waterfallEndTy = FixedVectorType::get(getInt32Ty(), vecTy->getNumElements() / 4);
        resultValue = cast<Instruction>(CreateBitCast(resultValue, waterfallEndTy, instName));
        useOfNonUniformInst = &resultValue->getOperandUse(0);
      }
    }
    resultValue = CreateIntrinsic(Intrinsic::amdgcn_waterfall_end, waterfallEndTy, {waterfallBegin, resultValue},
                                  nullptr, instName);
    if (!useOfNonUniformInst)
      useOfNonUniformInst = &resultValue->getOperandUse(1);
    if (waterfallEndTy != nonUniformInst->getType())
      resultValue = cast<Instruction>(CreateBitCast(resultValue, nonUniformInst->getType(), instName));

    // Replace all uses of nonUniformInst with the result of this code.
    *useOfNonUniformInst = PoisonValue::get(nonUniformInst->getType());
    nonUniformInst->replaceAllUsesWith(resultValue);
    *useOfNonUniformInst = nonUniformInst;
  }

  return resultValue;
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
  if (getPipelineState()->getShaderWaveSize(m_shaderStage) == 64)
    result = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {getInt32(-1), result});
  return result;
}
