/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerAlgebraTransform.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerAlgebraTransform.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerAlgebraTransform.h"
#include "SPIRVInternal.h"
#include "hex_float.h"
#include "llpcContext.h"
#include "lgc/llpcBuilder.h"
#include "lgc/llpcPipeline.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"

#define DEBUG_TYPE "llpc-spirv-lower-algebra-transform"

using namespace lgc;
using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Initializes static members.
char SpirvLowerAlgebraTransform::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering opertions for algebraic transformation.
ModulePass *createSpirvLowerAlgebraTransform(bool enableConstFolding, bool enableFloatOpt) {
  return new SpirvLowerAlgebraTransform(enableConstFolding, enableFloatOpt);
}

// =====================================================================================================================
//
// @param enableConstFolding : Whether enable constant folding
// @param enableFloatOpt : Whether enable floating point optimization
SpirvLowerAlgebraTransform::SpirvLowerAlgebraTransform(bool enableConstFolding, bool enableFloatOpt)
    : SpirvLower(ID), m_enableConstFolding(enableConstFolding), m_enableFloatOpt(enableFloatOpt), m_changed(false) {}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerAlgebraTransform::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Algebra-Transform\n");

  SpirvLower::init(&module);
  m_changed = false;

  auto &commonShaderMode = m_context->getBuilder()->getCommonShaderMode();
  m_fp16DenormFlush = commonShaderMode.fp16DenormMode == FpDenormMode::FlushOut ||
                      commonShaderMode.fp16DenormMode == FpDenormMode::FlushInOut;
  m_fp32DenormFlush = commonShaderMode.fp32DenormMode == FpDenormMode::FlushOut ||
                      commonShaderMode.fp32DenormMode == FpDenormMode::FlushInOut;
  m_fp64DenormFlush = commonShaderMode.fp64DenormMode == FpDenormMode::FlushOut ||
                      commonShaderMode.fp64DenormMode == FpDenormMode::FlushInOut;
  m_fp16Rtz = commonShaderMode.fp16RoundMode == FpRoundMode::Zero;

  if (m_enableConstFolding && (m_fp16DenormFlush || m_fp32DenormFlush || m_fp64DenormFlush)) {
    // Do constant folding if we need flush denorm to zero.
    auto &targetLibInfo = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*m_entryPoint);
    auto &dataLayout = m_module->getDataLayout();

    for (auto &block : *m_entryPoint) {
      for (auto instIter = block.begin(), instEnd = block.end(); instIter != instEnd;) {
        Instruction *inst = &(*instIter++);

        // DCE instruction if trivially dead.
        if (isInstructionTriviallyDead(inst, &targetLibInfo)) {
          LLVM_DEBUG(dbgs() << "Algebriac transform: DCE: " << *inst << '\n');
          inst->eraseFromParent();
          m_changed = true;
          continue;
        }

        // Skip Constant folding if it isn't floating point const expression
        auto destType = inst->getType();
        if (inst->use_empty() || inst->getNumOperands() == 0 || !destType->isFPOrFPVectorTy() ||
            !isa<Constant>(inst->getOperand(0)))
          continue;

        // ConstantProp instruction if trivially constant.
        if (Constant *constVal = ConstantFoldInstruction(inst, dataLayout, &targetLibInfo)) {
          LLVM_DEBUG(dbgs() << "Algebriac transform: constant folding: " << *constVal << " from: " << *inst << '\n');
          if ((destType->isHalfTy() && m_fp16DenormFlush) || (destType->isFloatTy() && m_fp32DenormFlush) ||
              (destType->isDoubleTy() && m_fp64DenormFlush)) {
            // Replace denorm value with zero
            if (constVal->isFiniteNonZeroFP() && !constVal->isNormalFP())
              constVal = ConstantFP::get(destType, 0.0);
          }

          inst->replaceAllUsesWith(constVal);
          if (isInstructionTriviallyDead(inst, &targetLibInfo))
            inst->eraseFromParent();

          m_changed = true;
          continue;
        }
      }
    }
  }

  if (m_enableFloatOpt)
    visit(m_module);

  return m_changed;
}

// =====================================================================================================================
// Checks desired denormal flush behavior and inserts llvm.canonicalize.
//
// @param inst : Instruction to flush denormals if needed
void SpirvLowerAlgebraTransform::flushDenormIfNeeded(Instruction *inst) {
  auto destTy = inst->getType();
  if ((destTy->getScalarType()->isHalfTy() && m_fp16DenormFlush) ||
      (destTy->getScalarType()->isFloatTy() && m_fp32DenormFlush) ||
      (destTy->getScalarType()->isDoubleTy() && m_fp64DenormFlush)) {
    // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
    auto builder = m_context->getBuilder();
    builder->SetInsertPoint(inst->getNextNode());
    auto canonical = builder->CreateIntrinsic(Intrinsic::canonicalize, destTy, UndefValue::get(destTy));

    inst->replaceAllUsesWith(canonical);
    canonical->setArgOperand(0, inst);
    m_changed = true;
  }
}

// =====================================================================================================================
// Visits unary operator instruction.
//
// @param unaryOp : Unary operator instruction
void SpirvLowerAlgebraTransform::visitUnaryOperator(UnaryOperator &unaryOp) {
  if (unaryOp.getOpcode() == Instruction::FNeg)
    flushDenormIfNeeded(&unaryOp);
}

// =====================================================================================================================
// Visits binary operator instruction.
//
// @param binaryOp : Binary operator instruction
void SpirvLowerAlgebraTransform::visitBinaryOperator(BinaryOperator &binaryOp) {
  Instruction::BinaryOps opCode = binaryOp.getOpcode();

  auto src1 = binaryOp.getOperand(0);
  auto src2 = binaryOp.getOperand(1);
  bool src1IsConstZero =
      isa<ConstantAggregateZero>(src1) || (isa<ConstantFP>(src1) && cast<ConstantFP>(src1)->isZero());
  bool src2IsConstZero =
      isa<ConstantAggregateZero>(src2) || (isa<ConstantFP>(src2) && cast<ConstantFP>(src2)->isZero());
  Value *dest = nullptr;

  if (opCode == Instruction::FAdd) {
    // Recursively find backward if the operand "does not" specify contract flags
    auto fastMathFlags = binaryOp.getFastMathFlags();
    if (fastMathFlags.allowContract()) {
      bool hasNoContract = isOperandNoContract(src1) || isOperandNoContract(src2);
      bool allowContract = !hasNoContract;

      // Reassocation and contract should be same
      fastMathFlags.setAllowReassoc(allowContract);
      fastMathFlags.setAllowContract(allowContract);
      binaryOp.copyFastMathFlags(fastMathFlags);
    }
  } else if (opCode == Instruction::FSub) {
    if (src1IsConstZero) {
      // NOTE: Source1 is constant zero, we might be performing FNEG operation. This will be optimized
      // by backend compiler with sign bit reversed via XOR. Check floating-point controls.
      flushDenormIfNeeded(&binaryOp);
    }
  } else if (opCode == Instruction::FRem) {
    auto destTy = binaryOp.getType();
    if (destTy->getScalarType()->isHalfTy()) {
      // TODO: FREM for float16 type is not well handled by backend compiler. We lower it here:
      // frem(x, y) = x - y * trunc(x/y)

      auto builder = m_context->getBuilder();
      builder->SetInsertPoint(&binaryOp);

      auto one = ConstantFP::get(Type::getHalfTy(*m_context), 1.0);
      if (auto vecTy = dyn_cast<VectorType>(destTy))
        one = ConstantVector::getSplat(vecTy->getElementCount(), one);

      // -trunc(x * 1/y)
      Value *trunc = BinaryOperator::CreateFDiv(one, src2, "", &binaryOp);
      trunc = BinaryOperator::CreateFMul(trunc, src1, "", &binaryOp);
      trunc = builder->CreateIntrinsic(Intrinsic::trunc, destTy, trunc);
      trunc = UnaryOperator::CreateFNeg(trunc, "", &binaryOp);

      // -trunc(x/y) * y + x
      auto fRem = builder->CreateIntrinsic(Intrinsic::fmuladd, destTy, {trunc, src2, src1});

      binaryOp.replaceAllUsesWith(fRem);
      binaryOp.dropAllReferences();
      binaryOp.eraseFromParent();

      m_changed = true;
    }
  }

  // NOTE: We can't do constant folding for the following floating operations if we have floating-point controls that
  // will flush denormals or preserve NaN.
  if (!m_fp16DenormFlush && !m_fp32DenormFlush && !m_fp64DenormFlush) {
    switch (opCode) {
    case Instruction::FAdd:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src1IsConstZero)
          dest = src2;
        else if (src2IsConstZero)
          dest = src1;
      }
      break;
    case Instruction::FMul:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src1IsConstZero)
          dest = src1;
        else if (src2IsConstZero)
          dest = src2;
      }
      break;
    case Instruction::FDiv:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src1IsConstZero && !src2IsConstZero)
          dest = src1;
      }
      break;
    case Instruction::FSub:
      if (binaryOp.getFastMathFlags().noNaNs()) {
        if (src2IsConstZero)
          dest = src1;
      }
      break;
    default:
      break;
    }

    if (dest) {
      binaryOp.replaceAllUsesWith(dest);
      binaryOp.dropAllReferences();
      binaryOp.eraseFromParent();

      m_changed = true;
    }
  }

  // Replace FDIV x, y with FDIV 1.0, y; MUL x if it isn't optimized
  if (opCode == Instruction::FDiv && !dest && src1 && src2) {
    Constant *one = ConstantFP::get(binaryOp.getType(), 1.0);
    if (src1 != one) {
      IRBuilder<> builder(*m_context);
      builder.SetInsertPoint(&binaryOp);
      builder.setFastMathFlags(binaryOp.getFastMathFlags());
      Value *rcp = builder.CreateFDiv(ConstantFP::get(binaryOp.getType(), 1.0), src2);
      Value *fDiv = builder.CreateFMul(src1, rcp);

      binaryOp.replaceAllUsesWith(fDiv);
      binaryOp.dropAllReferences();
      binaryOp.eraseFromParent();

      m_changed = true;
    }
  }
}

// =====================================================================================================================
// Visits call instruction.
//
// @param callInst : Call instruction
void SpirvLowerAlgebraTransform::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();

  if (callee->isIntrinsic() && callee->getIntrinsicID() == Intrinsic::fabs) {
    // NOTE: FABS will be optimized by backend compiler with sign bit removed via AND.
    flushDenormIfNeeded(&callInst);
  } else {
    // Disable fast math for gl_Position.
    // TODO: Having this here is not good, as it requires us to know implementation details of Builder.
    // We need to find a neater way to do it.
    auto calleeName = callee->getName();
    unsigned builtIn = InvalidValue;
    Value *valueWritten = nullptr;
    if (calleeName.startswith("llpc.output.export.builtin.")) {
      builtIn = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
      valueWritten = callInst.getOperand(callInst.getNumArgOperands() - 1);
    } else if (calleeName.startswith("llpc.call.write.builtin")) {
      builtIn = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
      valueWritten = callInst.getOperand(0);
    }
    if (builtIn == lgc::BuiltInPosition)
      disableFastMath(valueWritten);
  }
}

// =====================================================================================================================
// Visits fptrunc instruction.
//
// @param fptruncInst : Fptrunc instruction
void SpirvLowerAlgebraTransform::visitFPTruncInst(FPTruncInst &fptruncInst) {
  if (m_fp16Rtz) {
    auto src = fptruncInst.getOperand(0);
    auto srcTy = src->getType();
    auto destTy = fptruncInst.getDestTy();

    if (srcTy->getScalarType()->isDoubleTy() && destTy->getScalarType()->isHalfTy()) {
      // NOTE: doubel -> float16 conversion is done in backend compiler with RTE rounding. Thus, we have to split
      // it with two phases to disable such lowering if we need RTZ rounding.
      auto floatTy = srcTy->isVectorTy() ? VectorType::get(Type::getFloatTy(*m_context), srcTy->getVectorNumElements())
                                         : Type::getFloatTy(*m_context);
      auto floatValue = new FPTruncInst(src, floatTy, "", &fptruncInst);
      auto dest = new FPTruncInst(floatValue, destTy, "", &fptruncInst);

      fptruncInst.replaceAllUsesWith(dest);
      fptruncInst.dropAllReferences();
      fptruncInst.eraseFromParent();

      m_changed = true;
    }
  }
}

// =====================================================================================================================
// Recursively finds backward if the FPMathOperator operand does not specifiy "contract" flag.
//
// @param operand : Operand to check
bool SpirvLowerAlgebraTransform::isOperandNoContract(Value *operand) {
  if (isa<BinaryOperator>(operand)) {
    auto inst = dyn_cast<BinaryOperator>(operand);

    if (isa<FPMathOperator>(operand)) {
      auto fastMathFlags = inst->getFastMathFlags();
      bool allowContract = fastMathFlags.allowContract();
      if (fastMathFlags.any() && !allowContract)
        return true;
    }

    for (auto opIt = inst->op_begin(), end = inst->op_end(); opIt != end; ++opIt)
      return isOperandNoContract(*opIt);
  }
  return false;
}

// =====================================================================================================================
// Disable fast math for all values related with the specified value
//
// @param value : Value to disable fast math
void SpirvLowerAlgebraTransform::disableFastMath(Value *value) {
  std::set<Instruction *> allValues;
  std::list<Instruction *> workSet;
  if (isa<Instruction>(value)) {
    allValues.insert(cast<Instruction>(value));
    workSet.push_back(cast<Instruction>(value));
  }

  auto it = workSet.begin();
  while (!workSet.empty()) {
    if (isa<FPMathOperator>(*it)) {
      // Reset fast math flags to default
      auto inst = cast<Instruction>(*it);
      llvm::FastMathFlags fastMathFlags;
      inst->copyFastMathFlags(fastMathFlags);
    }

    for (Value *operand : (*it)->operands()) {
      if (isa<Instruction>(operand)) {
        // Add new values
        auto inst = cast<Instruction>(operand);
        if (allValues.find(inst) == allValues.end()) {
          allValues.insert(inst);
          workSet.push_back(inst);
        }
      }
    }

    it = workSet.erase(it);
  }
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for algebraic transformation.
INITIALIZE_PASS(SpirvLowerAlgebraTransform, DEBUG_TYPE, "Lower SPIR-V algebraic transforms", false, false)
