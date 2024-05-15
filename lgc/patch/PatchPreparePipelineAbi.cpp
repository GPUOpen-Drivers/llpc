/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  PatchPreparePipelineAbi.cpp
* @brief LLPC source file: contains implementation of class lgc::PatchPreparePipelineAbi.
***********************************************************************************************************************
*/
#include "lgc/patch/PatchPreparePipelineAbi.h"
#include "MeshTaskShader.h"
#include "RegisterMetadataBuilder.h"
#include "ShaderMerger.h"
#include "lgc/state/PalMetadata.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-prepare-pipeline-abi"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
PatchPreparePipelineAbi::PatchPreparePipelineAbi() {
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

  LLVM_DEBUG(dbgs() << "Run the pass Patch-Prepare-Pipeline-Abi\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  m_pipelineShaders = &pipelineShaders;
  m_analysisHandlers = &analysisHandlers;

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStage::Vertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStage::TessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStage::TessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStage::Geometry);
  m_hasTask = m_pipelineState->hasShaderStage(ShaderStage::Task);
  m_hasMesh = m_pipelineState->hasShaderStage(ShaderStage::Mesh);

  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  if (auto hsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::TessControl))
    storeTessFactors(hsEntryPoint);

  mergeShader(module);

  setAbiEntryNames(module);

  addAbiMetadata(module);

  m_pipelineState->getPalMetadata()->finalizePipeline(m_pipelineState->isWholePipeline());

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Read tessellation factors from on-chip LDS.
//
// @param pipelineState : Pipeline state
// @param relPatchId : Relative patch ID
// @param builder : IR builder to insert instructions
std::pair<Value *, Value *> PatchPreparePipelineAbi::readTessFactors(PipelineState *pipelineState, Value *relPatchId,
                                                                     IRBuilder<> &builder) {
  auto func = builder.GetInsertBlock()->getParent();
  auto lds = Patch::getLdsVariable(pipelineState, func);

  // Helper to read value from LDS
  auto readValueFromLds = [&](Type *readTy, Value *ldsOffset) {
    assert(readTy->getScalarSizeInBits() == 32); // Only accept 32-bit data

    Value *readPtr = builder.CreateGEP(builder.getInt32Ty(), lds, ldsOffset);
    readPtr = builder.CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
    return builder.CreateAlignedLoad(readTy, readPtr, Align(4));
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
      pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.calcFactor.onChip.tessFactorStart;

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
  const auto &calcFactor = pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.calcFactor;
  Value *tfBufferOffset = builder.CreateMul(relPatchId, builder.getInt32(calcFactor.tessFactorStride * sizeof(float)));

  CoherentFlag coherent = {};
  if (pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    coherent.bits.glc = true;
  }

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
  } else if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 11) {
    bufferFormatX2 = BUF_FORMAT_32_32_FLOAT_GFX11;
    bufferFormatX4 = BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
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
  const bool hasTs = (m_hasTcs || m_hasTes);

  if (m_pipelineState->isGraphics()) {
    if (m_hasTask || m_hasMesh) {
      auto taskEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Task);
      auto meshEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Mesh);
      MeshTaskShader meshTaskShader(m_pipelineState, m_analysisHandlers);
      meshTaskShader.process(taskEntryPoint, meshEntryPoint);
      return;
    }

    ShaderMerger shaderMerger(m_pipelineState, m_pipelineShaders);
    const bool enableNgg = m_pipelineState->getNggControl()->enableNgg;

    if (hasTs && m_hasGs) {
      // TS-GS pipeline
      auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::TessEval);
      auto gsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Geometry);

      if (enableNgg) {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStage::Geometry);
          auto copyShaderEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::CopyShader);
          if (copyShaderEntryPoint)
            lgc::setShaderStage(copyShaderEntryPoint, ShaderStage::Geometry);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, gsEntryPoint, copyShaderEntryPoint);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStage::Geometry);
        }
      } else {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStage::Geometry);
          auto esGsEntryPoint = shaderMerger.generateEsGsEntryPoint(esEntryPoint, gsEntryPoint);
          esGsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(esGsEntryPoint, ShaderStage::Geometry);
        }
      }

      // This must be done after generating the EsGs entry point because it must appear first in the module.
      if (m_hasTcs) {
        auto lsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Vertex);
        auto hsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::TessControl);

        if (hsEntryPoint) {
          if (lsEntryPoint)
            lgc::setShaderStage(lsEntryPoint, ShaderStage::TessControl);
          auto lsHsEntryPoint = shaderMerger.generateLsHsEntryPoint(lsEntryPoint, hsEntryPoint);
          lsHsEntryPoint->setCallingConv(CallingConv::AMDGPU_HS);
          lgc::setShaderStage(lsHsEntryPoint, ShaderStage::TessControl);
        }
      }
    } else if (hasTs) {
      // TS-only pipeline
      if (m_hasTcs) {
        auto lsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Vertex);
        auto hsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::TessControl);

        if (hsEntryPoint) {
          if (lsEntryPoint)
            lgc::setShaderStage(lsEntryPoint, ShaderStage::TessControl);
          auto lsHsEntryPoint = shaderMerger.generateLsHsEntryPoint(lsEntryPoint, hsEntryPoint);
          lsHsEntryPoint->setCallingConv(CallingConv::AMDGPU_HS);
          lgc::setShaderStage(lsHsEntryPoint, ShaderStage::TessControl);
        }
      }

      if (enableNgg) {
        // If NGG is enabled, ES-GS merged shader should be present even if GS is absent
        auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::TessEval);

        if (esEntryPoint) {
          lgc::setShaderStage(esEntryPoint, ShaderStage::TessEval);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, nullptr, nullptr);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStage::TessEval);
        }
      }
    } else if (m_hasGs) {
      // GS-only pipeline
      auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Vertex);
      auto gsEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Geometry);

      if (enableNgg) {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStage::Geometry);
          auto copyShaderEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::CopyShader);
          if (copyShaderEntryPoint)
            lgc::setShaderStage(copyShaderEntryPoint, ShaderStage::Geometry);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, gsEntryPoint, copyShaderEntryPoint);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStage::Geometry);
        }
      } else {
        if (gsEntryPoint) {
          if (esEntryPoint)
            lgc::setShaderStage(esEntryPoint, ShaderStage::Geometry);
          auto esGsEntryPoint = shaderMerger.generateEsGsEntryPoint(esEntryPoint, gsEntryPoint);
          esGsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(esGsEntryPoint, ShaderStage::Geometry);
        }
      }
    } else if (m_hasVs) {
      // VS_FS pipeline
      if (enableNgg) {
        // If NGG is enabled, ES-GS merged shader should be present even if GS is absent
        auto esEntryPoint = m_pipelineShaders->getEntryPoint(ShaderStage::Vertex);
        if (esEntryPoint) {
          lgc::setShaderStage(esEntryPoint, ShaderStage::Vertex);
          auto primShaderEntryPoint = shaderMerger.buildPrimShader(esEntryPoint, nullptr, nullptr);
          primShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
          lgc::setShaderStage(primShaderEntryPoint, ShaderStage::Vertex);
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

  for (auto &func : module) {
    if (!func.empty()) {
      auto callingConv = func.getCallingConv();
      StringRef entryName = getEntryPointName(callingConv, false);
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
  RegisterMetadataBuilder regMetadataBuilder(&module, m_pipelineState, m_pipelineShaders);
  regMetadataBuilder.buildPalMetadata();
}

// =====================================================================================================================
// Handle the store of tessellation factors.
//
// @param entryPoint : Entry-point of tessellation control shader
void PatchPreparePipelineAbi::storeTessFactors(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStage::TessControl); // Must be tessellation control shader

  if (m_pipelineState->canOptimizeTessFactor())
    return; // If TF store is to be optimized, skip further processing

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
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::TessControl)->entryArgIdxs.tcs;
  const auto tfBufferBase = getFunctionArgument(entryPoint, entryArgIdxs.tfBufferBase);
  const auto relPatchId = pipelineSysValues.get(entryPoint)->getRelativeId();

  // Read back tessellation factors and write them to TF buffer
  auto tessFactors = readTessFactors(m_pipelineState, relPatchId, builder);
  writeTessFactors(m_pipelineState, tfBufferDesc, tfBufferBase, relPatchId, tessFactors.first, tessFactors.second,
                   builder);

  pipelineSysValues.clear();
}
