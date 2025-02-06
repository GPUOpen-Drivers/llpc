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
* @file  PreparePipelineAbi.cpp
* @brief LLPC source file: contains implementation of class lgc::PreparePipelineAbi.
***********************************************************************************************************************
*/
#include "lgc/lowering/PreparePipelineAbi.h"
#include "MeshTaskShader.h"
#include "RegisterMetadataBuilder.h"
#include "ShaderMerger.h"
#include "lgc/state/PalMetadata.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-prepare-pipeline-abi"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
PreparePipelineAbi::PreparePipelineAbi() {
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PreparePipelineAbi::run(Module &module, ModuleAnalysisManager &analysisManager) {
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

  LLVM_DEBUG(dbgs() << "Run the pass Prepare-Pipeline-Abi\n");

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
    storeTessFactorsAndHsOutputs(hsEntryPoint);

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
std::pair<Value *, Value *> PreparePipelineAbi::readTessFactors(PipelineState *pipelineState, Value *relPatchId,
                                                                IRBuilder<> &builder) {
  auto func = builder.GetInsertBlock()->getParent();
  auto lds = Patch::getLdsVariable(pipelineState, func);

  const auto &hwConfig = pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.hwConfig;

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

  assert(numOuterTfs >= 2 && numOuterTfs <= 4);
  // ldsOffset = tessFactorStart + relPatchId * tessFactorStride
  Value *ldsOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.onChip.tessFactorStride));
  ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(hwConfig.onChip.tessFactorStart));
  Value *outerTf = readValueFromLds(FixedVectorType::get(builder.getFloatTy(), numOuterTfs), ldsOffset);

  // NOTE: For isoline, the outer tessellation factors have to be exchanged, which is required by HW.
  if (primitiveMode == PrimitiveMode::Isolines) {
    assert(numOuterTfs == 2);
    outerTf = builder.CreateShuffleVector(outerTf, ArrayRef<int>{1, 0});
  }

  assert(numInnerTfs <= 2);
  Value *innerTf = nullptr;
  if (numInnerTfs > 0) {
    // ldsOffset = tessFactorStart + relPatchId * tessFactorStride + numOuterTfs
    Value *ldsOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.onChip.tessFactorStride));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(hwConfig.onChip.tessFactorStart));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(numOuterTfs));
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
void PreparePipelineAbi::writeTessFactors(PipelineState *pipelineState, Value *tfBufferDesc, Value *tfBufferBase,
                                          Value *relPatchId, Value *outerTf, Value *innerTf, BuilderBase &builder) {
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
  const auto numOuterTfs = cast<FixedVectorType>(outerTf->getType())->getNumElements();
  const auto numInnerTfs = innerTf ? cast<FixedVectorType>(innerTf->getType())->getNumElements()
                                   : 0; // Isoline doesn't have inner tessellation factors

  Value *tfBufferOffset = builder.CreateMul(relPatchId, builder.getInt32((numOuterTfs + numInnerTfs) * sizeof(float)));

  CoherentFlag coherent = {};
  if (pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    coherent.bits.glc = true;
  }

  auto primitiveMode = pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
  if (primitiveMode == PrimitiveMode::Isolines) {
    assert(numOuterTfs == 2 && numInnerTfs == 0);
    builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
                            {outerTf,                             // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(coherent.u32All)}); // glc

  } else if (primitiveMode == PrimitiveMode::Triangles) {
    assert(numOuterTfs == 3 && numInnerTfs == 1);

    // For triangle, we can combine outer tessellation factors with inner ones
    Value *tessFactor = builder.CreateShuffleVector(outerTf, ArrayRef<int>{0, 1, 2, 3});
    tessFactor =
        builder.CreateInsertElement(tessFactor, builder.CreateExtractElement(innerTf, static_cast<uint64_t>(0)), 3);

    builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
                            {tessFactor,                          // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(coherent.u32All)}); // glc
  } else {
    assert(primitiveMode == PrimitiveMode::Quads);
    assert(numOuterTfs == 4 && numInnerTfs == 2);

    builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
                            {outerTf,                             // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(coherent.u32All)}); // glc

    tfBufferOffset = builder.CreateAdd(tfBufferOffset, builder.getInt32(4 * sizeof(float)));
    builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_buffer_store,
                            {innerTf,                             // vdata
                             tfBufferDesc,                        // rsrc
                             tfBufferOffset,                      // voffset
                             tfBufferBase,                        // soffset
                             builder.getInt32(coherent.u32All)}); // glc
  }
}

// =====================================================================================================================
// Write HS outputs to off-chip LDS buffer.
//
// @param pipelineState : Pipeline state
// @param offChipLdsDesc : Off-chip LDS buffer descriptor
// @param offChipLdsBase : Off-chip LDS buffer base offset
// @param relPatchId : Relative patch ID (output patch ID in group)
// @param vertexIdx : Vertex indexing (output control point ID)
// @param outerTf : Outer tessellation factors to check (if any one is less than or equal to zero, discard the patch)
// @param builder : IR builder to insert instructions
void PreparePipelineAbi::writeHsOutputs(PipelineState *pipelineState, Value *offChipLdsDesc, Value *offChipLdsBase,
                                        Value *relPatchId, Value *vertexIdx, Value *outerTf, BuilderBase &builder) {
  IRBuilder<>::InsertPointGuard guard(builder);

  auto func = builder.GetInsertBlock()->getParent();
  auto lds = Patch::getLdsVariable(pipelineState, func);

  // Helper to read value from LDS
  auto readValueFromLds = [&](Type *readTy, Value *ldsOffset) {
    assert(readTy->getScalarSizeInBits() == 32); // Only accept 32-bit data

    Value *readPtr = builder.CreateGEP(builder.getInt32Ty(), lds, ldsOffset);
    readPtr = builder.CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
    return builder.CreateAlignedLoad(readTy, readPtr, Align(4));
  };

  //
  // Check if this patch could be discarded
  //
  Value *minOuterTf = builder.CreateExtractElement(outerTf, static_cast<uint64_t>(0));
  for (unsigned i = 1; i < cast<FixedVectorType>(outerTf->getType())->getNumElements(); ++i)
    minOuterTf = builder.CreateBinaryIntrinsic(Intrinsic::minnum, minOuterTf, builder.CreateExtractElement(outerTf, i));

  auto validPatch = builder.CreateFCmpOGT(minOuterTf, ConstantFP::get(builder.getFloatTy(), 0.0)); // minOuterTf > 0.0
  builder.CreateIf(validPatch, false, ".writeHsOutputs");

  //
  // Write HS outputs to off-chip LDS buffer if this patch is valid
  //
  auto &inOutUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage;
  const auto &builtInUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->builtInUsage.tcs;
  const auto &hwConfig = inOutUsage.tcs.hwConfig;

  // Check if we don't need to write this built-in to off-chip LDS buffer because it is only accessed by HS
  auto checkBuiltInNotToWrite = [&](unsigned builtIn) {
    if (pipelineState->getNextShaderStage(ShaderStage::TessControl) == ShaderStage::TessEval) {
      auto nextInOutStage = pipelineState->getShaderResourceUsage(ShaderStage::TessEval)->inOutUsage;
      if (builtIn == BuiltInTessLevelOuter || builtIn == BuiltInTessLevelInner) {
        if (inOutUsage.perPatchBuiltInOutputLocMap.count(builtIn) > 0 &&
            nextInOutStage.perPatchBuiltInInputLocMap.count(builtIn) == 0)
          return true;
      } else {
        if (inOutUsage.builtInOutputLocMap.count(builtIn) > 0 && nextInOutStage.builtInInputLocMap.count(builtIn) == 0)
          return true;
      }
    }
    return false;
  };

  static const unsigned BufferFormatsGfx10[] = {BUF_FORMAT_32_FLOAT, BUF_FORMAT_32_32_FLOAT_GFX10,
                                                BUF_FORMAT_32_32_32_FLOAT_GFX10, BUF_FORMAT_32_32_32_32_FLOAT_GFX10};
  static const unsigned BufferFormatsGfx11[] = {BUF_FORMAT_32_FLOAT, BUF_FORMAT_32_32_FLOAT_GFX11,
                                                BUF_FORMAT_32_32_32_FLOAT_GFX11, BUF_FORMAT_32_32_32_32_FLOAT_GFX11};

  const auto gfxIp = pipelineState->getTargetInfo().getGfxIpVersion();
  ArrayRef<unsigned> bufferFormats(gfxIp.major == 10 ? BufferFormatsGfx10 : BufferFormatsGfx11);
  CoherentFlag coherent = {};
  if (gfxIp.major <= 11) {
    coherent.bits.glc = true;
  }

  // Write per-vertex HS outputs to off-chip LDS buffer
  if (inOutUsage.outputMapLocCount > 0) {
    SmallDenseSet<unsigned> builtInLocsNotToWrite;
    SmallDenseMap<unsigned, Type *> builtInLocsToTypes;

    for (const auto &[builtIn, loc] : inOutUsage.builtInOutputLocMap) {
      if (checkBuiltInNotToWrite(builtIn)) {
        assert(inOutUsage.builtInOutputLocMap.count(builtIn) > 0);
        builtInLocsNotToWrite.insert(inOutUsage.builtInOutputLocMap[builtIn]);
      } else {
        switch (builtIn) {
        case BuiltInPosition:
          builtInLocsToTypes[loc] = FixedVectorType::get(builder.getFloatTy(), 4);
          break;
        case BuiltInPointSize:
          builtInLocsToTypes[loc] = builder.getFloatTy();
          break;
        case BuiltInClipDistance:
        case BuiltInCullDistance: {
          const unsigned clipOrCullDistance =
              builtIn == BuiltInClipDistance ? builtInUsage.clipDistance : builtInUsage.cullDistance;
          assert(clipOrCullDistance > 0 && clipOrCullDistance <= 8);

          builtInLocsToTypes[loc] = clipOrCullDistance == 1
                                        ? builder.getFloatTy()
                                        : FixedVectorType::get(builder.getFloatTy(), std::min(clipOrCullDistance, 4U));
          if (clipOrCullDistance > 4) {
            builtInLocsToTypes[loc + 1] = clipOrCullDistance == 5
                                              ? builder.getFloatTy()
                                              : FixedVectorType::get(builder.getFloatTy(), clipOrCullDistance - 4);
          }

          break;
        }
        case BuiltInViewportIndex:
        case BuiltInLayer:
          builtInLocsToTypes[loc] = builder.getInt32Ty();
          break;
        default:
          llvm_unreachable("Unexpected built-in");
          break;
        }
      }
    }

    // baseOffset = outputPatchStart + (relPatchId * outputVertexCount + vertexIdx) * outputVertexStride +
    //            = outputPatchStart + relPatchId * outputPatchSize + vertexIdx * outputVertexStride
    auto onChipLdsBaseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.onChip.outputPatchSize));
    onChipLdsBaseOffset = builder.CreateAdd(
        onChipLdsBaseOffset, builder.CreateMul(vertexIdx, builder.getInt32(hwConfig.onChip.outputVertexStride)));
    onChipLdsBaseOffset = builder.CreateAdd(onChipLdsBaseOffset, builder.getInt32(hwConfig.onChip.outputPatchStart));

    auto offChipLdsBaseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.offChip.outputPatchSize));
    offChipLdsBaseOffset = builder.CreateAdd(
        offChipLdsBaseOffset, builder.CreateMul(vertexIdx, builder.getInt32(hwConfig.offChip.outputVertexStride)));
    offChipLdsBaseOffset = builder.CreateAdd(offChipLdsBaseOffset, builder.getInt32(hwConfig.offChip.outputPatchStart));

    for (unsigned loc = 0; loc < inOutUsage.outputMapLocCount; ++loc) {
      if (builtInLocsNotToWrite.count(loc) > 0)
        continue;

      Type *outputTy = FixedVectorType::get(builder.getInt32Ty(), 4); // <4 x i32> for generic outputs
      if (builtInLocsToTypes.count(loc) > 0)
        outputTy = builtInLocsToTypes[loc]; // Built-in outputs have known types

      const unsigned numComponents = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;

      // ldsOffset = baseOffset + attribOffset
      auto attribOffset = builder.getInt32(4 * loc);
      auto onChipLdsOffset = builder.CreateAdd(onChipLdsBaseOffset, attribOffset);
      auto output = readValueFromLds(outputTy, onChipLdsOffset);

      auto offChipLdsOffset = builder.CreateAdd(offChipLdsBaseOffset, attribOffset);
      offChipLdsOffset = builder.CreateMul(offChipLdsOffset, builder.getInt32(4)); // Convert to byte offset

      builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store,
                              {output,                                             // vdata
                               offChipLdsDesc,                                     // rsrc
                               offChipLdsOffset,                                   // voffset
                               offChipLdsBase,                                     // soffset
                               builder.getInt32(bufferFormats[numComponents - 1]), // format
                               builder.getInt32(coherent.u32All)});                // glc
    }
  }

  // Write per-patch HS outputs to off-chip LDS buffer
  if (inOutUsage.perPatchOutputMapLocCount > 0) {
    SmallDenseSet<unsigned> builtInLocsNotToWrite;
    SmallDenseMap<unsigned, Type *> builtInLocsToTypes;

    for (const auto &[builtIn, loc] : inOutUsage.perPatchBuiltInOutputLocMap) {
      if (checkBuiltInNotToWrite(builtIn)) {
        assert(inOutUsage.perPatchBuiltInOutputLocMap.count(builtIn) > 0);
        builtInLocsNotToWrite.insert(inOutUsage.perPatchBuiltInOutputLocMap[builtIn]);
      } else {
        Type *type = nullptr;
        switch (builtIn) {
        case BuiltInTessLevelOuter:
          type = FixedVectorType::get(builder.getFloatTy(), 4);
          break;
        case BuiltInTessLevelInner:
          type = FixedVectorType::get(builder.getFloatTy(), 2);
          break;
        default:
          llvm_unreachable("Unexpected built-in");
          break;
        }
        builtInLocsToTypes[loc] = type;
      }
    }

    // baseOffset = patchConstStart + relPatchId * patchConstSize
    auto onChipLdsBaseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.onChip.patchConstSize));
    onChipLdsBaseOffset = builder.CreateAdd(onChipLdsBaseOffset, builder.getInt32(hwConfig.onChip.patchConstStart));

    auto offChipLdsBaseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.offChip.patchConstSize));
    offChipLdsBaseOffset = builder.CreateAdd(offChipLdsBaseOffset, builder.getInt32(hwConfig.offChip.patchConstStart));

    for (unsigned loc = 0; loc < inOutUsage.perPatchOutputMapLocCount; ++loc) {
      if (builtInLocsNotToWrite.count(loc) > 0)
        continue;

      Type *outputTy = FixedVectorType::get(builder.getInt32Ty(), 4); // <4 x i32> for generic outputs
      if (builtInLocsToTypes.count(loc) > 0)
        outputTy = builtInLocsToTypes[loc]; // Built-in outputs have known types

      const unsigned numComponents = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;

      // ldsOffset = baseOffset + attribOffset
      auto attribOffset = builder.getInt32(4 * loc);
      auto onChipLdsOffset = builder.CreateAdd(onChipLdsBaseOffset, attribOffset);
      auto output = readValueFromLds(outputTy, onChipLdsOffset);

      auto offChipLdsOffset = builder.CreateAdd(offChipLdsBaseOffset, attribOffset);
      offChipLdsOffset = builder.CreateMul(offChipLdsOffset, builder.getInt32(4)); // Convert to byte offset

      builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store,
                              {output,                                             // vdata
                               offChipLdsDesc,                                     // rsrc
                               offChipLdsOffset,                                   // voffset
                               offChipLdsBase,                                     // soffset
                               builder.getInt32(bufferFormats[numComponents - 1]), // format
                               builder.getInt32(coherent.u32All)});                // glc
    }
  }
}

// =====================================================================================================================
// Merge shaders and set calling convention for the entry-point of each shader (GFX9+)
//
// @param module : LLVM module
void PreparePipelineAbi::mergeShader(Module &module) {
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
void PreparePipelineAbi::setAbiEntryNames(Module &module) {

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
void PreparePipelineAbi::addAbiMetadata(Module &module) {
  RegisterMetadataBuilder regMetadataBuilder(&module, m_pipelineState, m_pipelineShaders);
  regMetadataBuilder.buildPalMetadata();
}

// =====================================================================================================================
// Handle the store of tessellation factors (TFs) and the store of HS outputs to off-chip LDS buffer if the patch is
// valid (all of its outer TFs are greater than zero).
//
// @param entryPoint : Entry-point of tessellation control shader
void PreparePipelineAbi::storeTessFactorsAndHsOutputs(Function *entryPoint) {
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

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(retInst);

  PipelineSystemValues pipelineSysValues;
  pipelineSysValues.initialize(m_pipelineState);

  const auto tfBufferDesc = pipelineSysValues.get(entryPoint)->getTessFactorBufDesc();
  const auto offChipLdsDesc = pipelineSysValues.get(entryPoint)->getOffChipLdsDesc();
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::TessControl)->entryArgIdxs.tcs;
  const auto tfBufferBase = getFunctionArgument(entryPoint, entryArgIdxs.tfBufferBase);
  const auto offChipLdsBase = getFunctionArgument(entryPoint, entryArgIdxs.offChipLdsBase);
  const auto relPatchId = pipelineSysValues.get(entryPoint)->getRelativeId();
  const auto vertexIdx = pipelineSysValues.get(entryPoint)->getInvocationId();

  // Read back tessellation factors and write them to TF buffer
  const auto &[outerTf, innerTf] = readTessFactors(m_pipelineState, relPatchId, builder);
  writeTessFactors(m_pipelineState, tfBufferDesc, tfBufferBase, relPatchId, outerTf, innerTf, builder);

  // Write HS outputs to off-chip LDS buffer
  writeHsOutputs(m_pipelineState, offChipLdsDesc, offChipLdsBase, relPatchId, vertexIdx, outerTf, builder);

  pipelineSysValues.clear();
}
