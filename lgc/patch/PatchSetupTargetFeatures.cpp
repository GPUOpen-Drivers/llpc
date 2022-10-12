/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  PatchSetupTargetFeatures.cpp
* @brief LLPC source file: contains declaration and implementation of class lgc::PatchSetupTargetFeatures.
***********************************************************************************************************************
*/
#include "lgc/patch/PatchSetupTargetFeatures.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-setup-target-features"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Pass to set up target features on shader entry-points
class LegacyPatchSetupTargetFeatures : public LegacyPatch {
public:
  static char ID;
  LegacyPatchSetupTargetFeatures() : LegacyPatch(ID) {}

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<LegacyPipelineStateWrapper>();
  }

  bool runOnModule(Module &module) override;

  LegacyPatchSetupTargetFeatures(const LegacyPatchSetupTargetFeatures &) = delete;
  LegacyPatchSetupTargetFeatures &operator=(const LegacyPatchSetupTargetFeatures &) = delete;

private:
  PatchSetupTargetFeatures m_impl;
};

char LegacyPatchSetupTargetFeatures::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create pass to set up target features
ModulePass *lgc::createLegacyPatchSetupTargetFeatures() {
  return new LegacyPatchSetupTargetFeatures();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchSetupTargetFeatures::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  return m_impl.runImpl(module, pipelineState);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchSetupTargetFeatures::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  runImpl(module, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchSetupTargetFeatures::runImpl(Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Setup-Target-Features\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  setupTargetFeatures(&module);

  return true; // Modified the module.
}

// =====================================================================================================================
// Setup LLVM target features, target features are set per entry point function.
//
// @param [in/out] module : LLVM module
void PatchSetupTargetFeatures::setupTargetFeatures(Module *module) {
  std::string globalFeatures = "";

  if (m_pipelineState->getOptions().includeDisassembly)
    globalFeatures += ",+DumpCode";

  for (auto func = module->begin(), end = module->end(); func != end; ++func) {
    if (func->isDeclaration())
      continue;

    std::string targetFeatures(globalFeatures);
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 409358
    // Old version of the code
    AttrBuilder builder;
#else
    // New version of the code (also handles unknown version, which we treat as latest)
    AttrBuilder builder(module->getContext());
#endif

    ShaderStage shaderStage = lgc::getShaderStage(&*func);

    if (shaderStage == ShaderStage::ShaderStageInvalid) {
      errs() << "Invalid shader stage for function " << func->getName() << "\n";
      report_fatal_error("Got invalid shader stage when setting up features for function");
    }

    if (isShaderEntryPoint(&*func)) {
      bool useSiScheduler = m_pipelineState->getShaderOptions(shaderStage).useSiScheduler;
      if (useSiScheduler) {
        // It was found that enabling both SIScheduler and SIFormClauses was bad on one particular
        // game. So we disable the latter here. That only affects XNACK targets.
        targetFeatures += ",+si-scheduler";
        builder.addAttribute("amdgpu-max-memory-clause", "1");
      }
    }

    if (func->getCallingConv() == CallingConv::AMDGPU_GS) {
      // NOTE: For NGG primitive shader, enable 128-bit LDS load/store operations to optimize gvec4 data
      // read/write. This usage must enable the feature of using CI+ additional instructions.
      const auto nggControl = m_pipelineState->getNggControl();
      if (nggControl->enableNgg && !nggControl->passthroughMode)
        targetFeatures += ",+ci-insts,+enable-ds128";
    }

    if (func->getCallingConv() == CallingConv::AMDGPU_HS) {
      // Force s_barrier to be present (ignore optimization)
      builder.addAttribute("amdgpu-flat-work-group-size", "128,128");
    }
    if (func->getCallingConv() == CallingConv::AMDGPU_CS || func->getCallingConv() == CallingConv::AMDGPU_Gfx) {
      // Set the work group size
      const auto &computeMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
      unsigned flatWorkGroupSize = computeMode.workgroupSizeX * computeMode.workgroupSizeY * computeMode.workgroupSizeZ;
      SmallVector<char, 8> attributeBuf;
      builder.addAttribute("amdgpu-flat-work-group-size",
                           (Twine(flatWorkGroupSize) + "," + Twine(flatWorkGroupSize)).toStringRef(attributeBuf));
    }
    if (func->getCallingConv() == CallingConv::AMDGPU_CS) {
      // Tag the position of MultiDispatchInfo argument, so the backend knows which
      // sgpr needs to be preloaded for COMPUTE_PGM_RSRC2.tg_size_en (Work-Group Info).
      // This is needed for LDS spilling.
      for (unsigned i = 0, e = func->arg_size(); i != e; ++i) {
        if (func->getArg(i)->getName().equals("MultiDispatchInfo")) {
          builder.addAttribute("amdgpu-work-group-info-arg-no", std::to_string(i));
        }
      }
    }

    auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

    if (gfxIp.major >= 10) {
      // Setup wavefront size per shader stage
      unsigned waveSize = m_pipelineState->getShaderWaveSize(shaderStage);

      targetFeatures += ",+wavefrontsize" + std::to_string(waveSize);

      // Allow driver setting for WGP by forcing backend to set 0
      // which is then OR'ed with the driver set value
      targetFeatures += ",+cumode";
    }

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 414671
    // Old version of the code
#else
    // New version of the code (also handles unknown version, which we treat as latest)
    // Enable flat scratch for gfx10.3+
    if (gfxIp.major == 10 && gfxIp.minor >= 3)
      targetFeatures += ",+enable-flat-scratch";
#endif

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

    if (shaderStage != ShaderStageCopyShader) {
      const auto &shaderMode = m_pipelineState->getShaderModes()->getCommonShaderMode(shaderStage);
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

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 396807
    // Old version of the code
    AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
    func->addAttributes(attribIdx, builder);
#else
    // New version of the code (also handles unknown version, which we treat as
    // latest)
    func->addFnAttrs(builder);
#endif
  }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(LegacyPatchSetupTargetFeatures, DEBUG_TYPE, "Patch LLVM to set up target features", false, false)
