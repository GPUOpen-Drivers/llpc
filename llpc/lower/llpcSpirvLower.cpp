/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLower.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLower.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLower.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcSpirvLowerAccessChain.h"
#include "llpcSpirvLowerConstImmediateStore.h"
#include "llpcSpirvLowerGlobal.h"
#include "llpcSpirvLowerInstMetaRemove.h"
#include "llpcSpirvLowerMath.h"
#include "llpcSpirvLowerMemoryOp.h"
#include "llpcSpirvLowerTerminator.h"
#include "llpcSpirvLowerTranslator.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/FunctionAttrs.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/GlobalOpt.h"
#include "llvm/Transforms/IPO/InferFunctionAttrs.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Vectorize.h"

#define DEBUG_TYPE "llpc-spirv-lower"

using namespace lgc;
using namespace llvm;

namespace Llpc {
// =====================================================================================================================
// Replace a constant with instructions using a builder.
//
// @param context : The context
// @param [in/out] constVal : The constant to replace with instructions.
void SpirvLower::replaceConstWithInsts(Context *context, Constant *const constVal)

{
  SmallSet<Constant *, 8> otherConsts;
  Builder *builder = context->getBuilder();
  for (User *const user : constVal->users()) {
    if (Constant *const otherConst = dyn_cast<Constant>(user))
      otherConsts.insert(otherConst);
  }

  for (Constant *const otherConst : otherConsts)
    replaceConstWithInsts(context, otherConst);

  otherConsts.clear();

  SmallVector<Value *, 8> users;

  for (User *const user : constVal->users())
    users.push_back(user);

  for (Value *const user : users) {
    Instruction *const inst = dyn_cast<Instruction>(user);
    assert(inst);

    // If the instruction is a phi node, we have to insert the new instructions in the correct predecessor.
    if (PHINode *const phiNode = dyn_cast<PHINode>(inst)) {
      const unsigned incomingValueCount = phiNode->getNumIncomingValues();
      for (unsigned i = 0; i < incomingValueCount; i++) {
        if (phiNode->getIncomingValue(i) == constVal) {
          builder->SetInsertPoint(phiNode->getIncomingBlock(i)->getTerminator());
          break;
        }
      }
    } else
      builder->SetInsertPoint(inst);

    if (ConstantExpr *const constExpr = dyn_cast<ConstantExpr>(constVal)) {
      Instruction *const insertPos = builder->Insert(constExpr->getAsInstruction());
      inst->replaceUsesOfWith(constExpr, insertPos);
    } else if (ConstantVector *const constVector = dyn_cast<ConstantVector>(constVal)) {
      Value *resultValue = UndefValue::get(constVector->getType());
      for (unsigned i = 0; i < constVector->getNumOperands(); i++) {
        // Have to not use the builder here because it will constant fold and we are trying to undo that now!
        Instruction *const insertPos =
            InsertElementInst::Create(resultValue, constVector->getOperand(i), builder->getInt32(i));
        resultValue = builder->Insert(insertPos);
      }
      inst->replaceUsesOfWith(constVector, resultValue);
    } else
      llvm_unreachable("Should never be called!");
  }

  constVal->removeDeadConstantUsers();
  constVal->destroyConstant();
}

// =====================================================================================================================
// Removes those constant expressions that reference global variables.
//
// @param context : The context
// @param global : The global variable
void SpirvLower::removeConstantExpr(Context *context, GlobalVariable *global) {
  SmallVector<Constant *, 8> constantUsers;

  for (User *const user : global->users()) {
    if (Constant *const pConst = dyn_cast<Constant>(user))
      constantUsers.push_back(pConst);
  }

  for (Constant *const constVal : constantUsers)
    replaceConstWithInsts(context, constVal);
}

// =====================================================================================================================
// Add per-shader lowering passes to pass manager
//
// @param context : LLPC context
// @param stage : Shader stage
// @param [in/out] passMgr : Pass manager to add passes to
// @param lowerTimer : Timer to time lower passes with, nullptr if not timing
void SpirvLower::addPasses(Context *context, ShaderStage stage, lgc::PassManager &passMgr, Timer *lowerTimer
) {
  // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
  context->getLgcContext()->preparePassManager(passMgr);

  // Start timer for lowering passes.
  if (lowerTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, lowerTimer, true);

  // Function inlining. Use the "always inline" pass, since we want to inline all functions, and
  // we marked (non-entrypoint) functions as "always inline" just after SPIR-V reading.
  passMgr.addPass(AlwaysInlinerPass());
  passMgr.addPass(GlobalDCEPass());

  // Lower SPIR-V access chain
  passMgr.addPass(SpirvLowerAccessChain());

  // Lower SPIR-V terminators
  passMgr.addPass(SpirvLowerTerminator());

  // Lower SPIR-V global variables, inputs, and outputs
  passMgr.addPass(SpirvLowerGlobal());

  // Lower SPIR-V constant immediate store.
  passMgr.addPass(SpirvLowerConstImmediateStore());

  // Lower SPIR-V constant folding - must be done before instruction combining pass.
  passMgr.addPass(SpirvLowerMathConstFolding());

  // Lower SPIR-V memory operations
  passMgr.addPass(SpirvLowerMemoryOp());

  // Remove redundant load/store operations and do minimal optimization
  // It is required by SpirvLowerImageOp.
  passMgr.addPass(createModuleToFunctionPassAdaptor(SROA()));
  passMgr.addPass(GlobalOptPass());
  passMgr.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  passMgr.addPass(createModuleToFunctionPassAdaptor(ADCEPass()));
  passMgr.addPass(createModuleToFunctionPassAdaptor(InstCombinePass(2)));
  passMgr.addPass(createModuleToFunctionPassAdaptor(SimplifyCFGPass()));
  passMgr.addPass(createModuleToFunctionPassAdaptor(EarlyCSEPass()));
  passMgr.addPass(IPSCCPPass());

  // Lower SPIR-V floating point optimisation
  passMgr.addPass(SpirvLowerMathFloatOp());

  // Lower SPIR-V instruction metadata remove
  passMgr.addPass(SpirvLowerInstMetaRemove());

  // Stop timer for lowering passes.
  if (lowerTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, lowerTimer, false);

  // Dump the result
  if (EnableOuts()) {
    passMgr.addPass(PrintModulePass(outs(),
                                    "\n"
                                    "===============================================================================\n"
                                    "// LLPC SPIR-V lowering results\n"));
  }
}

// =====================================================================================================================
// Register a pass to identify it with a short name in the pass manager
//
// @param [in/out] passMgr : Pass manager
void SpirvLower::registerPasses(lgc::PassManager &passMgr) {
#define LLPC_PASS(NAME, CLASS) passMgr.registerPass(NAME, decltype(CLASS)::name());
#include "PassRegistry.def"
}

// =====================================================================================================================
// Add per-shader lowering passes to pass manager
//
// @param context : LLPC context
// @param stage : Shader stage
// @param [in/out] passMgr : Pass manager to add passes to
// @param lowerTimer : Timer to time lower passes with, nullptr if not timing
void LegacySpirvLower::addPasses(Context *context, ShaderStage stage, legacy::PassManager &passMgr, Timer *lowerTimer
) {
  // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
  context->getLgcContext()->preparePassManager(&passMgr);

  // Start timer for lowering passes.
  if (lowerTimer)
    passMgr.add(LgcContext::createStartStopTimer(lowerTimer, true));

  // Function inlining. Use the "always inline" pass, since we want to inline all functions, and
  // we marked (non-entrypoint) functions as "always inline" just after SPIR-V reading.
  passMgr.add(createAlwaysInlinerLegacyPass());
  passMgr.add(createGlobalDCEPass());

  // Lower SPIR-V access chain
  passMgr.add(createLegacySpirvLowerAccessChain());

  // Lower SPIR-V terminators
  passMgr.add(createLegacySpirvLowerTerminator());

  // Lower SPIR-V global variables, inputs, and outputs
  passMgr.add(createLegacySpirvLowerGlobal());

  // Lower SPIR-V constant immediate store.
  passMgr.add(createLegacySpirvLowerConstImmediateStore());

  // Lower SPIR-V constant folding - must be done before instruction combining pass.
  passMgr.add(createLegacySpirvLowerMathConstFolding());

  // Lower SPIR-V memory operations
  passMgr.add(createLegacySpirvLowerMemoryOp());

  // Remove redundant load/store operations and do minimal optimization
  // It is required by SpirvLowerImageOp.
  passMgr.add(createSROAPass());
  passMgr.add(createGlobalOptimizerPass());
  passMgr.add(createPromoteMemoryToRegisterPass());
  passMgr.add(createAggressiveDCEPass());
  passMgr.add(createInstructionCombiningPass(2));
  passMgr.add(createCFGSimplificationPass());
  passMgr.add(createEarlyCSEPass());
  passMgr.add(createIPSCCPPass());

  // Lower SPIR-V floating point optimisation
  passMgr.add(createLegacySpirvLowerMathFloatOp());

  // Lower SPIR-V instruction metadata remove
  passMgr.add(createLegacySpirvLowerInstMetaRemove());

  // Stop timer for lowering passes.
  if (lowerTimer)
    passMgr.add(LgcContext::createStartStopTimer(lowerTimer, false));

  // Dump the result
  if (EnableOuts()) {
    passMgr.add(createPrintModulePass(
        outs(), "\n"
                "===============================================================================\n"
                "// LLPC SPIR-V lowering results\n"));
  }
}

// =====================================================================================================================
// Initializes the pass according to the specified module.
//
// NOTE: This function should be called at the beginning of "runOnModule()".
//
// @param module : LLVM module
void SpirvLower::init(Module *module) {
  m_module = module;
  m_context = static_cast<Context *>(&m_module->getContext());
  if (m_module->empty()) {
    m_shaderStage = ShaderStageInvalid;
    m_entryPoint = nullptr;
  } else {
    m_shaderStage = getShaderStageFromModule(m_module);
    m_entryPoint = getEntryPoint(m_module);
  }
  m_builder = m_context->getBuilder();
}

} // namespace Llpc
