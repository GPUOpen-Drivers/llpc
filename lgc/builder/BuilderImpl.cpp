/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "PipelineState.h"
#include "TargetInfo.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
//
// @param builderContext : BuilderContext
// @param pipeline : PipelineState (as public superclass Pipeline)
BuilderImpl::BuilderImpl(BuilderContext *builderContext, Pipeline *pipeline)
    : BuilderImplBase(builderContext), BuilderImplArith(builderContext), BuilderImplDesc(builderContext),
      BuilderImplImage(builderContext), BuilderImplInOut(builderContext), BuilderImplMatrix(builderContext),
      BuilderImplMisc(builderContext), BuilderImplSubgroup(builderContext) {
  m_pipelineState = reinterpret_cast<PipelineState *>(pipeline);
  m_pipelineState->setNoReplayer();
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

  const unsigned compCount = product->getType()->getVectorNumElements();
  Value *scalar = CreateExtractElement(product, uint64_t(0));

  for (unsigned i = 1; i < compCount; ++i)
    scalar = CreateFAdd(scalar, CreateExtractElement(product, i));

  scalar->setName(instName);
  return scalar;
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP operations.
bool BuilderImplBase::supportDpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 8;
}

// =====================================================================================================================
// Get whether the context we are building in support the bpermute operation.
bool BuilderImplBase::supportBPermute() const {
  auto gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion().major;
  auto supportBPermute = gfxIp == 8 || gfxIp == 9;
  auto waveSize = getPipelineState()->getShaderWaveSize(getShaderStageFromFunction(GetInsertBlock()->getParent()));
  supportBPermute = supportBPermute || (gfxIp == 10 && waveSize == 32);
  return supportBPermute;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane DPP operations.
bool BuilderImplBase::supportPermLaneDpp() const {
  return getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 10;
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

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.
// This does not use the current insert point; new code is inserted before and after pNonUniformInst.
//
// @param nonUniformInst : The instruction to put in a waterfall loop
// @param operandIdxs : The operand index/indices for non-uniform inputs that need to be uniform
// @param instName : Name to give instruction(s)
Instruction *BuilderImplBase::createWaterfallLoop(Instruction *nonUniformInst, ArrayRef<unsigned> operandIdxs,
                                                  const Twine &instName) {
  assert(operandIdxs.empty() == false);

  // For each non-uniform input, try and trace back through a descriptor load to find the non-uniform index
  // used in it. If that fails, we just use the operand value as the index.
  SmallVector<Value *, 2> nonUniformIndices;
  for (unsigned operandIdx : operandIdxs) {
    Value *nonUniformVal = nonUniformInst->getOperand(operandIdx);
    if (auto call = dyn_cast<CallInst>(nonUniformVal)) {
      if (auto calledFunc = call->getCalledFunction()) {
        if (calledFunc->getName().startswith(lgcName::DescriptorLoadFromPtr)) {
          call = dyn_cast<CallInst>(call->getArgOperand(0)); // The descriptor pointer
          if (call && call->getCalledFunction()->getName().startswith(lgcName::DescriptorIndex))
            nonUniformVal = call->getArgOperand(1); // The index operand
        }
      }
    }
    nonUniformIndices.push_back(nonUniformVal);
  }

  // Save Builder's insert point, and set it to insert new code just before pNonUniformInst.
  auto savedInsertPoint = saveIP();
  SetInsertPoint(nonUniformInst);

  // Get the waterfall index. If there are two indices (image resource+sampler case), join them into
  // a single struct.
  Value *waterfallIndex = nonUniformIndices[0];
  if (nonUniformIndices.size() > 1) {
    assert(nonUniformIndices.size() == 2);
    SmallVector<Type *, 2> indexTys;
    for (Value *nonUniformIndex : nonUniformIndices)
      indexTys.push_back(nonUniformIndex->getType());
    auto waterfallIndexTy = StructType::get(getContext(), indexTys);
    waterfallIndex = UndefValue::get(waterfallIndexTy);
    for (unsigned structIndex = 0; structIndex < nonUniformIndices.size(); ++structIndex)
      waterfallIndex = CreateInsertValue(waterfallIndex, nonUniformIndices[structIndex], structIndex);
  }

  // Start the waterfall loop using the waterfall index.
  Value *waterfallBegin =
      CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, waterfallIndex->getType(), waterfallIndex, nullptr, instName);

  // Scalarize each non-uniform operand of the instruction.
  for (unsigned operandIdx : operandIdxs) {
    Value *desc = nonUniformInst->getOperand(operandIdx);
    auto descTy = desc->getType();
    desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy}, {waterfallBegin, desc}, nullptr,
                           instName);
    if (nonUniformInst->getType()->isVoidTy()) {
      // The buffer/image operation we are waterfalling is a store with no return value. Use
      // llvm.amdgcn.waterfall.last.use on the descriptor.
      desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_last_use, descTy, {waterfallBegin, desc}, nullptr, instName);
    }
    // Replace the descriptor operand in the buffer/image operation.
    nonUniformInst->setOperand(operandIdx, desc);
  }

  Instruction *resultValue = nonUniformInst;

  // End the waterfall loop (as long as pNonUniformInst is not a store with no result).
  if (!nonUniformInst->getType()->isVoidTy()) {
    SetInsertPoint(nonUniformInst->getNextNode());
    SetCurrentDebugLocation(nonUniformInst->getDebugLoc());

    Use *useOfNonUniformInst = nullptr;
    Type *waterfallEndTy = resultValue->getType();
    if (auto vecTy = dyn_cast<VectorType>(waterfallEndTy)) {
      if (vecTy->getElementType()->isIntegerTy(8)) {
        // ISel does not like waterfall.end with vector of i8 type, so cast if necessary.
        assert((vecTy->getNumElements() % 4) == 0);
        waterfallEndTy = getInt32Ty();
        if (vecTy->getNumElements() != 4)
          waterfallEndTy = VectorType::get(getInt32Ty(), vecTy->getNumElements() / 4);
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

    // Replace all uses of pNonUniformInst with the result of this code.
    *useOfNonUniformInst = UndefValue::get(nonUniformInst->getType());
    nonUniformInst->replaceAllUsesWith(resultValue);
    *useOfNonUniformInst = nonUniformInst;
  }

  // Restore Builder's insert point.
  restoreIP(savedInsertPoint);
  return resultValue;
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector unary operation
//
// @param value : Input value
// @param callback : Callback function
Value *BuilderImplBase::scalarize(Value *value, std::function<Value *(Value *)> callback) {
  if (auto vecTy = dyn_cast<VectorType>(value->getType())) {
    Value *result0 = callback(CreateExtractElement(value, uint64_t(0)));
    Value *result = UndefValue::get(VectorType::get(result0->getType(), vecTy->getNumElements()));
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
Value *BuilderImplBase::scalarizeInPairs(Value *value, std::function<Value *(Value *)> callback) {
  if (auto vecTy = dyn_cast<VectorType>(value->getType())) {
    Value *inComps = CreateShuffleVector(value, value, ArrayRef<unsigned>{0, 1});
    Value *resultComps = callback(inComps);
    Value *result = UndefValue::get(VectorType::get(resultComps->getType()->getScalarType(), vecTy->getNumElements()));
    result = CreateInsertElement(result, CreateExtractElement(resultComps, uint64_t(0)), uint64_t(0));
    if (vecTy->getNumElements() > 1)
      result = CreateInsertElement(result, CreateExtractElement(resultComps, 1), 1);

    for (unsigned idx = 2, end = vecTy->getNumElements(); idx < end; idx += 2) {
      unsigned indices[2] = {idx, idx + 1};
      inComps = CreateShuffleVector(value, value, indices);
      resultComps = callback(inComps);
      result = CreateInsertElement(result, CreateExtractElement(resultComps, uint64_t(0)), idx);
      if (idx + 1 < end)
        result = CreateInsertElement(result, CreateExtractElement(resultComps, 1), idx + 1);
    }
    return result;
  }

  // For the scalar case, we need to create a vec2.
  Value *inComps = UndefValue::get(VectorType::get(value->getType(), 2));
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
Value *BuilderImplBase::scalarize(Value *value0, Value *value1, std::function<Value *(Value *, Value *)> callback) {
  if (auto vecTy = dyn_cast<VectorType>(value0->getType())) {
    Value *result0 = callback(CreateExtractElement(value0, uint64_t(0)), CreateExtractElement(value1, uint64_t(0)));
    Value *result = UndefValue::get(VectorType::get(result0->getType(), vecTy->getNumElements()));
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
                                  std::function<Value *(Value *, Value *, Value *)> callback) {
  if (auto vecTy = dyn_cast<VectorType>(value0->getType())) {
    Value *result0 = callback(CreateExtractElement(value0, uint64_t(0)), CreateExtractElement(value1, uint64_t(0)),
                              CreateExtractElement(value2, uint64_t(0)));
    Value *result = UndefValue::get(VectorType::get(result0->getType(), vecTy->getNumElements()));
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
