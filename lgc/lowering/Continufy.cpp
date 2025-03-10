/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Continufy.cpp
 * @brief LLPC source file: contains implementation of class lgc::Continufy.
 * This pass translates indirect call into cps.await call, which will be lowered into continuation call.
 ***********************************************************************************************************************
 */

#include "lgc/lowering/Continufy.h"
#include "compilerutils/CompilerUtils.h"
#include "llpc/GpurtEnums.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/Builder.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PalMetadata.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-continufy"

using namespace llvm;
using namespace lgc;
using namespace lgc::cps;

namespace lgc {
using RtStage = rt::RayTracingShaderStage;

static Function *insertCpsArguments(Function &fn) {
  // Mutate function arguments, add ({} %state, %rcr, %shader-index).
  LLVMContext &context = fn.getContext();
  SmallVector<Type *> argTys = {StructType::get(context, {}), IntegerType::get(context, 32),
                                IntegerType::get(context, 32)};
  auto *fnTy = fn.getFunctionType();
  argTys.append(fnTy->params().begin(), fnTy->params().end());

  auto *newFn = compilerutils::mutateFunctionArguments(fn, Type::getVoidTy(context), argTys, fn.getAttributes());

  fn.replaceAllUsesWith(newFn);
  for (unsigned idx = 0; idx < fn.arg_size(); idx++) {
    Value *oldArg = fn.getArg(idx);
    Value *newArg = newFn->getArg(idx + 3);
    newArg->setName(oldArg->getName());
    oldArg->replaceAllUsesWith(newArg);
  }
  newFn->getArg(0)->setName("state");
  newFn->getArg(1)->setName("rcr");
  newFn->getArg(2)->setName("shader-index");
  return newFn;
}

/// Return the CPS levels mask of the ray-tracing stages that the input stage will return to.
/// NOTE: As Continufy pass will only be used to transform legacy indirect-call based ray-tracing shaders to lgccps
/// based continuation passing shader. The 'return stages' are just the possible callers of the input stage in typical
/// Vulkan ray-tracing pipeline.
static unsigned getReturnedLevels(int stage) {
  // Traversal will return to RGS or CHS/MISS.
  if (stage == -1)
    return 1u << (unsigned)CpsSchedulingLevel::RayGen | 1u << (unsigned)CpsSchedulingLevel::ClosestHit_Miss_Callable;

  RtStage rtStage = static_cast<RtStage>(stage);
  switch (rtStage) {
  case RtStage::RayGeneration:
    llvm_unreachable("Raygen shader should not arrive here.");
  case RtStage::ClosestHit:
  case RtStage::Miss:
    // Traversal
    return (1u << (unsigned)CpsSchedulingLevel::Traversal);
  case RtStage::Callable:
    // CHS/Miss/Callable | RGS
    return (1u << (unsigned)CpsSchedulingLevel::ClosestHit_Miss_Callable | 1u << (unsigned)CpsSchedulingLevel::RayGen);
  case RtStage::AnyHit:
    // IS | Traversal
    return (1u << (unsigned)CpsSchedulingLevel::Intersection | 1u << (unsigned)CpsSchedulingLevel::Traversal);
  case RtStage::Intersection:
    // Traversal
    return 1u << (unsigned)CpsSchedulingLevel::Traversal;
  default:
    llvm_unreachable("Unknown raytracing shader type.");
  }
}

/// Return CPS scheduling level of the ray-tracing stage.
static CpsSchedulingLevel getCpsLevelFromRtStage(int stage) {
  // Traversal
  if (stage == -1)
    return CpsSchedulingLevel::Traversal;

  RtStage rtStage = static_cast<RtStage>(stage);
  switch (rtStage) {
  case RtStage::RayGeneration:
    return CpsSchedulingLevel::RayGen;
  case RtStage::ClosestHit:
  case RtStage::Miss:
  case RtStage::Callable:
    return CpsSchedulingLevel::ClosestHit_Miss_Callable;
  case RtStage::AnyHit:
    return CpsSchedulingLevel::AnyHit_CombinedIntersection_AnyHit;
  case RtStage::Intersection:
    return CpsSchedulingLevel::Intersection;
  default:
    llvm_unreachable("Unknown raytracing shader type.");
  }
}

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses Continufy::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the Continufy pass \n");
  LLVMContext &context = module.getContext();
  ContHelper::setStackAddrspace(module, ContStackAddrspace::ScratchLLPC);

  llvm_dialects::Builder builder(context);
  SmallVector<Instruction *> tobeErased;

  for (auto &fn : make_early_inc_range(module.functions())) {
    MDNode *continufyStage = fn.getMetadata("continufy.stage");
    Function *fnPtr = &fn;
    std::optional<int32_t> currentRtStage;
    if (continufyStage) {
      fnPtr = insertCpsArguments(fn);
      currentRtStage = mdconst::extract<ConstantInt>(continufyStage->getOperand(0))->getSExtValue();
      CpsSchedulingLevel level = getCpsLevelFromRtStage(currentRtStage.value());
      setCpsFunctionLevel(*fnPtr, level);
    }

    // Translate call instruction with %continufy.stage into lgc.cps.await() with continuation reference.
    for (auto &block : *fnPtr) {
      for (auto &inst : block) {
        if (!isa<CallInst>(inst))
          continue;
        auto *calleeStage = inst.getMetadata("continufy.stage");
        if (!calleeStage)
          continue;

        auto &call = cast<CallInst>(inst);
        assert(call.getCallingConv() == CallingConv::SPIR_FUNC);
        auto *called = call.getCalledOperand();

        builder.SetInsertPoint(&call);
        auto *continuationRef = builder.CreatePtrToInt(called, IntegerType::get(context, 32));
        CpsSchedulingLevel calleeLevel =
            getCpsLevelFromRtStage(mdconst::extract<ConstantInt>(calleeStage->getOperand(0))->getSExtValue());
        continuationRef = builder.CreateOr(continuationRef, builder.getInt32((uint32_t)calleeLevel));

        // Always put a shader-index.
        SmallVector<Value *> tailArgs = {PoisonValue::get(builder.getInt32Ty())};
        tailArgs.append(call.arg_begin(), call.arg_end());
        auto *newCall = builder.create<AwaitOp>(call.getType(), continuationRef, 1u << (unsigned)calleeLevel, tailArgs);
        ContHelper::ReturnedRegisterCount::setValue(newCall, 0);
        call.replaceAllUsesWith(newCall);
        tobeErased.push_back(&call);
      }

      // Translate 'ret' into lgc.cps.jump for continufy stages.
      Instruction *term = block.getTerminator();
      if (auto *retInst = dyn_cast<ReturnInst>(term)) {
        builder.SetInsertPoint(term);

        if (!currentRtStage.has_value() || currentRtStage.value() == (int32_t)RtStage::RayGeneration) {
          builder.create<lgc::cps::CompleteOp>();
        } else {
          Value *poisonI32 = PoisonValue::get(builder.getInt32Ty());
          auto *retValue = retInst->getReturnValue();
          // %rcr, %shader-index
          SmallVector<Value *> tailArgs = {poisonI32};
          // return value
          if (retValue)
            tailArgs.push_back(retValue);

          builder.create<JumpOp>(fnPtr->getArg(1), getReturnedLevels(currentRtStage.value()), poisonI32 /* csp */,
                                 poisonI32 /* shaderRecIdx */, poisonI32 /* rcr */, tailArgs);
        }

        builder.CreateUnreachable();
        term->eraseFromParent();
      }
    }
  }
  for (auto *inst : tobeErased)
    inst->eraseFromParent();

  return PreservedAnalyses::allInSet<CFGAnalyses>();
}

} // namespace lgc
