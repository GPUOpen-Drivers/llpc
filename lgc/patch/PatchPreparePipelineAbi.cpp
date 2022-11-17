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
#include "llvm/IR/IntrinsicsAMDGPU.h"
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
ModulePass *lgc::createLegacyPatchPreparePipelineAbi() {
  return new LegacyPatchPreparePipelineAbi;
}

// =====================================================================================================================
PatchPreparePipelineAbi::PatchPreparePipelineAbi() {
}

// =====================================================================================================================
LegacyPatchPreparePipelineAbi::LegacyPatchPreparePipelineAbi() : ModulePass(ID) {
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchPreparePipelineAbi::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  PipelineShadersResult &pipelineShaders = getAnalysis<LegacyPipelineShaders>().getResult();

  auto getPostDomTree = [&](Function &func) -> PostDominatorTree & {
    return getAnalysis<PostDominatorTreeWrapperPass>(func).getPostDomTree();
  };
  auto getCycleInfo = [&](Function &func) -> CycleInfo & {
    return getAnalysis<CycleInfoWrapperPass>(func).getCycleInfo();
  };

  PatchPreparePipelineAbi::FunctionAnalysisHandlers analysisHandlers = {};
  analysisHandlers.getPostDomTree = getPostDomTree;
  analysisHandlers.getCycleInfo = getCycleInfo;

  return m_impl.runImpl(module, pipelineShaders, pipelineState, analysisHandlers);
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

  auto getPostDomTree = [&](Function &func) -> PostDominatorTree & {
    auto &funcAnalysisManager = analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
    return funcAnalysisManager.getResult<PostDominatorTreeAnalysis>(func);
  };
  auto getCycleInfo = [&](Function &func) -> CycleInfo & {
    auto &funcAnalysisManager = analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
    return funcAnalysisManager.getResult<CycleAnalysis>(func);
  };

  FunctionAnalysisHandlers analysisHandlers = {};
  analysisHandlers.getPostDomTree = getPostDomTree;
  analysisHandlers.getCycleInfo = getCycleInfo;

  runImpl(module, pipelineShaders, pipelineState, analysisHandlers);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] pipelineShaders : Pipeline shaders result object to use for this pass
// @param [in/out] pipelineState : Pipeline state object to use for this pass
// @param analysisHandlers : A collection of handler functions to get the analysis info of the given function
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchPreparePipelineAbi::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                      PipelineState *pipelineState, FunctionAnalysisHandlers &analysisHandlers) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Prepare-Pipeline-Abi\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  m_pipelineShaders = &pipelineShaders;
  m_analysisHandlers = &analysisHandlers;

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
  m_hasTask = m_pipelineState->hasShaderStage(ShaderStageTask);
  m_hasMesh = m_pipelineState->hasShaderStage(ShaderStageMesh);

  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  if (auto hsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTessControl))
    storeTessFactors(hsEntryPoint);

  if (m_gfxIp.major >= 9)
    mergeShader(module);

  setAbiEntryNames(module);

  addAbiMetadata(module);

  m_pipelineState->getPalMetadata()->finalizePipeline(m_pipelineState->isWholePipeline());

  return true; // Modified the module.
}

// =====================================================================================================================
// Read tessellation factors from on-chip LDS.
//
// @param pipelineState : Pipeline state
// @param relPatchId : Relative patch ID
// @param builder : IR builder to insert instructions
std::pair<Value *, Value *> PatchPreparePipelineAbi::readTessFactors(PipelineState *pipelineState, Value *relPatchId,
                                                                     IRBuilder<> &builder) {
  auto module = builder.GetInsertBlock()->getModule();
  auto lds = Patch::getLdsVariable(pipelineState, module);

  // Helper to read value from LDS
  auto readValueFromLds = [&](Type *readTy, Value *ldsOffset) {
    assert(readTy->getScalarSizeInBits() == 32); // Only accept 32-bit data

    Value *readPtr = builder.CreateGEP(lds->getValueType(), lds, {builder.getInt32(0), ldsOffset});
    readPtr = builder.CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
    return builder.CreateLoad(readTy, readPtr);
  };

  unsigned numOuterTfs = 0;
  unsigned numInnerTfs = 0;

  const auto primitiveMode = pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
  switch (primitiveMode) {
  case PrimitiveMode::Triangles:
    numOuterTfs = 3;
    numInnerTfs = 1;
    break;
  case PrimitiveMode::Quads:
    numOuterTfs = 4;
    numInnerTfs = 2;
    break;
  case PrimitiveMode::Isolines:
    numOuterTfs = 2;
    numInnerTfs = 0;
    break;
  default:
    llvm_unreachable("Unknown primitive mode!");
    break;
  }

  const auto tessFactorStart =
      pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor.onChip.tessFactorStart;

  assert(numOuterTfs >= 2 && numOuterTfs <= 4);
  // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch
  Value *ldsOffset = builder.CreateMul(relPatchId, builder.getInt32(MaxTessFactorsPerPatch));
  ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(tessFactorStart));
  Value *outerTf = readValueFromLds(FixedVectorType::get(builder.getFloatTy(), numOuterTfs), ldsOffset);

  // NOTE: For isoline, the outer tessellation factors have to be exchanged, which is required by HW.
  if (primitiveMode == PrimitiveMode::Isolines) {
    assert(numOuterTfs == 2);
    outerTf = builder.CreateShuffleVector(outerTf, ArrayRef<int>{1, 0});
  }

  assert(numInnerTfs <= 2);
  Value *innerTf = nullptr;
  if (numInnerTfs > 0) {
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + 4
    Value *ldsOffset = builder.CreateMul(relPatchId, builder.getInt32(MaxTessFactorsPerPatch));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(tessFactorStart + 4));
    innerTf = readValueFromLds(FixedVectorType::get(builder.getFloatTy(), numInnerTfs), ldsOffset);
  }

  return std::make_pair(outerTf, innerTf);
}

// =====================================================================================================================
// Write tessellation factors to TF buffer.
//
// @param pipelineState : Pipeline state
// @param tfBufferDesc : TF buffer descriptor
// @param tfBufferBase : TF buffer base offset
// @param relPatchId : Relative patch ID
// @param outerTf : Outer tessellation factors to write to TF buffer
// @param innerTf : Inner tessellation factors to write to TF buffer
// @param builder : IR builder to insert instructions
void PatchPreparePipelineAbi::writeTessFactors(PipelineState *pipelineState, Value *tfBufferDesc, Value *tfBufferBase,
                                               Value *relPatchId, Value *outerTf, Value *innerTf,
                                               IRBuilder<> &builder) {
  // NOTE: Tessellation factors are from tessellation level array and we have:
  //   Isoline:
  //     TF[0] = outerTF[0]
  //     TF[1] = outerTF[1]
  //   Triangle:
  //     TF[0] = outerTF[0]
  //     TF[1] = outerTF[1]
  //     TF[2] = outerTF[2]
  //     TF[3] = innerTF[0]
  //   Quad:
  //     TF[0] = outerTF[0]
  //     TF[1] = outerTF[1]
  //     TF[2] = outerTF[2]
  //     TF[3] = outerTF[3]
  //     TF[4] = innerTF[0]
  //     TF[5] = innerTF[1]
  if (pipelineState->isTessOffChip()) {
    if (pipelineState->getTargetInfo().getGfxIpVersion().major <= 8) {
      // NOTE: Additional 4-byte offset is required for tessellation off-chip mode (pre-GFX9).
      tfBufferBase = builder.CreateAdd(tfBufferBase, builder.getInt32(4));
    }
  }

  const auto &calcFactor = pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  Value *tfBufferOffset = builder.CreateMul(relPatchId, builder.getInt32(calcFactor.tessFactorStride * sizeof(float)));

  CoherentFlag coherent = {};
  coherent.bits.glc = true;

  const auto numOuterTfs = cast<FixedVectorType>(outerTf->getType())->getNumElements();
  const auto numInnerTfs = innerTf ? cast<FixedVectorType>(innerTf->getType())->getNumElements()
                                   : 0; // Isoline doesn't have inner tessellation factors

  (void(numOuterTfs)); // Unused
  (void(numInnerTfs));

  auto bufferFormatX2 = BUF_NUM_FORMAT_FLOAT << 4 | BUF_DATA_FORMAT_32_32;
  auto bufferFormatX4 = BUF_NUM_FORMAT_FLOAT << 4 | BUF_DATA_FORMAT_32_32_32_32;
  if (pipelineState->getTargetInfo().getGfxIpVersion().major == 10) {
    bufferFormatX2 = BUF_FORMAT_32_32_FLOAT_GFX10;
    bufferFormatX4 = BUF_FORMAT_32_32_32_32_FLOAT_GFX10;
  }

  auto primitiveMode = pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
  if (primitiveMode == PrimitiveMode::Isolines) {
    assert(numOuterTfs == 2 && numInnerTfs == 0);

    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outerTf->getType(),
                            {outerTf,                             // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(bufferFormatX2),    // format
                             builder.getInt32(coherent.u32All)}); // glc

  } else if (primitiveMode == PrimitiveMode::Triangles) {
    assert(numOuterTfs == 3 && numInnerTfs == 1);

    // For triangle, we can combine outer tessellation factors with inner ones
    Value *tessFactor = builder.CreateShuffleVector(outerTf, ArrayRef<int>{0, 1, 2, 3});
    tessFactor =
        builder.CreateInsertElement(tessFactor, builder.CreateExtractElement(innerTf, static_cast<uint64_t>(0)), 3);

    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, tessFactor->getType(),
                            {tessFactor,                          // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(bufferFormatX4),    // format
                             builder.getInt32(coherent.u32All)}); // glc
  } else {
    assert(primitiveMode == PrimitiveMode::Quads);
    assert(numOuterTfs == 4 && numInnerTfs == 2);

    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outerTf->getType(),
                            {outerTf,                             // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(bufferFormatX4),    // format
                             builder.getInt32(coherent.u32All)}); // glc

    tfBufferOffset = builder.CreateAdd(tfBufferOffset, builder.getInt32(4 * sizeof(float)));
    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, innerTf->getType(),
                            {innerTf,                             // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(bufferFormatX2),    // format
                             builder.getInt32(coherent.u32All)}); // glc
  }
}

// =====================================================================================================================
// Merge shaders and set calling convention for the entry-point of each shader (GFX9+)
//
// @param module : LLVM module
void PatchPreparePipelineAbi::mergeShader(Module &module) {
  assert(m_gfxIp.major >= 9);

  const bool hasTs = (m_hasTcs || m_hasTes);

  if (m_pipelineState->isGraphics()) {
    if (m_hasTask || m_hasMesh) {
      auto taskEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageTask);
      auto meshEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStageMesh);
      MeshTaskShader meshTaskShader(m_pipelineState, m_analysisHandlers);
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
      }
    }
  }
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
// Handle the store of tessellation factors.
//
// @param entryPoint : Entry-point of tessellation control shader
void PatchPreparePipelineAbi::storeTessFactors(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTessControl); // Must be tessellation control shader

  // Find the return instruction
  Instruction *retInst = nullptr;
  for (auto &block : *entryPoint) {
    retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      assert(retInst->getType()->isVoidTy());
      break;
    }
  }
  assert(retInst); // Must have return instruction

  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(retInst);

  PipelineSystemValues pipelineSysValues;
  pipelineSysValues.initialize(m_pipelineState);

  const auto tfBufferDesc = pipelineSysValues.get(entryPoint)->getTessFactorBufDesc();
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl)->entryArgIdxs.tcs;
  const auto tfBufferBase = getFunctionArgument(entryPoint, entryArgIdxs.tfBufferBase);
  const auto relPatchId = pipelineSysValues.get(entryPoint)->getRelativeId();

  // Read back tessellation factors and write them to TF buffer
  auto tessFactors = readTessFactors(m_pipelineState, relPatchId, builder);
  writeTessFactors(m_pipelineState, tfBufferDesc, tfBufferBase, relPatchId, tessFactors.first, tessFactors.second,
                   builder);

  pipelineSysValues.clear();
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(LegacyPatchPreparePipelineAbi, DEBUG_TYPE, "Patch LLVM for preparing pipeline ABI", false, false)
