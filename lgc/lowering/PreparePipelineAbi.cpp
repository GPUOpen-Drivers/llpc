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
#include "lgc/Debug.h"
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

  LgcLowering::init(&module);

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
  auto lds = LgcLowering::getLdsVariable(pipelineState, func);

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
  } else {
    coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_SYS;
    coherent.gfx12.th = pipelineState->getTemporalHint(TH::TH_WB, TemporalHintTessFactorWrite);
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
  auto lds = LgcLowering::getLdsVariable(pipelineState, func);

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

  // NOTE: Here, we dynamically set FP32 denorm mode to allow inout denorms. This is because TFs with denorm values
  // will be flushed to zeros during this check if FP32 denorm mode is not set to allow denorms via FLOAT_MODE
  // register field.
  //
  // MODE[7:4] = FP_DENORM, [5:4] = Single precision denorm mode, [7:6]= Double precision and FP16 denormal mode
  // Mode:
  //   0 = flush input and output denorms
  //   1 = allow input denorms, flush output denorms
  //   2 = flush input denorms, allow output denorms
  //   3 = allow input and output denorms
  static const unsigned AllowInOutDenorms = 3;
  builder.CreateSetReg(WaveStateReg::MODE, 4, 4, builder.getInt32(AllowInOutDenorms));

  Value *minOuterTf = builder.CreateExtractElement(outerTf, static_cast<uint64_t>(0));
  for (unsigned i = 1; i < cast<FixedVectorType>(outerTf->getType())->getNumElements(); ++i)
    minOuterTf = builder.CreateBinaryIntrinsic(Intrinsic::minnum, minOuterTf, builder.CreateExtractElement(outerTf, i));

  auto validPatch = builder.CreateFCmpOGT(minOuterTf, ConstantFP::get(builder.getFloatTy(), 0.0)); // minOuterTf > 0.0
  builder.CreateIf(validPatch, false, ".writeHsOutputs");

  //
  // Write HS outputs to off-chip LDS buffer if this patch is valid
  //
  auto &inOutUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage;
  const auto &nextInOutUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessEval)->inOutUsage;

  const auto &builtInUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->builtInUsage.tcs;
  const auto &nextBuiltInUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessEval)->builtInUsage.tes;

  const auto &hwConfig = inOutUsage.tcs.hwConfig;
  const bool hasTes = pipelineState->hasShaderStage(ShaderStage::TessEval);

  const auto gfxIp = pipelineState->getTargetInfo().getGfxIpVersion();
  const unsigned bufferFormat =
      gfxIp.major >= 11 ? BUF_FORMAT_32_32_32_32_FLOAT_GFX11 : BUF_FORMAT_32_32_32_32_FLOAT_GFX10;
  CoherentFlag coherent = {};
  if (gfxIp.major <= 11) {
    coherent.bits.glc = true;
  } else {
    coherent.gfx12.th = TH::TH_WB;
    coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
  }

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC HS output write info\n\n");

  // HS output write info (
  struct HsOutputWriteInfo {
    unsigned onChipLoc; // Location in on-chip LDS
    unsigned builtIn;   // Whether for a built-in
  };

  // Write per-vertex HS outputs to off-chip LDS buffer (to next stage)
  unsigned offChipLocCount = hasTes ? nextInOutUsage.inputMapLocCount : inOutUsage.outputMapLocCount;
  if (offChipLocCount > 0) {
    LLPC_OUTS("Per-vertex Outputs [OnChip, OffChip]:\n");

    SmallDenseMap<unsigned, HsOutputWriteInfo> hsOutputWrites;

    // Check generic outputs
    const auto &genericOffChipLocMap = hasTes ? nextInOutUsage.inputLocInfoMap : inOutUsage.outputLocInfoMap;
    auto &genericOnChipLocMap = inOutUsage.outputLocInfoMap;

    for (const auto &[origLocInfo, offChipLocInfo] : genericOffChipLocMap) {
      const unsigned offChipLoc = offChipLocInfo.getLocation();
      if (hsOutputWrites.count(offChipLoc) == 0) {
        assert(genericOnChipLocMap.count(origLocInfo) > 0);
        hsOutputWrites[offChipLoc].onChipLoc = genericOnChipLocMap[origLocInfo].getLocation();
        hsOutputWrites[offChipLoc].builtIn = InvalidValue;
      }
    }

    // Check built-in outputs
    const auto &builtInOffChipLocMap = hasTes ? nextInOutUsage.builtInInputLocMap : inOutUsage.builtInOutputLocMap;
    auto &builtInOnChipLocMap = inOutUsage.builtInOutputLocMap;

    for (const auto &[builtIn, offChipLoc] : builtInOffChipLocMap) {
      assert(builtInOnChipLocMap.count(builtIn) > 0);
      hsOutputWrites[offChipLoc].onChipLoc = builtInOnChipLocMap[builtIn];
      hsOutputWrites[offChipLoc].builtIn = builtIn;

      if (builtIn == BuiltInClipDistance || builtIn == BuiltInCullDistance) {
        unsigned clipOrCullDistance = 0;
        if (hasTes) {
          clipOrCullDistance =
              builtIn == BuiltInClipDistance ? nextBuiltInUsage.clipDistanceIn : nextBuiltInUsage.cullDistanceIn;
        } else {
          clipOrCullDistance = builtIn == BuiltInClipDistance ? builtInUsage.clipDistance : builtInUsage.cullDistance;
        }
        assert(clipOrCullDistance > 0 && clipOrCullDistance <= 8);

        if (clipOrCullDistance > 4) {
          hsOutputWrites[offChipLoc + 1].onChipLoc = builtInOnChipLocMap[builtIn] + 1;
          hsOutputWrites[offChipLoc + 1].builtIn = builtIn;
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

    for (unsigned offChipLoc = 0; offChipLoc < offChipLocCount; ++offChipLoc) {
      if (hsOutputWrites.count(offChipLoc) == 0)
        continue; // Skip the location if it is not recorded (unlinked pipeline)

      const unsigned onChipLoc = hsOutputWrites[offChipLoc].onChipLoc;
      const unsigned builtIn = hsOutputWrites[offChipLoc].builtIn;

      LLPC_OUTS("location = [" << onChipLoc << ", " << offChipLoc << "]");
      if (builtIn != InvalidValue) {
        LLPC_OUTS(" (builtin = " << PipelineState::getBuiltInName(static_cast<BuiltInKind>(builtIn)) << ")");
      }
      LLPC_OUTS("\n");

      // ldsOffset = baseOffset + attribOffset
      auto attribOffset = builder.getInt32(4 * onChipLoc);
      auto onChipLdsOffset = builder.CreateAdd(onChipLdsBaseOffset, attribOffset);
      auto output = readValueFromLds(FixedVectorType::get(builder.getInt32Ty(), 4), onChipLdsOffset);

      attribOffset = builder.getInt32(4 * offChipLoc);
      auto offChipLdsOffset = builder.CreateAdd(offChipLdsBaseOffset, attribOffset);
      offChipLdsOffset = builder.CreateMul(offChipLdsOffset, builder.getInt32(4)); // Convert to byte offset

      builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store,
                              {output,                              // vdata
                               offChipLdsDesc,                      // rsrc
                               offChipLdsOffset,                    // voffset
                               offChipLdsBase,                      // soffset
                               builder.getInt32(bufferFormat),      // format
                               builder.getInt32(coherent.u32All)}); // glc
    }

    LLPC_OUTS("\n");
  }

  // Write per-patch HS outputs to off-chip LDS buffer (to next stage)
  offChipLocCount = hasTes ? nextInOutUsage.perPatchInputMapLocCount : inOutUsage.perPatchOutputMapLocCount;
  if (offChipLocCount > 0) {
    LLPC_OUTS("Per-patch Outputs [OnChip, OffChip]:\n");

    SmallDenseMap<unsigned, HsOutputWriteInfo> hsOutputWrites;

    // Check generic outputs
    const auto &genericOffChipLocMap = hasTes ? nextInOutUsage.perPatchInputLocMap : inOutUsage.perPatchOutputLocMap;
    auto &genericOnChipLocMap = inOutUsage.perPatchOutputLocMap;

    for (const auto &[origLoc, offChipLoc] : genericOffChipLocMap) {
      if (hsOutputWrites.count(offChipLoc) == 0) {
        assert(genericOnChipLocMap.count(origLoc) > 0);
        hsOutputWrites[offChipLoc].onChipLoc = genericOnChipLocMap[origLoc];
        hsOutputWrites[offChipLoc].builtIn = InvalidValue;
      }
    }

    // Check built-in outputs
    const auto &builtInOffChipLocMap =
        hasTes ? nextInOutUsage.perPatchBuiltInInputLocMap : inOutUsage.perPatchBuiltInOutputLocMap;
    auto &builtInOnChipLocMap = inOutUsage.perPatchBuiltInOutputLocMap;

    for (const auto &[builtIn, offChipLoc] : builtInOffChipLocMap) {
      assert(builtInOnChipLocMap.count(builtIn) > 0);
      hsOutputWrites[offChipLoc].onChipLoc = builtInOnChipLocMap[builtIn];
      hsOutputWrites[offChipLoc].builtIn = builtIn;
    }

    // baseOffset = patchConstStart + relPatchId * patchConstSize
    auto onChipLdsBaseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.onChip.patchConstSize));
    onChipLdsBaseOffset = builder.CreateAdd(onChipLdsBaseOffset, builder.getInt32(hwConfig.onChip.patchConstStart));

    auto offChipLdsBaseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.offChip.patchConstSize));
    offChipLdsBaseOffset = builder.CreateAdd(offChipLdsBaseOffset, builder.getInt32(hwConfig.offChip.patchConstStart));

    for (unsigned offChipLoc = 0; offChipLoc < offChipLocCount; ++offChipLoc) {
      if (hsOutputWrites.count(offChipLoc) == 0)
        continue; // Skip the location if it is not recorded (unlinked pipeline)

      const unsigned onChipLoc = hsOutputWrites[offChipLoc].onChipLoc;
      const unsigned builtIn = hsOutputWrites[offChipLoc].builtIn;

      LLPC_OUTS("location = [" << onChipLoc << ", " << offChipLoc << "]");
      if (builtIn != InvalidValue) {
        LLPC_OUTS(" (builtin = " << PipelineState::getBuiltInName(static_cast<BuiltInKind>(builtIn)) << ")");
      }
      LLPC_OUTS("\n");

      // ldsOffset = baseOffset + attribOffset
      auto attribOffset = builder.getInt32(4 * onChipLoc);
      auto onChipLdsOffset = builder.CreateAdd(onChipLdsBaseOffset, attribOffset);
      auto output = readValueFromLds(FixedVectorType::get(builder.getInt32Ty(), 4), onChipLdsOffset);

      attribOffset = builder.getInt32(4 * offChipLoc);
      auto offChipLdsOffset = builder.CreateAdd(offChipLdsBaseOffset, attribOffset);
      offChipLdsOffset = builder.CreateMul(offChipLdsOffset, builder.getInt32(4)); // Convert to byte offset

      builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store,
                              {output,                              // vdata
                               offChipLdsDesc,                      // rsrc
                               offChipLdsOffset,                    // voffset
                               offChipLdsBase,                      // soffset
                               builder.getInt32(bufferFormat),      // format
                               builder.getInt32(coherent.u32All)}); // glc
    }

    LLPC_OUTS("\n");
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
