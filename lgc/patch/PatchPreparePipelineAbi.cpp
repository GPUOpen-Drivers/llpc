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
* @file  PatchPreparePipelineAbi.cpp
* @brief LLPC source file: contains implementation of class lgc::PatchPreparePipelineAbi.
***********************************************************************************************************************
*/
#include "lgc/patch/PatchPreparePipelineAbi.h"
#include "Gfx6ConfigBuilder.h"
#include "Gfx9ConfigBuilder.h"
#include "MeshTaskShader.h"
#include "ShaderMerger.h"
#include "lgc/state/PalMetadata.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-prepare-pipeline-abi"

using namespace llvm;
using namespace lgc;

char LegacyPatchPreparePipelineAbi::ID = 0;

// =====================================================================================================================
// Create pass to prepare the pipeline ABI
//
// @param onlySetCallingConvs : Should we only set the calling conventions, or do the full prepare.
ModulePass *lgc::createLegacyPatchPreparePipelineAbi(bool onlySetCallingConvs) {
  return new LegacyPatchPreparePipelineAbi(onlySetCallingConvs);
}

// =====================================================================================================================
PatchPreparePipelineAbi::PatchPreparePipelineAbi(bool onlySetCallingConvs)
    : m_onlySetCallingConvs(onlySetCallingConvs) {
}

// =====================================================================================================================
LegacyPatchPreparePipelineAbi::LegacyPatchPreparePipelineAbi(bool onlySetCallingConvs)
    : ModulePass(ID), m_impl(onlySetCallingConvs) {
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchPreparePipelineAbi::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  PipelineShadersResult &pipelineShaders = getAnalysis<LegacyPipelineShaders>().getResult();
  return m_impl.runImpl(module, pipelineShaders, pipelineState);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchPreparePipelineAbi::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  runImpl(module, pipelineShaders, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] pipelineShaders : Pipeline shaders result object to use for this pass
// @param [in/out] pipelineState : Pipeline state object to use for this pass
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchPreparePipelineAbi::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                      PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Prepare-Pipeline-Abi\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  m_pipelineShaders = &pipelineShaders;

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
  m_hasTask = m_pipelineState->hasShaderStage(ShaderStageTask);
  m_hasMesh = m_pipelineState->hasShaderStage(ShaderStageMesh);

  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  // If we've only to set the calling conventions, do that now.
  if (m_onlySetCallingConvs) {
    setCallingConvs(module);
    setRemainingCallingConvs(module);
  } else {
    if (m_gfxIp.major >= 9)
      mergeShaderAndSetCallingConvs(module);

    setAbiEntryNames(module);
    setRemainingCallingConvs(module);

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
  setCallingConv(ShaderStageTask, CallingConv::AMDGPU_CS);
  setCallingConv(ShaderStageMesh, CallingConv::AMDGPU_GS);

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
  setCallingConv(ShaderStageTask, CallingConv::AMDGPU_CS);
  setCallingConv(ShaderStageMesh, CallingConv::AMDGPU_GS);

  if (m_pipelineState->isGraphics()) {
    if (m_hasTask || m_hasMesh) {
      auto taskEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTask);
      auto meshEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageMesh);
      MeshTaskShader meshTaskShader(m_pipelineState);
      meshTaskShader.process(taskEntryPoint, meshEntryPoint);
      return;
    }

    ShaderMerger shaderMerger(m_pipelineState, m_pipelineShaders);
    const bool enableNgg = m_pipelineState->getNggControl()->enableNgg;

    if (hasTs && m_hasGs) {
      // TS-GS pipeline
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

      // This must be done after generating the EsGs entry point because it must appear first in the module.
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
INITIALIZE_PASS(LegacyPatchPreparePipelineAbi, DEBUG_TYPE, "Patch LLVM for preparing pipeline ABI", false, false)
