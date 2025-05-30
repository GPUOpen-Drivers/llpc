/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  SetupTargetFeatures.cpp
* @brief LLPC source file: contains declaration and implementation of class lgc::SetUpTargetFeatures.
***********************************************************************************************************************
*/
#include "lgc/lowering/SetupTargetFeatures.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-set-up-target-features"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses SetUpTargetFeatures::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();

  LLVM_DEBUG(dbgs() << "Run the pass Set-up-Target-Features\n");

  LgcLowering::init(&module);

  m_pipelineState = pipelineState;
  setupTargetFeatures(&module);

#ifndef NDEBUG
  // On a debug build, check there are no leftover lgc*.* dialect ops.
  bool err = false;
  for (Function &decl : module) {
    if (!decl.isDeclaration() || decl.getIntrinsicID() != Intrinsic::not_intrinsic || decl.use_empty())
      continue;
    if (decl.getName().starts_with("lgc") && decl.getName().find('.') != StringRef::npos) {
      errs() << "Leftover dialect op " << decl.getName() << "\n";
      err = true;
    }
  }
  if (err)
    report_fatal_error("Leftover dialect ops");
#endif

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Setup LLVM target features, target features are set per entry point function.
//
// @param [in/out] module : LLVM module
void SetUpTargetFeatures::setupTargetFeatures(Module *module) {
  std::string globalFeatures = "";

  if (m_pipelineState->getOptions().includeDisassembly)
    globalFeatures += ",+DumpCode";

  for (auto func = module->begin(), end = module->end(); func != end; ++func) {
    if (func->isDeclaration())
      continue;

    std::string targetFeatures(globalFeatures);
    AttrBuilder builder(module->getContext());

    auto shaderStage = lgc::getShaderStage(&*func);

    // NOTE: AMDGPU_CS_ChainPreserve is expected to not have shader stage set.
    if (func->getCallingConv() != CallingConv::AMDGPU_CS_ChainPreserve) {
      if (!shaderStage.has_value()) {
        errs() << "Invalid shader stage for function " << func->getName() << "\n";
        report_fatal_error("Got invalid shader stage when setting up features for function");
      }
    }

    if (isShaderEntryPoint(&*func)) {
      const ShaderOptions &options = m_pipelineState->getShaderOptions(shaderStage.value());
      if (options.useSiScheduler) {
        // It was found that enabling both SIScheduler and SIFormClauses was bad on one particular
        // game. So we disable the latter here. That only affects XNACK targets.
        targetFeatures += ",+si-scheduler";
        builder.addAttribute("amdgpu-max-memory-clause", "1");
      }

      LlvmScheduleStrategy schedStrategy = options.scheduleStrategy;
      if (schedStrategy == LlvmScheduleStrategy::MaxMemoryClause) {
        builder.addAttribute("amdgpu-sched-strategy", "max-memory-clause");
        // Use a more aggressive value than the default value. This helps clustering more instructions.
        builder.addAttribute("amdgpu-max-memory-cluster-dwords", "32");
      } else if (schedStrategy == LlvmScheduleStrategy::MaxIlp) {
        builder.addAttribute("amdgpu-sched-strategy", "max-ilp");
      }
    }

    auto callingConv = func->getCallingConv();
    if (callingConv == CallingConv::AMDGPU_GS) {
      // NOTE: For NGG primitive shader, enable 128-bit LDS load/store operations to optimize gvec4 data
      // read/write. This usage must enable the feature of using CI+ additional instructions.
      const auto nggControl = m_pipelineState->getNggControl();
      if (nggControl->enableNgg && !nggControl->passthroughMode)
        targetFeatures += ",+ci-insts,+enable-ds128";
    }

    if (callingConv == CallingConv::AMDGPU_HS) {
      // Force s_barrier to be present (ignore optimization)
      builder.addAttribute("amdgpu-flat-work-group-size", "128,128");
    }

    if (callingConv == CallingConv::AMDGPU_CS || callingConv == CallingConv::AMDGPU_Gfx ||
        callingConv == CallingConv::AMDGPU_CS_Chain) {
      // Set the work group size
      const auto &computeMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
      unsigned flatWorkGroupSize = computeMode.workgroupSizeX * computeMode.workgroupSizeY * computeMode.workgroupSizeZ;
      SmallVector<char, 8> attributeBuf;
      builder.addAttribute("amdgpu-flat-work-group-size",
                           (Twine(flatWorkGroupSize) + "," + Twine(flatWorkGroupSize)).toStringRef(attributeBuf));
    }
    if (callingConv == CallingConv::AMDGPU_CS) {
      // Tag the position of MultiDispatchInfo argument, so the backend knows which
      // sgpr needs to be preloaded for COMPUTE_PGM_RSRC2.tg_size_en (Work-Group Info).
      // This is needed for LDS spilling.
      for (unsigned i = 0, e = func->arg_size(); i != e; ++i) {
        if (func->getArg(i)->getName() == "MultiDispatchInfo") {
          builder.addAttribute("amdgpu-work-group-info-arg-no", std::to_string(i));
        }
      }
    }

    auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
    if (gfxIp.major >= 12) {
      if (m_pipelineState->getOptions().expertSchedulingMode)
        builder.addAttribute("amdgpu-expert-scheduling", "true");

      if (m_pipelineState->getOptions().disableDynamicVgpr ||
          m_pipelineState->getOptions().rtIndirectMode <= RayTracingIndirectMode::Legacy) {
        targetFeatures += ",-dynamic-vgpr";
      } else {
        targetFeatures += ",+dynamic-vgpr";

        // Set the dVGPR block size, unless it's unspecified or equal to LLVM's
        // default value.
        auto blockSize = m_pipelineState->getOptions().dynamicVgprBlockSize;
        if (blockSize != 0 && blockSize != 16)
          targetFeatures += ",+dynamic-vgpr-block-size-" + std::to_string(blockSize);
      }
    }

    // NOTE: The sub-attribute 'wavefrontsize' of 'target-features' is set in advance to let optimization
    // pass know we are in which wavesize mode. Here, we read back it and append it to finalized target
    // feature strings.
    if (func->hasFnAttribute("target-features"))
      targetFeatures += func->getFnAttribute("target-features").getValueAsString();

    if (shaderStage.has_value()) {
      if (m_pipelineState->getShaderWgpMode(shaderStage.value()))
        targetFeatures += ",-cumode";
      else
        targetFeatures += ",+cumode";
    }

    // Enable flat scratch for gfx10.3+
    if (gfxIp.major == 10 && gfxIp.minor >= 3)
      targetFeatures += ",+enable-flat-scratch";

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportsXnack) {
      // Enable or disable xnack depending on whether page migration is enabled.
      if (m_pipelineState->getOptions().pageMigrationEnabled)
        targetFeatures += ",+xnack";
      else
        targetFeatures += ",-xnack";
    }

    // Set up denormal mode attributes.

    // In the backend, f32 denormals are handled by default, so request denormal flushing behavior.
    builder.addAttribute("denormal-fp-math-f32", "preserve-sign");

    if (shaderStage.has_value() && shaderStage != ShaderStage::CopyShader) {
      const auto &shaderMode = m_pipelineState->getShaderModes()->getCommonShaderMode(shaderStage.value());
      if (shaderMode.fp16DenormMode == FpDenormMode::FlushNone || shaderMode.fp16DenormMode == FpDenormMode::FlushIn ||
          shaderMode.fp64DenormMode == FpDenormMode::FlushNone || shaderMode.fp64DenormMode == FpDenormMode::FlushIn) {
        builder.addAttribute("denormal-fp-math", "ieee");
      } else if (shaderMode.fp16DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp16DenormMode == FpDenormMode::FlushInOut ||
                 shaderMode.fp64DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp64DenormMode == FpDenormMode::FlushInOut) {
        builder.addAttribute("denormal-fp-math", "preserve-sign");
      }
      if (shaderMode.fp32DenormMode == FpDenormMode::FlushNone || shaderMode.fp32DenormMode == FpDenormMode::FlushIn) {
        builder.addAttribute("denormal-fp-math-f32", "ieee");
      } else if (shaderMode.fp32DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp32DenormMode == FpDenormMode::FlushInOut) {
        builder.addAttribute("denormal-fp-math-f32", "preserve-sign");
      }
    }

    builder.addAttribute("target-features", targetFeatures);

    // Prevent spilling of VGPRs holding SGPR spills as this can have undefined behaviour in callee functions.
    // Note: this is an intermediate workaround and should be removed when backend support is complete.
    builder.addAttribute("amdgpu-prealloc-sgpr-spill-vgprs");

    func->addFnAttrs(builder);
  }
}
