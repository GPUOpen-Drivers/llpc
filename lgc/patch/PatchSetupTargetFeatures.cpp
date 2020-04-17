/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-patch-setup-target-features"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Pass to set up target features on shader entry-points
class PatchSetupTargetFeatures : public Patch {
public:
  static char ID;
  PatchSetupTargetFeatures() : Patch(ID) {}

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
  }

  bool runOnModule(Module &module) override;

  PatchSetupTargetFeatures(const PatchSetupTargetFeatures &) = delete;
  PatchSetupTargetFeatures &operator=(const PatchSetupTargetFeatures &) = delete;

private:
  void setupTargetFeatures(Module *module);
  ShaderStage getShaderStageFromCallingConv(CallingConv::ID callConv);

  PipelineState *m_pipelineState;
};

char PatchSetupTargetFeatures::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create pass to set up target features
ModulePass *lgc::createPatchSetupTargetFeatures() {
  return new PatchSetupTargetFeatures();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchSetupTargetFeatures::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Setup-Target-Features\n");

  Patch::init(&module);

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  setupTargetFeatures(&module);

  return true; // Modified the module.
}

// =====================================================================================================================
// Setup LLVM target features, target features are set per entry point function.
//
// @param [in, out] module : LLVM module
void PatchSetupTargetFeatures::setupTargetFeatures(Module *module) {
  std::string globalFeatures = "";

  if (m_pipelineState->getOptions().includeDisassembly)
    globalFeatures += ",+DumpCode";

  for (auto func = module->begin(), end = module->end(); func != end; ++func) {
    if (!func->empty() && func->getLinkage() == GlobalValue::ExternalLinkage) {
      std::string targetFeatures(globalFeatures);
      AttrBuilder builder;

      ShaderStage shaderStage = getShaderStageFromCallingConv(func->getCallingConv());

      bool useSiScheduler = m_pipelineState->getShaderOptions(shaderStage).useSiScheduler;
      if (useSiScheduler) {
        // It was found that enabling both SIScheduler and SIFormClauses was bad on one particular
        // game. So we disable the latter here. That only affects XNACK targets.
        targetFeatures += ",+si-scheduler";
        builder.addAttribute("amdgpu-max-memory-clause", "1");
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
      if (func->getCallingConv() == CallingConv::AMDGPU_CS) {
        // Set the work group size
        const auto &csBuiltInUsage = m_pipelineState->getShaderModes()->getComputeShaderMode();
        unsigned flatWorkGroupSize =
            csBuiltInUsage.workgroupSizeX * csBuiltInUsage.workgroupSizeY * csBuiltInUsage.workgroupSizeZ;
        auto flatWorkGroupSizeString = std::to_string(flatWorkGroupSize);
        builder.addAttribute("amdgpu-flat-work-group-size", flatWorkGroupSizeString + "," + flatWorkGroupSizeString);
      }

      auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
      if (gfxIp.major >= 9)
        targetFeatures += ",+enable-scratch-bounds-checks";

      if (gfxIp.major >= 10) {
        // Setup wavefront size per shader stage
        unsigned waveSize = m_pipelineState->getShaderWaveSize(shaderStage);

        targetFeatures += ",+wavefrontsize" + std::to_string(waveSize);

        // Allow driver setting for WGP by forcing backend to set 0
        // which is then OR'ed with the driver set value
        targetFeatures += ",+cumode";
      }

      // Set up denormal mode attributes.

      // In the backend, f32 denormals are handled by default, so request denormal flushing behavior.
      builder.addAttribute("denormal-fp-math-f32", "preserve-sign");

      if (shaderStage != ShaderStageCopyShader && shaderStage != ShaderStageFetch) {
        const auto &shaderMode = m_pipelineState->getShaderModes()->getCommonShaderMode(shaderStage);
        if (shaderMode.fp16DenormMode == FpDenormMode::FlushNone ||
            shaderMode.fp16DenormMode == FpDenormMode::FlushIn ||
            shaderMode.fp64DenormMode == FpDenormMode::FlushNone || shaderMode.fp64DenormMode == FpDenormMode::FlushIn) {
          builder.addAttribute("denormal-fp-math", "ieee");
        }
        else if (shaderMode.fp16DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp16DenormMode == FpDenormMode::FlushInOut ||
                 shaderMode.fp64DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp64DenormMode == FpDenormMode::FlushInOut) {
          builder.addAttribute("denormal-fp-math", "preserve-sign");
        }
        if (shaderMode.fp32DenormMode == FpDenormMode::FlushNone || shaderMode.fp32DenormMode == FpDenormMode::FlushIn) {
          builder.addAttribute("denormal-fp-math-f32", "ieee");
        }
        else if (shaderMode.fp32DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp32DenormMode == FpDenormMode::FlushInOut) {
          builder.addAttribute("denormal-fp-math-f32", "preserve-sign");
        }
      }

      builder.addAttribute("target-features", targetFeatures);
      AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
      func->addAttributes(attribIdx, builder);
    }
  }
}

// =====================================================================================================================
// Gets the shader stage from the specified calling convention.
//
// @param callConv : Calling convention
ShaderStage PatchSetupTargetFeatures::getShaderStageFromCallingConv(CallingConv::ID callConv) {
  ShaderStage shaderStage = ShaderStageInvalid;
  auto stageMask = m_pipelineState->getShaderStageMask();

  bool hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;
  bool hasTs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0 ||
                (stageMask & shaderStageToMask(ShaderStageTessEval)) != 0);

  switch (callConv) {
  case CallingConv::AMDGPU_PS:
    shaderStage = ShaderStageFragment;
    break;
  case CallingConv::AMDGPU_LS:
    shaderStage = ShaderStageVertex;
    break;
  case CallingConv::AMDGPU_HS:
    shaderStage = ShaderStageTessControl;
    break;
  case CallingConv::AMDGPU_ES:
    shaderStage = hasTs ? ShaderStageTessEval : ShaderStageVertex;
    break;
  case CallingConv::AMDGPU_GS:
    // NOTE: If GS is not present, this must be NGG.
    shaderStage = hasGs ? ShaderStageGeometry : (hasTs ? ShaderStageTessEval : ShaderStageVertex);
    break;
  case CallingConv::AMDGPU_VS:
    shaderStage = hasGs ? ShaderStageCopyShader : (hasTs ? ShaderStageTessEval : ShaderStageVertex);
    break;
  case CallingConv::AMDGPU_CS:
    shaderStage = ShaderStageCompute;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return shaderStage;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchSetupTargetFeatures, DEBUG_TYPE, "Patch LLVM to set up target features", false, false)
