/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  PatchPrepareAbi.cpp
* @brief LLPC source file: contains declaration and implementation of class lgc::PatchPreparePipelineAbi.
***********************************************************************************************************************
*/
#include "Gfx6ConfigBuilder.h"
#include "Gfx9ConfigBuilder.h"
#include "ShaderMerger.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-prepare-pipeline-abi"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Pass to prepare the pipeline ABI
class PatchPreparePipelineAbi final : public LegacyPatch {
public:
  static char ID;
  PatchPreparePipelineAbi(bool onlySetCallingConvs = false)
      : LegacyPatch(ID), m_onlySetCallingConvs(onlySetCallingConvs) {}

  bool runOnModule(Module &module) override;

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<LegacyPipelineStateWrapper>();
    analysisUsage.addRequired<LegacyPipelineShaders>();
  }

private:
  PatchPreparePipelineAbi(const PatchPreparePipelineAbi &) = delete;
  PatchPreparePipelineAbi &operator=(const PatchPreparePipelineAbi &) = delete;

  void setCallingConvs(Module &module);

  void setRemainingCallingConvs(Module &module);

  void mergeShaderAndSetCallingConvs(Module &module);

  void setCallingConv(ShaderStage stage, CallingConv::ID callingConv);

  void setAbiEntryNames(Module &module);

  void addAbiMetadata(Module &module);

  PipelineState *m_pipelineState;     // Pipeline state
  LegacyPipelineShaders *m_pipelineShaders; // API shaders in the pipeline

  bool m_hasVs;  // Whether the pipeline has vertex shader
  bool m_hasTcs; // Whether the pipeline has tessellation control shader
  bool m_hasTes; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;  // Whether the pipeline has geometry shader

  GfxIpVersion m_gfxIp; // Graphics IP version info

  const bool m_onlySetCallingConvs; // Whether to only set the calling conventions
};

char PatchPreparePipelineAbi::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create pass to prepare the pipeline ABI
//
// @param onlySetCallingConvs : Should we only set the calling conventions, or do the full prepare.
ModulePass *lgc::createPatchPreparePipelineAbi(bool onlySetCallingConvs) {
  return new PatchPreparePipelineAbi(onlySetCallingConvs);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchPreparePipelineAbi::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Prepare-Pipeline-Abi\n");

  LegacyPatch::init(&module);

  m_pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  m_pipelineShaders = &getAnalysis<LegacyPipelineShaders>();

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  // If we've only to set the calling conventions, do that now.
  if (m_onlySetCallingConvs) {
    setCallingConvs(module);
    setRemainingCallingConvs(module);
  } else {
    if (m_gfxIp.major >= 9)
      mergeShaderAndSetCallingConvs(module);
    setRemainingCallingConvs(module);

    setAbiEntryNames(module);

    addAbiMetadata(module);

    m_pipelineState->getPalMetadata()->finalizePipeline(m_pipelineState->isWholePipeline());
  }

  return true; // Modified the module.
}

// =====================================================================================================================
// Set calling convention for the entry-point of each shader (pre-GFX9)
//
// @param module : LLVM module
void PatchPreparePipelineAbi::setCallingConvs(Module &module) {
  const bool hasTs = (m_hasTcs || m_hasTes);

  // NOTE: For each entry-point, set the calling convention appropriate to the hardware shader stage. The action here
  // depends on the pipeline type.
  setCallingConv(ShaderStageCompute, CallingConv::AMDGPU_CS);
  setCallingConv(ShaderStageFragment, CallingConv::AMDGPU_PS);

  if (hasTs && m_hasGs) {
    // TS-GS pipeline
    setCallingConv(ShaderStageVertex, CallingConv::AMDGPU_LS);
    setCallingConv(ShaderStageTessControl, CallingConv::AMDGPU_HS);
    setCallingConv(ShaderStageTessEval, CallingConv::AMDGPU_ES);
    setCallingConv(ShaderStageGeometry, CallingConv::AMDGPU_GS);
    setCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
  } else if (hasTs) {
    // TS-only pipeline
    setCallingConv(ShaderStageVertex, CallingConv::AMDGPU_LS);
    setCallingConv(ShaderStageTessControl, CallingConv::AMDGPU_HS);
    setCallingConv(ShaderStageTessEval, CallingConv::AMDGPU_VS);
  } else if (m_hasGs) {
    // GS-only pipeline
    setCallingConv(ShaderStageVertex, CallingConv::AMDGPU_ES);
    setCallingConv(ShaderStageGeometry, CallingConv::AMDGPU_GS);
    setCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
  } else if (m_hasVs) {
    // VS-FS pipeline
    setCallingConv(ShaderStageVertex, CallingConv::AMDGPU_VS);
  }
}

// =====================================================================================================================
// Set calling convention for the non-entry-points that do not yet have a calling convention set.
//
// @param module : LLVM module
void PatchPreparePipelineAbi::setRemainingCallingConvs(Module &module) {
  for (Function &func : module) {
    if (func.isDeclaration() || func.getIntrinsicID() != Intrinsic::not_intrinsic ||
        func.getName().startswith(lgcName::InternalCallPrefix) ||
        func.getDLLStorageClass() == GlobalValue::DLLExportStorageClass)
      continue;
    func.setCallingConv(CallingConv::AMDGPU_Gfx);
  }
}

// =====================================================================================================================
// Merge shaders and set calling convention for the entry-point of each shader (GFX9+)
//
// @param module : LLVM module
void PatchPreparePipelineAbi::mergeShaderAndSetCallingConvs(Module &module) {
  assert(m_gfxIp.major >= 9);

  const bool hasTs = (m_hasTcs || m_hasTes);

  // NOTE: For each entry-point, set the calling convention appropriate to the hardware shader stage. The action here
  // depends on the pipeline type, and, for GFX9+, may involve merging shaders.
  setCallingConv(ShaderStageCompute, CallingConv::AMDGPU_CS);
  setCallingConv(ShaderStageFragment, CallingConv::AMDGPU_PS);

  if (m_pipelineState->isGraphics()) {
    ShaderMerger shaderMerger(m_pipelineState, m_pipelineShaders);
    const bool enableNgg = m_pipelineState->getNggControl()->enableNgg;

    if (hasTs && m_hasGs) {
      // TS-GS pipeline
      if (m_hasTcs) {
        auto lsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageVertex);
        auto hsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTessControl);

        if (hsEntryPoint) {
          if (lsEntryPoint)
            lgc::setShaderStage(lsEntryPoint, ShaderStageTessControl);
          auto lsHsEntryPoint = shaderMerger.generateLsHsEntryPoint(lsEntryPoint, hsEntryPoint);
          lsHsEntryPoint->setCallingConv(CallingConv::AMDGPU_HS);
          lgc::setShaderStage(lsHsEntryPoint, ShaderStageTessControl);
        }
      }

      auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTessEval);
      auto gsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageGeometry);

      if (enableNgg) {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStageGeometry);
          auto copyShaderEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageCopyShader);
          if (copyShaderEntryPoint)
            lgc::setShaderStage(copyShaderEntryPoint, ShaderStageGeometry);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, gsEntryPoint, copyShaderEntryPoint);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStageGeometry);
        }
      } else {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStageGeometry);
          auto esGsEntryPoint = shaderMerger.generateEsGsEntryPoint(esEntryPoint, gsEntryPoint);
          esGsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(esGsEntryPoint, ShaderStageGeometry);
        }

        setCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
      }
    } else if (hasTs) {
      // TS-only pipeline
      if (m_hasTcs) {
        auto lsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageVertex);
        auto hsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTessControl);

        if (hsEntryPoint) {
          if (lsEntryPoint)
            lgc::setShaderStage(lsEntryPoint, ShaderStageTessControl);
          auto lsHsEntryPoint = shaderMerger.generateLsHsEntryPoint(lsEntryPoint, hsEntryPoint);
          lsHsEntryPoint->setCallingConv(CallingConv::AMDGPU_HS);
          lgc::setShaderStage(lsHsEntryPoint, ShaderStageTessControl);
        }
      }

      if (enableNgg) {
        // If NGG is enabled, ES-GS merged shader should be present even if GS is absent
        auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTessEval);

        if (esEntryPoint) {
          lgc::setShaderStage(esEntryPoint, ShaderStageTessEval);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, nullptr, nullptr);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStageTessEval);
        }
      } else {
        setCallingConv(ShaderStageTessEval, CallingConv::AMDGPU_VS);
      }
    } else if (m_hasGs) {
      // GS-only pipeline
      auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageVertex);
      auto gsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageGeometry);

      if (enableNgg) {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStageGeometry);
          auto copyShaderEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageCopyShader);
          if (copyShaderEntryPoint)
            lgc::setShaderStage(copyShaderEntryPoint, ShaderStageGeometry);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, gsEntryPoint, copyShaderEntryPoint);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStageGeometry);
        }
      } else {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStageGeometry);
          auto esGsEntryPoint = shaderMerger.generateEsGsEntryPoint(esEntryPoint, gsEntryPoint);
          esGsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(esGsEntryPoint, ShaderStageGeometry);
        }

        setCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
      }
    } else if (m_hasVs) {
      // VS_FS pipeline
      if (enableNgg) {
        // If NGG is enabled, ES-GS merged shader should be present even if GS is absent
        auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageVertex);
        if (esEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStageVertex);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, nullptr, nullptr);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStageVertex);
        }
      } else
        setCallingConv(ShaderStageVertex, CallingConv::AMDGPU_VS);
    }
  }
}

// =====================================================================================================================
// Set calling convention on a particular API shader stage, if that stage has a shader
//
// @param shaderStage : Shader stage
// @param callingConv : Calling convention to set it to
void PatchPreparePipelineAbi::setCallingConv(ShaderStage shaderStage, CallingConv::ID callingConv) {
  auto entryPoint = m_pipelineShaders->getEntryPoint(shaderStage);
  if (entryPoint)
    entryPoint->setCallingConv(callingConv);
}

// =====================================================================================================================
// Set ABI-specified entrypoint name for each shader
//
// @param module : LLVM module
void PatchPreparePipelineAbi::setAbiEntryNames(Module &module) {
  bool hasTs = m_hasTcs || m_hasTes;
  bool isFetchless = m_pipelineState->getPalMetadata()->getVertexFetchCount() != 0;

  for (auto &func : module) {
    if (!func.empty()) {
      auto callingConv = func.getCallingConv();
      bool isFetchlessVs = false;
      if (isFetchless) {
        switch (callingConv) {
        case CallingConv::AMDGPU_VS:
          isFetchlessVs = !m_hasGs && !hasTs;
          break;
        case CallingConv::AMDGPU_GS:
          isFetchlessVs = m_gfxIp.major >= 9 && !hasTs;
          break;
        case CallingConv::AMDGPU_ES:
          isFetchlessVs = !hasTs;
          break;
        case CallingConv::AMDGPU_HS:
          isFetchlessVs = m_gfxIp.major >= 9;
          break;
        case CallingConv::AMDGPU_LS:
          isFetchlessVs = true;
          break;
        default:
          break;
        }
      }
      StringRef entryName = getEntryPointName(callingConv, isFetchlessVs);
      if (entryName != "")
        func.setName(entryName);
    }
  }
}

// =====================================================================================================================
// Add ABI metadata
//
// @param module : LLVM module
void PatchPreparePipelineAbi::addAbiMetadata(Module &module) {
  if (m_gfxIp.major <= 8) {
    Gfx6::ConfigBuilder configBuilder(&module, m_pipelineState);
    configBuilder.buildPalMetadata();
  } else {
    Gfx9::ConfigBuilder configBuilder(&module, m_pipelineState);
    configBuilder.buildPalMetadata();
  }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchPreparePipelineAbi, DEBUG_TYPE, "Patch LLVM for preparing pipeline ABI", false, false)
