/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLower.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLower.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLower.h"
#include "LowerGLCompatibility.h"
#include "LowerPostInline.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerAccessChain.h"
#include "llpcSpirvLowerCfgMerges.h"
#include "llpcSpirvLowerConstImmediateStore.h"
#include "llpcSpirvLowerCooperativeMatrix.h"
#include "llpcSpirvLowerGlobal.h"
#include "llpcSpirvLowerInstMetaRemove.h"
#include "llpcSpirvLowerMath.h"
#include "llpcSpirvLowerMemoryOp.h"
#include "llpcSpirvLowerRayTracing.h"
#include "llpcSpirvLowerTerminator.h"
#include "llpcSpirvLowerTranslator.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcSpirvProcessGpuRtLibrary.h"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/IR/Verifier.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
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

#define DEBUG_TYPE "llpc-spirv-lower"

using namespace lgc;
using namespace llvm;

namespace Llpc {

// =====================================================================================================================
// Add per-shader lowering passes to pass manager
//
// @param context : LLPC context
// @param stage : Shader stage
// @param [in/out] passMgr : Pass manager to add passes to
// @param lowerTimer : Timer to time lower passes with, nullptr if not timing
// @param lowerFlag : Add the required pass based on the flag
void SpirvLower::addPasses(Context *context, ShaderStage stage, lgc::PassManager &passMgr, Timer *lowerTimer,
                           LowerFlag lowerFlag) {
  // Start timer for lowering passes.
  if (lowerTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, lowerTimer, true);

  if (lowerFlag.isInternalRtShader)
    passMgr.addPass(SpirvProcessGpuRtLibrary());

  // Lower SPIR-V CFG merges before inlining
  passMgr.addPass(SpirvLowerCfgMerges());

  // Function inlining. Use the "always inline" pass, since we want to inline all functions, and
  // we marked (non-entrypoint) functions as "always inline" just after SPIR-V reading.
  passMgr.addPass(AlwaysInlinerPass());
  passMgr.addPass(GlobalDCEPass());

  // Lower SPIR-V access chain
  passMgr.addPass(SpirvLowerAccessChain());

  if (lowerFlag.isRayQuery || lowerFlag.usesAdvancedBlend)
    passMgr.addPass(LowerPostInline());

  // Lower SPIR-V terminators
  passMgr.addPass(SpirvLowerTerminator());

  // Lower spirv.cooperative.matrix.proxy to LGC operations. Should run before SROA.
  passMgr.addPass(SpirvLowerCooperativeMatrix());

  // Lower Glsl compatibility variables and operations
  passMgr.addPass(LowerGLCompatibility());

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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 444780
  // Old version of the code
  passMgr.addPass(createModuleToFunctionPassAdaptor(SROAPass()));
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  passMgr.addPass(createModuleToFunctionPassAdaptor(SROAPass(SROAOptions::ModifyCFG)));
#endif

  // Lower SPIR-V precision / adjust fast math flags.
  // Must be done before instruction combining pass to prevent incorrect contractions.
  // Should be after SROA to avoid having to track values through memory load/store.
  passMgr.addPass(SpirvLowerMathPrecision());

  passMgr.addPass(GlobalOptPass());
  passMgr.addPass(createModuleToFunctionPassAdaptor(ADCEPass()));

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 452298
  // Old version of the code
  unsigned instCombineOpt = 2;
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  auto instCombineOpt = InstCombineOptions().setMaxIterations(2);
#endif
  passMgr.addPass(createModuleToFunctionPassAdaptor(InstCombinePass(instCombineOpt)));
  passMgr.addPass(createModuleToFunctionPassAdaptor(SimplifyCFGPass()));
  passMgr.addPass(createModuleToFunctionPassAdaptor(EarlyCSEPass()));

  // Lower SPIR-V floating point optimisation
  passMgr.addPass(SpirvLowerMathFloatOp());

  // Lower SPIR-V instruction metadata remove
  passMgr.addPass(SpirvLowerInstMetaRemove());

  // Lower SPIR-V ray tracing related stuff, including entry point generation, lgc.rt dialect handling, some of
  // lgc.gpurt dialect handling.
  // And do inlining after SpirvLowerRayTracing as it will produce some extra functions.
  if (lowerFlag.isRayTracing) {
    assert(context->getPipelineType() == PipelineType::RayTracing);
    if (!static_cast<RayTracingContext *>(context->getPipelineContext())->isContinuationsMode()) {
      passMgr.addPass(SpirvLowerRayTracing());
      passMgr.addPass(AlwaysInlinerPass());
    }
  }

  if (lowerFlag.isRayTracing || lowerFlag.isRayQuery || lowerFlag.isInternalRtShader) {
    FunctionPassManager fpm;
    fpm.addPass(SROAPass(SROAOptions::PreserveCFG));
    fpm.addPass(InstCombinePass(instCombineOpt));
    passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  }

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
// Register all the translation passes into the given pass manager
//
// @param [in/out] passMgr : Pass manager
void SpirvLower::registerTranslationPasses(lgc::PassManager &passMgr) {
  passMgr.registerPass("llpc-spirv-lower-translator", SpirvLowerTranslator::name());
  passMgr.registerPass("llpc-spirv-lower-gpurt-library", SpirvProcessGpuRtLibrary::name());
}

// =====================================================================================================================
// Register all the lowering passes into the given pass manager
//
// @param [in/out] passMgr : Pass manager
void SpirvLower::registerLoweringPasses(lgc::PassManager &passMgr) {
#define LLPC_PASS(NAME, CLASS) passMgr.registerPass(NAME, CLASS::name());
#include "PassRegistry.inc"
}

// =====================================================================================================================
// Replace global variable with another global variable
//
// @param context : The context
// @param original : Replaced global variable
// @param replacement : Replacing global variable
void SpirvLower::replaceGlobal(Context *context, GlobalVariable *original, GlobalVariable *replacement) {
  convertUsersOfConstantsToInstructions(original);
  original->replaceAllUsesWith(replacement);
  original->eraseFromParent();
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
  SmallVector<Function *> entries;
  getEntryPoints(module, entries);
  if (entries.size() != 1) {
    m_shaderStage = ShaderStageInvalid;
    m_entryPoint = nullptr;
  } else {
    m_entryPoint = entries[0];
    m_shaderStage = getShaderStageFromFunction(m_entryPoint);
    if (m_shaderStage == ShaderStageInvalid) {
      // There might be cases we fail to get shader stage from a module that is not directly converted from SPIR-V, for
      // example, unified ray tracing pipeline shader, or entry for indirect ray tracing pipeline. In such case, clamp
      // the shader stage to compute.
      assert(m_entryPoint);
      m_shaderStage = ShaderStageCompute;
    }
  }
  m_builder = m_context->getBuilder();
}

} // namespace Llpc
