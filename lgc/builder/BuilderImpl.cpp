/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "BuilderImpl.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
//
// @param builderContext : LgcContext
// @param pipeline : PipelineState (as public superclass Pipeline)
BuilderImpl::BuilderImpl(LgcContext *builderContext, Pipeline *pipeline)
    : BuilderImplBase(builderContext), ArithBuilder(builderContext), DescBuilder(builderContext),
      ImageBuilder(builderContext), InOutBuilder(builderContext), MatrixBuilder(builderContext),
      MiscBuilder(builderContext), SubgroupBuilder(builderContext) {
  m_pipelineState = reinterpret_cast<PipelineState *>(pipeline);
}

// =====================================================================================================================
// Get the ShaderModes object.
ShaderModes *BuilderImplBase::getShaderModes() {
  return m_pipelineState->getShaderModes();
}

// =====================================================================================================================
// Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
//
// @param vector1 : The float vector 1
// @param vector2 : The float vector 2
// @param instName : Name to give instruction(s)
Value *BuilderImplBase::CreateDotProduct(Value *const vector1, Value *const vector2, const Twine &instName) {
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
// where available.
// Use a value of 0 for no accumulation and the value type is consistent with the result type. The result is saturated
// if there is an accumulator. The component type of input vectors can have 8-bit/16-bit/32-bit and i32/i16/i8 result.
//
// @param vector1 : The integer vector 1
// @param vector2 : The integer vector 2
// @param accumulator : The accumulator to the scalar of dot product
// @param flags : Bit 0 is "first vector is signed" and bit 1 is "second vector is signed"
// @param instName : Name to give instruction(s)
Value *BuilderImplBase::CreateIntegerDotProduct(Value *vector1, Value *vector2, Value *accumulator, unsigned flags,
                                                const Twine &instName) {
  Type *inputTy = vector1->getType();
  assert(inputTy->isVectorTy() && inputTy->getScalarType()->isIntegerTy());
  Value *scalar = nullptr;
  Type *outputTy = accumulator->getType();

  // The component of Vector 2 can be signed or unsigned
  const bool isSigned = (flags & FirstVectorSigned);
  // The mixed signed/unsigned is that component of Vector 1 is treated as signed and component of Vector 2 is treated
  // as unsigned.
  const bool isMixed = (flags == FirstVectorSigned);

  const unsigned compBitWidth = inputTy->getScalarSizeInBits();
  assert(compBitWidth >= 8 && compBitWidth <= 64);

  auto &supportIntegerDotFlag = getPipelineState()->getTargetInfo().getGpuProperty().supportIntegerDotFlag;
  // Check if the component bitwidth of vectors and the signedness of component of vectors are both supported by HW.
  const bool isSupportCompBitwidth = (supportIntegerDotFlag.compBitwidth16 && compBitWidth == 16) ||
                                     (supportIntegerDotFlag.compBitwidth8 && compBitWidth == 8);
  const bool isSupportSignedness =
      isMixed ? supportIntegerDotFlag.diffSignedness : supportIntegerDotFlag.sameSignedness;
  const bool hasHwNativeSupport = isSupportCompBitwidth && isSupportSignedness;

  // NOTE: For opcodes with an accumulator, the spec said "If any of the multiplications or additions, with the
  // exception of the final accumulation, overflow or underflow, the result of the instruction is undefined". For
  // opcodes without accumulator, the spec said "The resulting value will equal the low-order N bits of the correct
  // result R, where N is the result width and R is computed with enough precision to avoid overflow and underflow".
  const bool hasAccumulator = !(isa<ConstantInt>(accumulator) && cast<ConstantInt>(accumulator)->isNullValue());
  const unsigned compCount = cast<FixedVectorType>(inputTy)->getNumElements();
  Type *targetTy = hasHwNativeSupport ? getInt32Ty() : getInt64Ty();
  accumulator = isSigned ? CreateSExt(accumulator, targetTy) : CreateZExt(accumulator, targetTy);
  if (!hasHwNativeSupport) {
    // Emulate dot product with no HW support cases
    scalar = getIntN(targetTy->getScalarSizeInBits(), 0);

    for (unsigned elemIdx = 0; elemIdx < compCount; ++elemIdx) {
      Value *elem1 = CreateExtractElement(vector1, elemIdx);
      elem1 = isSigned ? CreateSExt(elem1, targetTy) : CreateZExt(elem1, targetTy);
      Value *elem2 = CreateExtractElement(vector2, elemIdx);
      elem2 = (isSigned && !isMixed) ? CreateSExt(elem2, targetTy) : CreateZExt(elem2, targetTy);
      Value *product = CreateMul(elem1, elem2);
      scalar = CreateAdd(product, scalar);
    }
    if (hasAccumulator) {
      if (compBitWidth == 8 || compBitWidth == 16) {
        scalar = CreateTrunc(scalar, getInt32Ty());
        accumulator = CreateTrunc(accumulator, getInt32Ty());
        Intrinsic::ID addIntrinsic = isSigned ? Intrinsic::sadd_sat : Intrinsic::uadd_sat;
        scalar = CreateBinaryIntrinsic(addIntrinsic, scalar, accumulator, nullptr, instName);
      } else {
        scalar = CreateAdd(scalar, accumulator);
      }
    }
  } else {
    // <4xi8>, <3xi8>, <2xi8> are native supported by using v_dot4_i32_i8 or v_dot4_u32_u8
    // <2xi16> is native supported by using v_dot2_i32_i16 and v_dot2_u32_u16
    // <3xi16>, <4xi16> will be split up with two sequences of v_dot2 and a final saturation add
    Value *input1 = vector1;
    Value *input2 = vector2;

    bool isDot4 = (compBitWidth == 8);
    Value *clamp = hasAccumulator ? getTrue() : getFalse();

    if (isDot4) {
      if (compCount < 4) {
        // Extend <3xi8> or <2xi8> to <4xi8>
        input1 = CreateShuffleVector(input1, Constant::getNullValue(inputTy), ArrayRef<int>({0, 1, 2, 3}));
        input2 = CreateShuffleVector(input2, Constant::getNullValue(inputTy), ArrayRef<int>({0, 1, 2, 3}));
      }
      // Cast <4xi8> to i32
      input1 = CreateBitCast(input1, getInt32Ty());
      input2 = CreateBitCast(input2, getInt32Ty());
      auto intrinsicDot4 = isSigned ? Intrinsic::amdgcn_sdot4 : Intrinsic::amdgcn_udot4;
      { scalar = CreateIntrinsic(intrinsicDot4, {}, {input1, input2, accumulator, clamp}, nullptr, instName); }
    } else {
      auto intrinsicDot2 = isSigned ? Intrinsic::amdgcn_sdot2 : Intrinsic::amdgcn_udot2;
      if (compCount == 2) {
        scalar = CreateIntrinsic(intrinsicDot2, {}, {input1, input2, accumulator, clamp}, nullptr, instName);
      } else {
        Value *intermediateRes = nullptr;
        scalar = nullptr;
        if (compCount == 3) {
          // Split <3xi16> up with an integer multiplication, a 16-bit integer dot product
          Value *w1 = CreateExtractElement(input1, 2);
          Value *w2 = CreateExtractElement(input2, 2);
          w1 = isSigned ? CreateSExt(w1, getInt32Ty()) : CreateZExt(w1, getInt32Ty());
          w2 = isSigned ? CreateSExt(w2, getInt32Ty()) : CreateZExt(w2, getInt32Ty());
          intermediateRes = CreateMul(w1, w2);

          input1 = CreateShuffleVector(input1, Constant::getNullValue(inputTy), ArrayRef<int>({0, 1}));
          input2 = CreateShuffleVector(input2, Constant::getNullValue(inputTy), ArrayRef<int>({0, 1}));
          scalar =
              CreateIntrinsic(intrinsicDot2, {}, {input1, input2, intermediateRes, getInt1(false)}, nullptr, instName);
        } else {
          assert(compCount == 4);
          // Split <4xi16> up with two 16-bit integer dot product
          Value *vec1 = CreateShuffleVector(input1, Constant::getNullValue(inputTy), ArrayRef<int>({0, 1}));
          Value *vec2 = CreateShuffleVector(input2, Constant::getNullValue(inputTy), ArrayRef<int>({0, 1}));
          intermediateRes =
              CreateIntrinsic(intrinsicDot2, {}, {vec1, vec2, getInt32(0), getInt1(false)}, nullptr, instName);

          input1 = CreateShuffleVector(input1, Constant::getNullValue(inputTy), ArrayRef<int>({2, 3}));
          input2 = CreateShuffleVector(input2, Constant::getNullValue(inputTy), ArrayRef<int>({2, 3}));
          scalar =
              CreateIntrinsic(intrinsicDot2, {}, {input1, input2, intermediateRes, getInt1(false)}, nullptr, instName);
        }
        // Add a saturation add if required.
        if (hasAccumulator) {
          Intrinsic::ID addIntrinsic = isSigned ? Intrinsic::sadd_sat : Intrinsic::uadd_sat;
          scalar = CreateBinaryIntrinsic(addIntrinsic, scalar, accumulator, nullptr, instName);
        }
      }
    }
  }

  // Do clamp if it has an accumulator
  // NOTE: 32-bit result is a saturating add result. Do the manual saturating for 8-bit/16-bit result.
  if (scalar->getType() != outputTy) {
    if (hasAccumulator) {
      const unsigned bitWidth = outputTy->getScalarSizeInBits();
      auto unsignedMax = (2ULL << (bitWidth - 1)) - 1;
      auto signedMax = unsignedMax >> 1;
      auto signedMin = -1ULL - signedMax;

      Value *minimum = nullptr, *maximum = nullptr;
      Value *isUnderflow = nullptr, *isOverflow = nullptr;
      if (isSigned) {
        scalar = CreateSExt(scalar, getInt64Ty());
        minimum = ConstantInt::getSigned(getInt64Ty(), signedMin);
        maximum = ConstantInt::getSigned(getInt64Ty(), signedMax);
        isUnderflow = CreateICmpSLT(scalar, minimum);
        isOverflow = CreateICmpSGT(scalar, maximum);
      } else {
        scalar = CreateZExt(scalar, getInt64Ty());
        minimum = getInt64(0);
        maximum = getInt64(unsignedMax);
        isUnderflow = CreateICmpULT(scalar, minimum);
        isOverflow = CreateICmpUGT(scalar, maximum);
      }
      scalar = CreateSelect(isUnderflow, minimum, scalar);
      scalar = CreateSelect(isOverflow, maximum, scalar);
      scalar = CreateTrunc(scalar, outputTy);
    } else {
      scalar = isSigned ? CreateSExtOrTrunc(scalar, outputTy) : CreateZExtOrTrunc(scalar, outputTy);
    }
  }

  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP operations.
bool BuilderImplBase::supportDpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 8;
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP ROW_XMASK operations.
bool BuilderImplBase::supportDppRowXmask() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 10;
}

// =====================================================================================================================
// Get whether the context we are building in support the bpermute operation.
bool BuilderImplBase::supportBPermute() const {
  auto gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion().major;
  auto supportBPermute = gfxIp == 8 || gfxIp == 9;
  auto waveSize = getPipelineState()->getShaderWaveSize(getShaderStage(GetInsertBlock()->getParent()));
  supportBPermute = supportBPermute || (gfxIp == 10 && waveSize == 32);
  return supportBPermute;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane DPP operations.
bool BuilderImplBase::supportPermLaneDpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 10;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane 64 DPP operations.
bool BuilderImplBase::supportPermLane64Dpp() const {
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
BranchInst *BuilderImplBase::createIf(Value *condition, bool wantElse, const Twine &instName) {
  // Create "if" block and move instructions in current block to it.
  BasicBlock *endIfBlock = GetInsertBlock();
  BasicBlock *ifBlock = BasicBlock::Create(getContext(), "", endIfBlock->getParent(), endIfBlock);
  ifBlock->takeName(endIfBlock);
  endIfBlock->setName(instName + ".endif");
  ifBlock->getInstList().splice(ifBlock->end(), endIfBlock->getInstList(), endIfBlock->begin(), GetInsertPoint());

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
// @param instName : Name to give instruction(s)
Instruction *BuilderImplBase::createWaterfallLoop(Instruction *nonUniformInst, ArrayRef<unsigned> operandIdxs,
                                                  bool scalarizeDescriptorLoads, const Twine &instName) {
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
  bool identicalIndexes = true;
  for (Value *nonUniformVal : nonUniformIndices) {
    Instruction *nuInst = dyn_cast<Instruction>(nonUniformVal);
    if (!nuInst || (firstIndexInst && !instructionsEqual(nuInst, firstIndexInst))) {
      identicalIndexes = false;
      break;
    }
    if (!firstIndexInst || nuInst->comesBefore(firstIndexInst))
      firstIndexInst = nuInst;
  }

  // Save Builder's insert point
  auto savedInsertPoint = saveIP();

  Value *waterfallBegin;
  if (scalarizeDescriptorLoads && firstIndexInst && identicalIndexes) {
    // Attempt to scalarize descriptor loads.

    // Begin waterfall loop just after shared index is computed.
    // This places all dependent instructions within the waterfall loop, including descriptor loads.
    auto nonUniformVal = cast<Value>(firstIndexInst);
    SetInsertPoint(firstIndexInst->getNextNonDebugInstruction(false));
    waterfallBegin = ConstantInt::get(getInt32Ty(), 0);
    waterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, nonUniformVal->getType(),
                                     {waterfallBegin, nonUniformVal}, nullptr, instName);

    // Scalarize shared index.
    auto descTy = nonUniformVal->getType();
    Value *desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy},
                                  {waterfallBegin, nonUniformVal}, nullptr, instName);

    // Replace all references to shared index within the waterfall loop with scalarized index.
    // (Note: this includes the non-uniform instruction itself.)
    // Loads using scalarized index will become scalar loads.
    for (Value *otherNonUniformVal : nonUniformIndices) {
      otherNonUniformVal->replaceUsesWithIf(desc, [desc, waterfallBegin, nonUniformInst](Use &U) {
        Instruction *userInst = cast<Instruction>(U.getUser());
        return U.getUser() != waterfallBegin && U.getUser() != desc &&
               (userInst->comesBefore(nonUniformInst) || userInst == nonUniformInst);
      });
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
      desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy}, {waterfallBegin, desc},
                             nullptr, instName);
      if (nonUniformInst->getType()->isVoidTy()) {
        // The buffer/image operation we are waterfalling is a store with no return value. Use
        // llvm.amdgcn.waterfall.last.use on the descriptor.
        desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_last_use, descTy, {waterfallBegin, desc}, nullptr, instName);
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
    *useOfNonUniformInst = UndefValue::get(nonUniformInst->getType());
    nonUniformInst->replaceAllUsesWith(resultValue);
    *useOfNonUniformInst = nonUniformInst;
  }

  // Restore Builder's insert point.
  restoreIP(savedInsertPoint);
  return resultValue;
#endif
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector unary operation
//
// @param value : Input value
// @param callback : Callback function
Value *BuilderImplBase::scalarize(Value *value, const std::function<Value *(Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    Value *result0 = callback(CreateExtractElement(value, uint64_t(0)));
    Value *result = UndefValue::get(FixedVectorType::get(result0->getType(), vecTy->getNumElements()));
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
Value *BuilderImplBase::scalarizeInPairs(Value *value, const std::function<Value *(Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    Value *inComps = CreateShuffleVector(value, value, ArrayRef<int>{0, 1});
    Value *resultComps = callback(inComps);
    Value *result =
        UndefValue::get(FixedVectorType::get(resultComps->getType()->getScalarType(), vecTy->getNumElements()));
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
  Value *inComps = UndefValue::get(FixedVectorType::get(value->getType(), 2));
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
Value *BuilderImplBase::scalarize(Value *value0, Value *value1,
                                  const std::function<Value *(Value *, Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value0->getType())) {
    Value *result0 = callback(CreateExtractElement(value0, uint64_t(0)), CreateExtractElement(value1, uint64_t(0)));
    Value *result = UndefValue::get(FixedVectorType::get(result0->getType(), vecTy->getNumElements()));
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
Value *BuilderImplBase::scalarize(Value *value0, Value *value1, Value *value2,
                                  const std::function<Value *(Value *, Value *, Value *)> &callback) {
  if (auto vecTy = dyn_cast<FixedVectorType>(value0->getType())) {
    Value *result0 = callback(CreateExtractElement(value0, uint64_t(0)), CreateExtractElement(value1, uint64_t(0)),
                              CreateExtractElement(value2, uint64_t(0)));
    Value *result = UndefValue::get(FixedVectorType::get(result0->getType(), vecTy->getNumElements()));
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
Value *BuilderImplBase::CreateGetLaneNumber() {
  Value *result = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {getInt32(-1), getInt32(0)});
  if (getPipelineState()->getShaderWaveSize(m_shaderStage) == 64)
    result = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {getInt32(-1), result});
  return result;
}
