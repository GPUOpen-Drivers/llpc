/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerMath.cpp
 * @brief LLPC source file: implementations of Llpc::LowerMathConstFolding and Llpc::LowerMathFloatOp.
 ***********************************************************************************************************************
 */
#include "LowerMath.h"
#include "Lowering.h"
#include "SPIRVInternal.h"
#include "hex_float.h"
#include "llpcContext.h"
#include "llpcGraphicsContext.h"
#include "lgc/Builder.h"
#include "lgc/Pipeline.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Local.h"

#define DEBUG_TYPE_CONST_FOLDING "lower-math-const-folding"
#define DEBUG_TYPE_PRECISION "lower-math-precision"
#define DEBUG_TYPE_FLOAT_OP "lower-math-float-op"

using namespace lgc;
using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

static cl::opt<bool>
    ForwardPropagateNoContract("forward-propagate-no-contract",
                               cl::desc("Forward propagate NoContraction decorations to dependent FAdd operations"),
                               cl::init(false));
static cl::opt<bool>
    BackwardPropagateNoContract("backward-propagate-no-contract",
                                cl::desc("Backward propagate NoContraction decorations to input operations"),
                                cl::init(false));
static cl::opt<bool> DisableGlPositionOpt("disable-gl-position-opt",
                                          cl::desc("Disable all use of fast math flags on gl_Position"),
                                          cl::init(false));

// =====================================================================================================================
LowerMath::LowerMath()
    : m_changed(false), m_fp16DenormFlush(false), m_fp32DenormFlush(false), m_fp64DenormFlush(false),
      m_fp16RoundToZero(false) {
}

// =====================================================================================================================
// Set denormal-fp-math attribute to the specified function according to provided FP denormal mode.
//
// @param func : Function to set the attribute
// @param fp32 : Whether the attribute is for FP32
// @param denormMode : FP denormal mode
static void setFpMathAttribute(Function &func, bool fp32, FpDenormMode denormMode) {
  const char *attrName = fp32 ? "denormal-fp-math-f32" : "denormal-fp-math";
  if (denormMode == FpDenormMode::FlushNone || denormMode == FpDenormMode::FlushIn)
    func.addFnAttr(attrName, "ieee");
  else if (fp32 || denormMode == FpDenormMode::FlushOut || denormMode == FpDenormMode::FlushInOut)
    func.addFnAttr(attrName, "preserve-sign");
}

// =====================================================================================================================
// Initialise transform class.
//
// @param [in/out] module : LLVM module to be run on
void LowerMath::init(Module &module) {
  Lowering::init(&module);
  m_changed = false;

  if (m_shaderStage == ShaderStageInvalid)
    return;

  // NOTE: We try to set denormal-fp-math here because later optimization passes will detect the attributes and decide
  // what to do. Such attributes will be set once again in LGC.
  auto shaderMode = Pipeline::getCommonShaderMode(module, getLgcShaderStage(m_shaderStage));
  setFpMathAttribute(*m_entryPoint, false, shaderMode.fp16DenormMode);
  setFpMathAttribute(*m_entryPoint, true, shaderMode.fp32DenormMode);
  setFpMathAttribute(*m_entryPoint, false, shaderMode.fp64DenormMode);

  m_fp16DenormFlush =
      shaderMode.fp16DenormMode == FpDenormMode::FlushOut || shaderMode.fp16DenormMode == FpDenormMode::FlushInOut;
  m_fp32DenormFlush =
      shaderMode.fp32DenormMode == FpDenormMode::FlushOut || shaderMode.fp32DenormMode == FpDenormMode::FlushInOut;
  m_fp64DenormFlush =
      shaderMode.fp64DenormMode == FpDenormMode::FlushOut || shaderMode.fp64DenormMode == FpDenormMode::FlushInOut;
  m_fp16RoundToZero = shaderMode.fp16RoundMode == FpRoundMode::Zero;
}

// =====================================================================================================================
// Checks desired denormal flush behavior and inserts llvm.canonicalize.
//
// @param inst : Instruction to flush denormals if needed
void LowerMath::flushDenormIfNeeded(Instruction *inst) {
  auto destTy = inst->getType();
  if ((destTy->getScalarType()->isHalfTy() && m_fp16DenormFlush) ||
      (destTy->getScalarType()->isFloatTy() && m_fp32DenormFlush) ||
      (destTy->getScalarType()->isDoubleTy() && m_fp64DenormFlush)) {
    // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
    auto builder = m_context->getBuilder();
    builder->SetInsertPoint(inst->getNextNode());
    auto canonical = builder->CreateIntrinsic(Intrinsic::canonicalize, destTy, PoisonValue::get(destTy));

    inst->replaceAllUsesWith(canonical);
    canonical->setArgOperand(0, inst);
    m_changed = true;
  }
}

// =====================================================================================================================
// Identify if a value does not specify "contract" flag.
//
// Note: FPMathOperators without any fast math flags are ignored.
//
// @param value : Value to check
static bool isNoContract(Value *value) {
  if (!isa<FPMathOperator>(value))
    return false;
  auto inst = cast<FPMathOperator>(value);
  FastMathFlags fastMathFlags = inst->getFastMathFlags();
  return (fastMathFlags.any() && !fastMathFlags.allowContract());
}

// =====================================================================================================================
// Disable fast math for all values related with the specified value
//
// @param value : Value to disable fast math for
// @param clearAll : Whether to clear all flags, including nnan and nsz
static void disableFastMath(Value *value, bool clearAll) {
  std::set<Instruction *> allValues;
  std::list<Instruction *> workSet;
  if (isa<Instruction>(value)) {
    allValues.insert(cast<Instruction>(value));
    workSet.push_back(cast<Instruction>(value));
  }

  auto it = workSet.begin();
  while (!workSet.empty()) {
    if (isa<FPMathOperator>(*it)) {
      // Reset fast math flags to default, but maintain nsz, nnan and afn as required.
      auto inst = cast<Instruction>(*it);
      FastMathFlags newFmf;
      if (!clearAll) {
        FastMathFlags instFmf = inst->getFastMathFlags();
        newFmf.setNoSignedZeros(instFmf.noSignedZeros());
        newFmf.setNoNaNs(instFmf.noNaNs());
        newFmf.setApproxFunc(instFmf.approxFunc());
      }
      inst->copyFastMathFlags(newFmf);
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

#define DEBUG_TYPE DEBUG_TYPE_CONST_FOLDING

// =====================================================================================================================
// Executes constant folding FElowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerMathConstFolding::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Math-Const-Folding\n");

  LowerMath::init(module);

  if (m_shaderStage == ShaderStageInvalid)
    return PreservedAnalyses::all();

  if (m_fp16DenormFlush || m_fp32DenormFlush || m_fp64DenormFlush) {
    // Do constant folding if we need flush denorm to zero.
    FunctionAnalysisManager &functionAnalysisManager =
        analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
    auto &targetLibInfo = functionAnalysisManager.getResult<TargetLibraryAnalysis>(*m_entryPoint);
    auto &dataLayout = m_module->getDataLayout();

    for (auto &block : *m_entryPoint) {
      for (auto instIter = block.begin(), instEnd = block.end(); instIter != instEnd;) {
        Instruction *inst = &(*instIter++);

        // DCE instruction if trivially dead.
        if (isInstructionTriviallyDead(inst, &targetLibInfo)) {
          LLVM_DEBUG(dbgs() << "Algebraic transform: DCE: " << *inst << '\n');
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
          LLVM_DEBUG(dbgs() << "Algebraic transform: constant folding: " << *constVal << " from: " << *inst << '\n');
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

  return m_changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Return the module entry point function.
Function *LowerMathConstFolding::getEntryPoint() {
  return m_entryPoint;
}

#undef DEBUG_TYPE // DEBUG_TYPE_CONST_FOLDING
#define DEBUG_TYPE DEBUG_TYPE_PRECISION

bool LowerMathPrecision::adjustExports(Module &module, bool disablePositionOpt) {
  bool changed = false;
  for (auto &func : module.functions()) {
    // Disable fast math for gl_Position and gl_FragDepth.
    // TODO: This requires knowledge of the Builder implementation, which is not ideal.
    // We need to find a neater way to do it.
    auto funcName = func.getName();
    bool isExport;
    if (funcName.starts_with("lgc.output.export.builtin."))
      isExport = true;
    else if (funcName.starts_with("lgc.create.write.builtin"))
      isExport = false;
    else
      continue;

    for (User *user : func.users()) {
      CallInst *callInst = cast<CallInst>(user);
      unsigned builtIn;
      Value *valueWritten;
      if (isExport) {
        builtIn = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
        valueWritten = callInst->getOperand(callInst->arg_size() - 1);
      } else {
        builtIn = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue();
        valueWritten = callInst->getOperand(0);
      }

      if (valueWritten && (builtIn == lgc::BuiltInPosition || builtIn == lgc::BuiltInFragDepth)) {
        disableFastMath(valueWritten, disablePositionOpt);
        changed = true;
      }
    }
  }
  return changed;
}

static bool clearContractFlag(Instruction *inst) {
  if (!isa<FPMathOperator>(inst))
    return false;
  LLVM_DEBUG(dbgs() << "clearing contract flags: " << *inst << "\n");
  FastMathFlags fastMathFlags = inst->getFastMathFlags();
  fastMathFlags.setAllowReassoc(false);
  fastMathFlags.setAllowContract(false);
  inst->copyFastMathFlags(fastMathFlags);
  return true;
}

bool LowerMathPrecision::propagateNoContract(Module &module, bool forward, bool backward) {
  bool changed = false;

  SmallVector<Instruction *> roots;
  DenseSet<Instruction *> visited;

  // Find all NoContract instructions to build root set
  LLVM_DEBUG(dbgs() << "locate no contract roots\n");
  for (auto &func : module) {
    for (auto &block : func) {
      for (auto &inst : block) {
        if (isNoContract(&inst)) {
          LLVM_DEBUG(dbgs() << "root: " << inst << "\n");
          roots.push_back(&inst);
          visited.insert(&inst);
        }
      }
    }
  }

  SmallVector<Instruction *> worklist;

  // Backward propagate via operands
  if (backward) {
    LLVM_DEBUG(dbgs() << "backward propagate no contract\n");
    worklist = roots;
    while (!worklist.empty()) {
      auto inst = worklist.pop_back_val();
      LLVM_DEBUG(dbgs() << "visit: " << *inst << "\n");
      for (Value *operand : inst->operands()) {
        if (auto opInst = dyn_cast<Instruction>(operand)) {
          if (!visited.insert(opInst).second)
            continue;
          if (clearContractFlag(opInst))
            changed = true;
          worklist.push_back(opInst);
        }
      }
    }
  }

  // Forward propagate via users
  if (forward) {
    LLVM_DEBUG(dbgs() << "forward propagate no contract\n");
    worklist = roots;
    while (!worklist.empty()) {
      auto inst = worklist.pop_back_val();
      LLVM_DEBUG(dbgs() << "visit: " << *inst << "\n");
      for (User *user : inst->users()) {
        // Only propagate through instructions
        if (auto userInst = dyn_cast<Instruction>(user)) {
          if (!visited.insert(userInst).second)
            continue;
          // Only update FAdd instructions
          if (userInst->getOpcode() == Instruction::FAdd) {
            if (clearContractFlag(userInst))
              changed = true;
          }
          worklist.push_back(userInst);
        }
      }
    }
  }

  return changed;
}

// =====================================================================================================================
// Run precision (fast math flag) adjustment FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerMathPrecision::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Math-Precision\n");

  Lowering::init(&module);
  if (m_shaderStage == ShaderStageInvalid)
    return PreservedAnalyses::all();

  bool forwardPropagate = false;
  bool backwardPropagate = false;
  bool disableGlPositionOpt = false;
  auto pipelineContext = m_context->getPipelineContext();
  switch (pipelineContext->getPipelineType()) {
  case PipelineType::Graphics: {
    auto shaderInfo = (static_cast<const GraphicsContext *>(pipelineContext))->getPipelineShaderInfo(m_shaderStage);
    forwardPropagate = forwardPropagate || shaderInfo->options.forwardPropagateNoContract;
    backwardPropagate = backwardPropagate || shaderInfo->options.backwardPropagateNoContract;
    disableGlPositionOpt = shaderInfo->options.disableGlPositionOpt;
    break;
  }
  case PipelineType::Compute: {
    auto shaderInfo = &(static_cast<const ComputePipelineBuildInfo *>(pipelineContext->getPipelineBuildInfo()))->cs;
    forwardPropagate = forwardPropagate || shaderInfo->options.forwardPropagateNoContract;
    backwardPropagate = backwardPropagate || shaderInfo->options.backwardPropagateNoContract;
    disableGlPositionOpt = shaderInfo->options.disableGlPositionOpt;
    break;
  }
  case PipelineType::RayTracing: {
    auto pipelineInfo = static_cast<const RayTracingPipelineBuildInfo *>(pipelineContext->getPipelineBuildInfo());
    // Note: turn on options if any of the shaders from this stage specify them, because we do not know exactly
    // shader this module is.
    for (unsigned i = 0; i < pipelineInfo->shaderCount; ++i) {
      if (pipelineInfo->pShaders[i].entryStage != m_shaderStage)
        continue;
      forwardPropagate = forwardPropagate || pipelineInfo->pShaders[i].options.forwardPropagateNoContract;
      backwardPropagate = backwardPropagate || pipelineInfo->pShaders[i].options.backwardPropagateNoContract;
      disableGlPositionOpt = pipelineInfo->pShaders[i].options.disableGlPositionOpt;
    }
    break;
  }
  default:
    break;
  }

  if (ForwardPropagateNoContract.getNumOccurrences())
    forwardPropagate = ForwardPropagateNoContract;
  if (BackwardPropagateNoContract.getNumOccurrences())
    backwardPropagate = BackwardPropagateNoContract;
  if (DisableGlPositionOpt.getNumOccurrences())
    disableGlPositionOpt = DisableGlPositionOpt;

  bool adjustedExports = false;
  if (pipelineContext->getPipelineOptions()->enableImplicitInvariantExports)
    adjustedExports = adjustExports(module, disableGlPositionOpt);

  bool propagatedNoContract = false;
  if (forwardPropagate || backwardPropagate)
    propagatedNoContract = propagateNoContract(module, forwardPropagate, backwardPropagate);

  return (adjustedExports || propagatedNoContract) ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

#undef DEBUG_TYPE // DEBUG_TYPE_PRECISION
#define DEBUG_TYPE DEBUG_TYPE_FLOAT_OP

// =====================================================================================================================
// Executes floating point optimisation FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerMathFloatOp::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Math-Float-Op\n");

  LowerMath::init(module);
  visit(m_module);

  return m_changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Visits binary operator instruction.
//
// @param binaryOp : Binary operator instruction
void LowerMathFloatOp::visitBinaryOperator(BinaryOperator &binaryOp) {
  Instruction::BinaryOps opCode = binaryOp.getOpcode();

  auto src1 = binaryOp.getOperand(0);
  auto src2 = binaryOp.getOperand(1);
  bool src1IsConstZero =
      isa<ConstantAggregateZero>(src1) || (isa<ConstantFP>(src1) && cast<ConstantFP>(src1)->isZero());
  bool src2IsConstZero =
      isa<ConstantAggregateZero>(src2) || (isa<ConstantFP>(src2) && cast<ConstantFP>(src2)->isZero());

  if (opCode == Instruction::FSub && src1IsConstZero) {
    // NOTE: Source1 is constant zero, we might be performing FNEG operation. This will be optimized
    // by backend compiler with sign bit reversed via XOR. Check floating-point controls.
    flushDenormIfNeeded(&binaryOp);
  }

  // NOTE: We can't do constant folding for the following floating operations if we have floating-point controls that
  // will flush denormals or preserve NaN.
  if (!m_fp16DenormFlush && !m_fp32DenormFlush && !m_fp64DenormFlush) {
    Value *dest = nullptr;
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
      return;
    }
  }

  // Replace FDIV x, y with FDIV 1.0, y; MUL x if it isn't optimized
  if (opCode == Instruction::FDiv) {
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
      return;
    }
  }
}

// =====================================================================================================================
// Visits call instruction.
//
// @param callInst : Call instruction
void LowerMathFloatOp::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  if (callee->isIntrinsic() && callee->getIntrinsicID() == Intrinsic::fabs) {
    // NOTE: FABS will be optimized by backend compiler with sign bit removed via AND.
    flushDenormIfNeeded(&callInst);
  }
}

// =====================================================================================================================
// Visits fptrunc instruction.
//
// @param fptruncInst : Fptrunc instruction
void LowerMathFloatOp::visitFPTruncInst(FPTruncInst &fptruncInst) {
  if (m_fp16RoundToZero) {
    auto src = fptruncInst.getOperand(0);
    auto srcTy = src->getType();
    auto destTy = fptruncInst.getDestTy();

    if (srcTy->getScalarType()->isDoubleTy() && destTy->getScalarType()->isHalfTy()) {
      // NOTE: double -> float16 conversion is done in backend compiler with RTE rounding. Thus, we have to split
      // it with two phases to disable such lowering if we need RTZ rounding.
      IRBuilder<> builder(*m_context);
      builder.SetInsertPoint(&fptruncInst);
      auto floatTy = srcTy->isVectorTy() ? FixedVectorType::get(Type::getFloatTy(*m_context),
                                                                cast<FixedVectorType>(srcTy)->getNumElements())
                                         : Type::getFloatTy(*m_context);
      auto floatValue = builder.CreateFPTrunc(src, floatTy);
      auto dest = builder.CreateFPTrunc(floatValue, destTy);

      fptruncInst.replaceAllUsesWith(dest);
      fptruncInst.eraseFromParent();

      m_changed = true;
    }
  }
}

#undef DEBUG_TYPE // DEBUG_TYPE_FLOAT_OP
