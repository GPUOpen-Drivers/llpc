﻿/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerInOut.cpp
 * @brief LLPC source file: contains implementation of class lgc::LowerInOut.
 *
 ***********************************************************************************************************************
 */
#include "lgc/lowering/LowerInOut.h"
#include "lgc/Builder.h"
#include "lgc/BuiltIns.h"
#include "lgc/Debug.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/util/WorkgroupLayout.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>

#define DEBUG_TYPE "lgc-lower-in-out"

using namespace llvm;
using namespace lgc;

namespace lgc {

// Preferred number of HS threads per subgroup.
constexpr unsigned MaxHsThreadsPerSubgroup = 256;

// =====================================================================================================================
LowerInOut::LowerInOut() {
  memset(&m_gfxIp, 0, sizeof(m_gfxIp));
  initPerShader();
}

// =====================================================================================================================
// Initialize per-shader members
void LowerInOut::initPerShader() {
  m_clipDistance = nullptr;
  m_cullDistance = nullptr;
  m_primitiveId = nullptr;
  m_fragDepth = nullptr;
  m_fragStencilRef = nullptr;
  m_sampleMask = nullptr;
  m_viewportIndex = nullptr;
  m_layer = nullptr;
  m_viewIndex = nullptr;
  m_threadId = nullptr;
  m_edgeFlag = nullptr;

  m_attribExports.clear();
}

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerInOut::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  auto getPostDominatorTree = [&](Function &f) -> PostDominatorTree & {
    auto &fam = analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
    return fam.getResult<PostDominatorTreeAnalysis>(f);
  };

  LLVM_DEBUG(dbgs() << "Run the pass Lower-In-Out\n");

  LgcLowering::init(&module);

  m_pipelineState = pipelineState;
  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  m_pipelineSysValues.initialize(m_pipelineState);

  auto entryPoint = pipelineShaders.getEntryPoint(ShaderStage::Fragment);
  if (entryPoint) {
    m_entryPoint = entryPoint;
    m_shaderStage = ShaderStage::Fragment;
    const auto fetchVisitor = llvm_dialects::VisitorBuilder<LowerInOut>()
                                  .add(&LowerInOut::visitEvalIjOffsetSmoothOp)
                                  .add(&LowerInOut::visitAdjustIjOp)
                                  .build();
    fetchVisitor.visit(*this, module);
  }

  const auto stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = stageMask.contains_any({ShaderStage::TessControl, ShaderStage::TessEval});
  m_hasGs = stageMask.contains(ShaderStage::Geometry);

  SmallVector<Function *, 16> inputCallees, otherCallees;
  for (auto &func : module.functions()) {
    auto name = func.getName();
    if (name.starts_with("lgc.input"))
      inputCallees.push_back(&func);
    else if (name.starts_with("lgc.output") || name.starts_with("lgc.gs") || name == "lgc.write.xfb.output")
      otherCallees.push_back(&func);
  }

  // Set buffer formats based on specific GFX
  static const std::array<unsigned char, 4> BufferFormatsGfx10 = {
      BUF_FORMAT_32_FLOAT,
      BUF_FORMAT_32_32_FLOAT_GFX10,
      BUF_FORMAT_32_32_32_FLOAT_GFX10,
      BUF_FORMAT_32_32_32_32_FLOAT_GFX10,
  };
  static const std::array<unsigned char, 4> BufferFormatsGfx11 = {
      BUF_FORMAT_32_FLOAT,
      BUF_FORMAT_32_32_FLOAT_GFX11,
      BUF_FORMAT_32_32_32_FLOAT_GFX11,
      BUF_FORMAT_32_32_32_32_FLOAT_GFX11,
  };

  switch (m_gfxIp.major) {
  case 10:
    m_buffFormats = &BufferFormatsGfx10;
    break;
  case 11:
  case 12:
    m_buffFormats = &BufferFormatsGfx11;
    break;
  default:
    llvm_unreachable("unsupported GFX IP");
    break;
  }

  // Process each shader in turn, in reverse order (because for example VS uses inOutUsage.tcs.calcFactor
  // set by TCS).
  for (auto stage : llvm::reverse(ShaderStagesNativeCopy)) {
    auto entryPoint = pipelineShaders.getEntryPoint(stage);
    if (entryPoint) {
      processFunction(*entryPoint, stage, inputCallees, otherCallees, getPostDominatorTree);
    }
  }

  // Process non-entry-point shaders
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;
    auto shaderStage = getShaderStage(&func);
    if (!shaderStage || &func == pipelineShaders.getEntryPoint(shaderStage.value()))
      continue;
    processFunction(func, shaderStage.value(), inputCallees, otherCallees, getPostDominatorTree);
  }

  for (auto callInst : m_importCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_importCalls.clear();

  for (auto callInst : m_exportCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_exportCalls.clear();

  for (auto callInst : m_gsMsgCalls) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
  }
  m_gsMsgCalls.clear();

  m_pipelineSysValues.clear();

  return PreservedAnalyses::none();
}

void LowerInOut::processFunction(Function &func, ShaderStageEnum shaderStage, SmallVectorImpl<Function *> &inputCallees,
                                 SmallVectorImpl<Function *> &otherCallees,
                                 const std::function<PostDominatorTree &(Function &)> &getPostDominatorTree) {
  PostDominatorTree &postDomTree = getPostDominatorTree(func);

  initPerShader();
  m_entryPoint = &func;
  m_shaderStage = shaderStage;
  processShader();

  // We process input first, because we cache lots of arguments to output during visit for later processing.
  // It will be a disaster if we visit output intrinsics first, and the cached value for output was invalidated
  // after we process input intrinsics (consider a value read from input was exported to output).
  visitCallInsts(inputCallees);
  visitCallInsts(otherCallees);
  visitReturnInsts();

  markExportDone(m_entryPoint, postDomTree);
}

// =====================================================================================================================
// Mark the 'done' flag to the very last position export instruction.
//
// @param [in/out] func : LLVM function to be run on
// @param postDomTree : The PostDominatorTree of the \p func
void LowerInOut::markExportDone(Function *func, PostDominatorTree &postDomTree) {
  // Position export in NGG primitive shader is handled later on. Here we only process position export in legacy HW VS.
  if (m_pipelineState->getNggControl()->enableNgg)
    return;

  SmallVector<CallInst *, 4> expInsts;

  Function *expDecl = m_module->getFunction("llvm.amdgcn.exp.f32");
  if (!expDecl)
    return;

  // Get the export call instructions
  for (auto user : expDecl->users()) {
    if (CallInst *callInst = dyn_cast<CallInst>(user)) {
      if (callInst->getFunction() == func) {
        if (ConstantInt *target = dyn_cast<ConstantInt>(callInst->getOperand(0))) {
          uint64_t targetValue = target->getZExtValue();
          if (targetValue >= EXP_TARGET_POS_0 && targetValue <= EXP_TARGET_POS_3)
            expInsts.push_back(callInst);
        }
      }
    }
  }

  if (expInsts.empty())
    return;

  CallInst *lastExport = expInsts[0];

  // Here we are trying to find the position-export that post-dominates all the other position exports (i.e. the last
  // export). And apply the 'done' flag to that position-export. Although in practice user can easily write a program
  // that put the gl_Position output inside a if-else, in which case it is hard for us to find the last export. But we
  // already handled such situation in previous pass to put the real position export call into the last return block. So
  // it would be safe for us to do like this. The reason I didn't do a simple backward traverse in return block to find
  // the very last export is because the copy-shader, in which case the position export is not in the return block.
  for (unsigned i = 1; i < expInsts.size(); i++) {
    if (postDomTree.dominates(expInsts[i], lastExport))
      lastExport = expInsts[i];
    else
      assert(postDomTree.dominates(lastExport, expInsts[i]));
  }
  lastExport->setOperand(6, ConstantInt::getTrue(*m_context));
}

// =====================================================================================================================
// Process a single shader
void LowerInOut::processShader() {
  // Initialize the output value for gl_PrimitiveID
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage.value())->builtInUsage;
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs;
  if (m_shaderStage == ShaderStage::Vertex) {
    if (builtInUsage.vs.primitiveId)
      m_primitiveId = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.primitiveId);
  } else if (m_shaderStage == ShaderStage::TessEval) {
    if (builtInUsage.tes.primitiveId) {
      m_primitiveId = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.patchId);
    }
  }

  // Thread ID will be used in on-chip GS offset calculation (ES -> GS ring is always on-chip on GFX10+)
  bool useThreadId = m_hasGs;

  // Thread ID will also be used for stream-out buffer export
  const bool enableXfb = m_pipelineState->enableXfb();
  useThreadId = useThreadId || enableXfb;

  if (useThreadId) {
    // Calculate and store thread ID
    BuilderBase builder(*m_context);
    builder.SetInsertPointPastAllocas(m_entryPoint);
    m_threadId = getSubgroupLocalInvocationId(builder);
  }

  // Initialize HW configurations for tessellation shaders
  if (m_shaderStage == ShaderStage::TessControl || m_shaderStage == ShaderStage::TessEval) {
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStage::TessControl);
    const bool hasTes = m_pipelineState->hasShaderStage(ShaderStage::TessEval);

    auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.hwConfig;
    if (!hwConfig.initialized) {
      hwConfig.initialized = true;

      //
      // NOTE: The LDS for tessellation is as follow:
      //
      //          +----------------+------------------+--------------+-------------+-------------+-------------+
      // On-chip  | HS Patch Count | Special TF Value | Output Patch | Patch Const | Tess Factor | Input Patch | (LDS)
      //          +----------------+------------------+--------------+-------------+-------------+-------------+
      //
      //          +--------------+-------------+
      // Off-chip | Output Patch | Patch Const | (LDS Buffer)
      //          +--------------+-------------+
      //
      // inputPatchTotalSize = inputVertexCount * inputVertexStride * maxNumHsPatchesPerGroup
      // outputPatchTotalSize = outputVertexCount * outputVertexStride * maxNumHsPatchesPerGroup
      // patchConstTotalSize = patchConstCount * 4 * maxNumHsPatchesPerGroup
      // tessFactorTotalSize = 6 * maxNumHsPatchesPerGroup
      //
      const auto &tcsInOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage;
      const auto &tesInOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessEval)->inOutUsage;

      const unsigned inputLocCount = std::max(tcsInOutUsage.inputMapLocCount, 1U);
      const unsigned onChipOutputLocCount = std::max(tcsInOutUsage.outputMapLocCount, 1U);
      const unsigned offChipOutputLocCount =
          std::max(hasTes ? tesInOutUsage.inputMapLocCount : tcsInOutUsage.outputMapLocCount, 1U);

      const unsigned inputVertexCount = m_pipelineState->getNumPatchControlPoints();
      const unsigned outputVertexCount =
          hasTcs ? m_pipelineState->getShaderModes()->getTessellationMode().outputVertices : MaxTessPatchVertices;

      unsigned tessFactorCount = 0;
      switch (m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode) {
      case PrimitiveMode::Triangles:
        tessFactorCount = 4;
        break;
      case PrimitiveMode::Quads:
        tessFactorCount = 6;
        break;
      case PrimitiveMode::Isolines:
        tessFactorCount = 2;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
      // Use odd-dword stride to avoid LDS bank conflict
      assert(tessFactorCount % 2 == 0);
      hwConfig.onChip.tessFactorStride = tessFactorCount + 1;

      // Use odd-dword stride to avoid LDS bank conflict
      hwConfig.onChip.inputVertexStride = (inputLocCount * 4) | 1;
      hwConfig.onChip.inputPatchSize = inputVertexCount * hwConfig.onChip.inputVertexStride;

      hwConfig.onChip.outputVertexStride = (onChipOutputLocCount * 4) | 1;
      hwConfig.onChip.outputPatchSize = outputVertexCount * hwConfig.onChip.outputVertexStride;

      hwConfig.offChip.outputVertexStride = offChipOutputLocCount * 4;
      hwConfig.offChip.outputPatchSize = outputVertexCount * hwConfig.offChip.outputVertexStride;

      const unsigned onChipPatchConstCount = tcsInOutUsage.perPatchOutputMapLocCount;
      const unsigned offChipPatchConstCount =
          hasTes ? tesInOutUsage.perPatchInputMapLocCount : tcsInOutUsage.perPatchOutputMapLocCount;
      // Use odd-dword stride to avoid LDS bank conflict
      hwConfig.onChip.patchConstSize = 0;
      if (onChipPatchConstCount > 0)
        hwConfig.onChip.patchConstSize = (onChipPatchConstCount * 4) | 1;

      hwConfig.offChip.patchConstSize = 0;
      if (offChipPatchConstCount > 0)
        hwConfig.offChip.patchConstSize = offChipPatchConstCount * 4;

      const unsigned ldsSizePerPatch = hwConfig.onChip.outputPatchSize + hwConfig.onChip.patchConstSize +
                                       hwConfig.onChip.tessFactorStride + hwConfig.onChip.inputPatchSize;
      const unsigned ldsBufferSizePerPatch = hwConfig.offChip.outputPatchSize + hwConfig.offChip.patchConstSize;
      hwConfig.maxNumPatchesPerGroup = calcMaxNumPatchesPerGroup(inputVertexCount, outputVertexCount, tessFactorCount,
                                                                 ldsSizePerPatch, ldsBufferSizePerPatch);

      const unsigned onChipOutputPatchTotalSize = hwConfig.maxNumPatchesPerGroup * hwConfig.onChip.outputPatchSize;
      const unsigned offChipOutputPatchTotalSize = hwConfig.maxNumPatchesPerGroup * hwConfig.offChip.outputPatchSize;

      const unsigned onChipPatchConstTotalSize = hwConfig.maxNumPatchesPerGroup * hwConfig.onChip.patchConstSize;
      const unsigned offChipPatchConstTotalSize = hwConfig.maxNumPatchesPerGroup * hwConfig.offChip.patchConstSize;

      const unsigned inputPatchTotalSize = hwConfig.maxNumPatchesPerGroup * hwConfig.onChip.inputPatchSize;
      const unsigned tessFactorTotalSize = hwConfig.maxNumPatchesPerGroup * hwConfig.onChip.tessFactorStride;

      // NOTE: Tess factors and TCS outputs are always stored to on-chip LDS first. Then, they are store to TF buffer
      // and off-chip LDS buffer (which will be loaded by TES).
      hwConfig.offChip.outputPatchStart = 0;
      hwConfig.offChip.patchConstStart = hwConfig.offChip.outputPatchStart + offChipOutputPatchTotalSize;

      if (m_pipelineState->canOptimizeTessFactor()) {
        //
        // NOTE: If we are going to optimize TF store, we need additional on-chip LDS size. The required size is
        // 2 dwords per HS wave (1 dword all-ones flag and 1 dword all-zeros flag) plus an extra dword to count
        // actual HS patches. The layout is as follow:
        //
        // +----------------+--------+--------+-----+--------+--------+
        // | HS Patch Count | All 1s | All 0s | ... | All 1s | All 0s |
        // +----------------+--------+--------+-----+--------+--------+
        //                  |<---- Wave 0 --->|     |<---- Wave N --->|
        //
        assert(m_gfxIp.major >= 11);
        hwConfig.onChip.hsPatchCountStart = 0; // One dword to store actual HS patch count
        hwConfig.onChip.specialTfValueStart = hwConfig.onChip.hsPatchCountStart + 1;

        const unsigned maxNumHsWaves =
            MaxHsThreadsPerSubgroup / m_pipelineState->getShaderWaveSize(ShaderStage::TessControl);
        hwConfig.onChip.specialTfValueSize = maxNumHsWaves * 2;
      }

      hwConfig.onChip.outputPatchStart = hwConfig.onChip.specialTfValueStart + hwConfig.onChip.specialTfValueSize;
      hwConfig.onChip.patchConstStart = hwConfig.onChip.outputPatchStart + onChipOutputPatchTotalSize;
      hwConfig.onChip.tessFactorStart = hwConfig.onChip.patchConstStart + onChipPatchConstTotalSize;
      hwConfig.onChip.inputPatchStart = hwConfig.onChip.tessFactorStart + tessFactorTotalSize;

      hwConfig.tessOnChipLdsSize = hwConfig.onChip.inputPatchStart + inputPatchTotalSize;

      // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
      // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
      // restriction by providing the capability of querying thread ID in group rather than in wave.
      const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Vertex);
      const auto tcsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl);
      if (vsResUsage->useRayQueryLdsStack || tcsResUsage->useRayQueryLdsStack)
        hwConfig.rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;

      // Make sure we don't run out of LDS space.
      assert(hwConfig.tessOnChipLdsSize + hwConfig.rayQueryLdsStackSize <=
             m_pipelineState->getTargetInfo().getGpuProperty().ldsSizePerThreadGroup);

      auto printLdsLayout = [=](const char *name, unsigned offset, unsigned size) {
        if (size != 0) {
          LLPC_OUTS(format("%-30s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, name, offset, size));
          LLPC_OUTS("\n");
        }
      };

      LLPC_OUTS("===============================================================================\n");
      LLPC_OUTS("// LLPC HW tessellation configurations\n\n");
      LLPC_OUTS("MaxNumPatchesPerGroup = " << hwConfig.maxNumPatchesPerGroup << "\n");
      LLPC_OUTS("Primitive = ");
      switch (m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode) {
      case PrimitiveMode::Triangles:
        LLPC_OUTS("Triangles");
        break;
      case PrimitiveMode::Quads:
        LLPC_OUTS("Quads");
        break;
      case PrimitiveMode::Isolines:
        LLPC_OUTS("Isolines");
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
      LLPC_OUTS(" (HW TFs = " << tessFactorCount << " dwords)\n");
      LLPC_OUTS("TF0/TF1 Messaging = " << (m_pipelineState->canOptimizeTessFactor() ? "true" : "false") << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Tessellator Patch [OnChip, OffChip]:\n");
      LLPC_OUTS("InputVertices = " << inputVertexCount << ", VertexStride = " << hwConfig.onChip.inputVertexStride
                                   << " dwords, Size = " << hwConfig.onChip.inputPatchSize << " dwords\n");
      LLPC_OUTS("OutputVertices = " << outputVertexCount << ", VertexStride = [" << hwConfig.onChip.outputVertexStride
                                    << ", " << hwConfig.offChip.outputVertexStride << "] dwords, Size = ["
                                    << hwConfig.onChip.outputPatchSize << ", " << hwConfig.offChip.outputPatchSize
                                    << "] dwords\n");
      LLPC_OUTS("PatchConstants = "
                << "[" << onChipPatchConstCount << ", " << offChipPatchConstCount << "], Size = ["
                << hwConfig.onChip.patchConstSize << ", " << hwConfig.offChip.patchConstSize << "] dwords\n");

      LLPC_OUTS("\n");
      LLPC_OUTS("Onchip LDS Layout (in dwords):\n");
      if (m_pipelineState->canOptimizeTessFactor()) {
        printLdsLayout("HS Patch Count", hwConfig.onChip.hsPatchCountStart, 1);
        printLdsLayout("Special TF Values", hwConfig.onChip.specialTfValueStart, hwConfig.onChip.specialTfValueSize);
      }
      printLdsLayout("Output Patches", hwConfig.onChip.outputPatchStart, onChipOutputPatchTotalSize);
      printLdsLayout("Patch Constants", hwConfig.onChip.patchConstStart, onChipPatchConstTotalSize);
      printLdsLayout("TFs", hwConfig.onChip.tessFactorStart, tessFactorTotalSize);
      printLdsLayout("Input Patches", hwConfig.onChip.inputPatchStart, inputPatchTotalSize);
      if (hwConfig.rayQueryLdsStackSize > 0)
        printLdsLayout("Ray Query Stack", hwConfig.tessOnChipLdsSize, hwConfig.rayQueryLdsStackSize);
      LLPC_OUTS("Total Onchip LDS = " << hwConfig.tessOnChipLdsSize + hwConfig.rayQueryLdsStackSize << " dwords\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Offchip LDS Buffer Layout (in dwords):\n");
      printLdsLayout("Output Patches", hwConfig.offChip.outputPatchStart, offChipOutputPatchTotalSize);
      printLdsLayout("Patch Constants", hwConfig.offChip.patchConstStart, offChipPatchConstTotalSize);
      LLPC_OUTS("Total Offchip LDS Buffer = " << offChipOutputPatchTotalSize + offChipPatchConstTotalSize
                                              << " dwords\n");
      LLPC_OUTS("\n");
    }
  }

  if (m_shaderStage == ShaderStage::Compute || m_shaderStage == ShaderStage::Task) {
    auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
    for (Function &func : *m_module) {
      // Different with above, this will force the threadID swizzle which will rearrange thread ID within a group into
      // blocks of 8*4, not to reconfig workgroup automatically and will support to be swizzled in 8*4 block
      // split.
      if (func.isDeclaration() && func.getName().starts_with(lgcName::ReconfigureLocalInvocationId)) {
        unsigned workgroupSizeX = mode.workgroupSizeX;
        unsigned workgroupSizeY = mode.workgroupSizeY;
        unsigned workgroupSizeZ = mode.workgroupSizeZ;
        SwizzleWorkgroupLayout layout = calculateWorkgroupLayout(m_pipelineState, m_shaderStage.value());
        if (m_gfxIp.major >= 12) {
          // For HW swizzle, the large-pattern unroll is basically the same Z-order pattern used for 2x2
          WorkgroupLayout swizzleWgLayout = WorkgroupLayout::Unknown;
          if (layout.macroLayout == WorkgroupLayout::Unknown)
            swizzleWgLayout = layout.microLayout;
          else
            swizzleWgLayout = layout.macroLayout;

          PalMetadata *metadata = m_pipelineState->getPalMetadata();
          if (m_pipelineState->getOptions().xInterleave != 0 || m_pipelineState->getOptions().yInterleave != 0) {
            metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::ComputeRegisters].getMap(
                true)[Util::Abi::ComputeRegisterMetadataKey::XInterleave] = m_pipelineState->getOptions().xInterleave;
            metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::ComputeRegisters].getMap(
                true)[Util::Abi::ComputeRegisterMetadataKey::YInterleave] = m_pipelineState->getOptions().yInterleave;
          } else {
            switch (swizzleWgLayout) {
            case WorkgroupLayout::Quads:
              metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::ComputeRegisters].getMap(
                  true)[Util::Abi::ComputeRegisterMetadataKey::XInterleave] = 1;
              metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::ComputeRegisters].getMap(
                  true)[Util::Abi::ComputeRegisterMetadataKey::YInterleave] = 1;
              break;
            case WorkgroupLayout::SexagintiQuads:
              metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::ComputeRegisters].getMap(
                  true)[Util::Abi::ComputeRegisterMetadataKey::XInterleave] = 3;
              metadata->getPipelineNode()[Util::Abi::PipelineMetadataKey::ComputeRegisters].getMap(
                  true)[Util::Abi::ComputeRegisterMetadataKey::YInterleave] = 3;
              break;
            default:
              break;
            }
          }
        }

        while (!func.use_empty()) {
          CallInst *reconfigCall = cast<CallInst>(*func.user_begin());
          Value *localInvocationId = reconfigCall->getArgOperand(0);
          if (m_gfxIp.major <= 11) {
            bool isHwLocalInvocationId = cast<ConstantInt>(reconfigCall->getArgOperand(1))->getZExtValue();
            if ((layout.microLayout == WorkgroupLayout::Quads) ||
                (layout.macroLayout == WorkgroupLayout::SexagintiQuads)) {
              BuilderBase builder(reconfigCall);
              localInvocationId = reconfigWorkgroupLayout(
                  localInvocationId, m_pipelineState, m_shaderStage.value(), layout.macroLayout, layout.microLayout,
                  workgroupSizeX, workgroupSizeY, workgroupSizeZ, isHwLocalInvocationId, builder);
            }
          }
          reconfigCall->replaceAllUsesWith(localInvocationId);
          reconfigCall->eraseFromParent();
        }
      }

      if (func.isDeclaration() && func.getName().starts_with(lgcName::SwizzleWorkgroupId)) {
        createSwizzleThreadGroupFunction();
      }
    }
  }
}

// =====================================================================================================================
// Visits all "call" instructions against the callee functions in current entry-point function.
//
// @param calleeFuncs : a list of candidate callee functions to check
void LowerInOut::visitCallInsts(ArrayRef<Function *> calleeFuncs) {
  for (auto callee : calleeFuncs) {
    for (auto user : callee->users()) {
      if (CallInst *callInst = dyn_cast<CallInst>(user)) {
        if (callInst->getFunction() == m_entryPoint)
          visitCallInst(*callInst);
      }
    }
  }
}

// =====================================================================================================================
// Visits all "ret" instructions in current entry-point function.
void LowerInOut::visitReturnInsts() {
  for (auto &block : *m_entryPoint)
    if (auto *retInst = dyn_cast<ReturnInst>(block.getTerminator()))
      visitReturnInst(*retInst);
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void LowerInOut::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&callInst);

  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage.value());

  auto mangledName = callee->getName();

  auto importBuiltInInput = lgcName::InputImportBuiltIn;
  auto importBuiltInOutput = lgcName::OutputImportBuiltIn;

  const bool isGenericInputImport = isa<InputImportGenericOp>(callInst);
  const bool isBuiltInInputImport = mangledName.starts_with(importBuiltInInput);
  const bool isInterpolatedInputImport = isa<InputImportInterpolatedOp>(callInst);
  const bool isGenericOutputImport = isa<OutputImportGenericOp>(callInst);
  const bool isBuiltInOutputImport = mangledName.starts_with(importBuiltInOutput);

  const bool isImport = (isGenericInputImport || isBuiltInInputImport || isInterpolatedInputImport ||
                         isGenericOutputImport || isBuiltInOutputImport);

  auto exportGenericOutput = lgcName::OutputExportGeneric;
  auto exportBuiltInOutput = lgcName::OutputExportBuiltIn;

  const bool isGenericOutputExport = mangledName.starts_with(exportGenericOutput);
  const bool isBuiltInOutputExport = mangledName.starts_with(exportBuiltInOutput);
  const bool isXfbOutputExport = isa<WriteXfbOutputOp>(callInst);

  const bool isExport = (isGenericOutputExport || isBuiltInOutputExport || isXfbOutputExport);

  const bool isInput = (isGenericInputImport || isBuiltInInputImport || isInterpolatedInputImport);
  const bool isOutput = (isGenericOutputImport || isBuiltInOutputImport || isGenericOutputExport ||
                         isBuiltInOutputExport || isXfbOutputExport);

  if (isImport && isInput) {
    // Input imports
    Value *input = nullptr;
    Type *inputTy = callInst.getType();

    m_importCalls.push_back(&callInst);

    if (isBuiltInInputImport) {
      const unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

      LLVM_DEBUG(dbgs() << "Find input import call: builtin = " << builtInId << "\n");

      switch (m_shaderStage.value()) {
      case ShaderStage::Vertex:
        // Nothing to do
        break;
      case ShaderStage::TessControl: {
        // Builtin Call has different number of operands
        Value *elemIdx = nullptr;
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.arg_size() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

        input = readTcsBuiltInInput(inputTy, builtInId, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStage::TessEval: {
        // Builtin Call has different number of operands
        Value *elemIdx = nullptr;
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.arg_size() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);
        input = readTesBuiltInInput(inputTy, builtInId, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStage::Geometry: {
        // Builtin Call has different number of operands
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          vertexIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        input = readGsBuiltInInput(inputTy, builtInId, vertexIdx, builder);
        break;
      }
      case ShaderStage::Mesh: {
        assert(callInst.arg_size() == 2);
        assert(isDontCareValue(callInst.getOperand(1)));
        input = readMeshBuiltInInput(inputTy, builtInId, builder);
        break;
      }
      case ShaderStage::Fragment: {
        Value *generalVal = nullptr;
        if (callInst.arg_size() >= 2)
          generalVal = callInst.getArgOperand(1);
        input = readFsBuiltInInput(inputTy, builtInId, generalVal, builder);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else {
      assert(m_shaderStage != ShaderStage::Vertex && "vertex fetch is handled by LowerVertexFetch");

      auto &genericLocationOp = cast<GenericLocationOp>(callInst);
      assert(isGenericInputImport || isInterpolatedInputImport);

      LLVM_DEBUG(dbgs() << "Find input import call: generic location = " << genericLocationOp.getLocation() << "\n");

      unsigned origLoc = genericLocationOp.getLocation();
      unsigned loc = InvalidValue;
      Value *locOffset = genericLocationOp.getLocOffset();
      Value *elemIdx = nullptr;
      bool highHalf = false;

      if (auto *constLocOffset = dyn_cast<ConstantInt>(locOffset)) {
        origLoc += constLocOffset->getZExtValue();
        locOffset = nullptr;
      } else {
        assert(m_shaderStage == ShaderStage::TessControl || m_shaderStage == ShaderStage::TessEval ||
               m_shaderStage == ShaderStage::Fragment);
      }

      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(origLoc);
      if (m_shaderStage == ShaderStage::TessEval ||
          (m_shaderStage == ShaderStage::Fragment &&
           (m_pipelineState->getPrevShaderStage(m_shaderStage.value()) == ShaderStage::Mesh ||
            m_pipelineState->isUnlinked()))) {
        // NOTE: For generic inputs of tessellation evaluation shader or fragment shader whose previous shader stage
        // is mesh shader or is in unlinked pipeline, they could be per-patch ones or per-primitive ones.
        const bool isPerPrimitive = genericLocationOp.getPerPrimitive();
        if (isPerPrimitive) {
          auto &checkedMap = m_shaderStage == ShaderStage::TessEval ? resUsage->inOutUsage.perPatchInputLocMap
                                                                    : resUsage->inOutUsage.perPrimitiveInputLocMap;
          auto locMapIt = checkedMap.find(origLoc);
          if (locMapIt != checkedMap.end())
            loc = locMapIt->second;
        } else {
          // NOTE: We need consider <location, component> key if component index is constant. Because inputs within same
          // location are compacted.
          auto locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
          if (locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end()) {
            loc = locInfoMapIt->second.getLocation();
          } else {
            assert(isa<ConstantInt>(genericLocationOp.getElemIdx()));
            origLocInfo.setComponent(cast<ConstantInt>(genericLocationOp.getElemIdx())->getZExtValue());
            auto locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
            if (locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end())
              loc = locInfoMapIt->second.getLocation();
          }
        }
      } else {
        if (m_pipelineState->canPackInput(m_shaderStage.value())) {
          // The inputLocInfoMap of {TCS, GS, FS} maps original InOutLocationInfo to tightly compact InOutLocationInfo
          const bool isTcs = m_shaderStage == ShaderStage::TessControl;
          (void)isTcs;
          // All packing of the VS-TCS interface is disabled if dynamic indexing is detected
          assert(!isTcs || (isa<ConstantInt>(genericLocationOp.getLocOffset()) &&
                            isa<ConstantInt>(genericLocationOp.getElemIdx())));
          origLocInfo.setComponent(cast<ConstantInt>(genericLocationOp.getElemIdx())->getZExtValue());
          auto locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
          assert(locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end());

          loc = locInfoMapIt->second.getLocation();
          elemIdx = builder.getInt32(locInfoMapIt->second.getComponent());
          highHalf = locInfoMapIt->second.isHighHalf();
        } else {
          // NOTE: We need consider <location, component> key if component index is constant. Because inputs within same
          // location are compacted.
          auto locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
          if (locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end()) {
            loc = locInfoMapIt->second.getLocation();
          } else {
            assert(isa<ConstantInt>(genericLocationOp.getElemIdx()));
            origLocInfo.setComponent(cast<ConstantInt>(genericLocationOp.getElemIdx())->getZExtValue());
            auto locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
            assert(locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end());
            if (locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end())
              loc = locInfoMapIt->second.getLocation();
          }
        }
      }
      assert(loc != InvalidValue);

      if (!elemIdx)
        elemIdx = genericLocationOp.getElemIdx();
      assert(isDontCareValue(elemIdx) == false);

      switch (m_shaderStage.value()) {
      case ShaderStage::TessControl: {
        auto &inputOp = cast<InputImportGenericOp>(genericLocationOp);
        auto vertexIdx = inputOp.getArrayIndex();
        assert(isDontCareValue(vertexIdx) == false);

        input = readTcsGenericInput(inputTy, loc, locOffset, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStage::TessEval: {
        auto &inputOp = cast<InputImportGenericOp>(genericLocationOp);

        Value *vertexIdx = nullptr;
        if (!inputOp.getPerPrimitive())
          vertexIdx = inputOp.getArrayIndex();

        input = readTesGenericInput(inputTy, loc, locOffset, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStage::Geometry: {
        const unsigned compIdx = cast<ConstantInt>(elemIdx)->getZExtValue();

        auto &inputOp = cast<InputImportGenericOp>(genericLocationOp);
        Value *vertexIdx = inputOp.getArrayIndex();
        assert(isDontCareValue(vertexIdx) == false);

        input = readGsGenericInput(inputTy, loc, compIdx, vertexIdx, builder);
        break;
      }
      case ShaderStage::Fragment: {
        unsigned interpMode = InOutInfo::InterpModeSmooth;
        Value *interpValue = nullptr;
        bool isPerPrimitive = false;

        if (auto *inputImportInterpolated = dyn_cast<InputImportInterpolatedOp>(&genericLocationOp)) {
          interpMode = inputImportInterpolated->getInterpMode();
          interpValue = inputImportInterpolated->getInterpValue();
        } else {
          assert(isa<InputImportGenericOp>(genericLocationOp));
          isPerPrimitive = true;
          interpMode = InOutInfo::InterpModeFlat;
        }

        input = readFsGenericInput(inputTy, loc, locOffset, elemIdx, isPerPrimitive, interpMode, interpValue, highHalf,
                                   builder);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    }

    callInst.replaceAllUsesWith(input);
  } else if (isImport && isOutput) {
    // Output imports
    assert(m_shaderStage == ShaderStage::TessControl);

    Value *output = nullptr;
    Type *outputTy = callInst.getType();

    m_importCalls.push_back(&callInst);

    if (isBuiltInOutputImport) {
      const unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

      LLVM_DEBUG(dbgs() << "Find output import call: builtin = " << builtInId << "\n");

      assert(callInst.arg_size() == 3);
      Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
      Value *vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

      output = readTcsBuiltInOutput(outputTy, builtInId, elemIdx, vertexIdx, builder);
    } else {
      auto &outputImportGeneric = cast<OutputImportGenericOp>(callInst);

      LLVM_DEBUG(dbgs() << "Find output import call: generic location = " << outputImportGeneric.getLocation() << "\n");

      unsigned origLoc = outputImportGeneric.getLocation();
      unsigned loc = InvalidValue;

      // NOTE: If location offset is a constant, we have to add it to the unmapped location before querying
      // the mapped location. Meanwhile, we have to adjust the location offset to 0 (rebase it).
      Value *locOffset = outputImportGeneric.getLocOffset();
      if (isa<ConstantInt>(locOffset)) {
        origLoc += cast<ConstantInt>(locOffset)->getZExtValue();
        locOffset = builder.getInt32(0);
      }

      // NOTE: For generic outputs of tessellation control shader, they could be per-patch ones.
      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(origLoc);
      auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
      if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
        loc = locInfoMapIt->second.getLocation();
      } else {
        assert(resUsage->inOutUsage.perPatchOutputLocMap.find(origLoc) !=
               resUsage->inOutUsage.perPatchOutputLocMap.end());
        loc = resUsage->inOutUsage.perPatchOutputLocMap[origLoc];
      }
      assert(loc != InvalidValue);

      auto elemIdx = outputImportGeneric.getElemIdx();
      assert(isDontCareValue(elemIdx) == false);
      auto vertexIdx = outputImportGeneric.getPerPrimitive() ? nullptr : outputImportGeneric.getArrayIndex();

      output = readTcsGenericOutput(outputTy, loc, locOffset, elemIdx, vertexIdx, builder);
    }

    callInst.replaceAllUsesWith(output);
  } else if (isExport) {
    // Output exports
    assert(isOutput);

    Value *output = callInst.getOperand(callInst.arg_size() - 1); // Last argument

    // Generic value (location or SPIR-V built-in ID or XFB buffer ID)
    unsigned value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

    LLVM_DEBUG(dbgs() << "Find output export call: builtin = " << isBuiltInOutputExport << " value = " << value
                      << "\n");

    m_exportCalls.push_back(&callInst);

    if (isXfbOutputExport) {
      unsigned xfbBuffer = value;
      assert(xfbBuffer < MaxTransformFeedbackBuffers);

      unsigned xfbOffset = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
      unsigned streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();

      // NOTE: Transform feedback output will be done in last vertex-processing shader stage.
      switch (m_shaderStage.value()) {
      case ShaderStage::Vertex: {
        // No TS/GS pipeline, VS is the last stage
        if (!m_hasGs && !m_hasTs)
          writeXfbOutput(output, xfbBuffer, xfbOffset, streamId, builder);
        break;
      }
      case ShaderStage::TessEval: {
        // TS-only pipeline, TES is the last stage
        if (!m_hasGs)
          writeXfbOutput(output, xfbBuffer, xfbOffset, streamId, builder);
        break;
      }
      case ShaderStage::Geometry: {
        // Do nothing, transform feedback output is done in copy shader
        break;
      }
      case ShaderStage::CopyShader: {
        // TS-GS or GS-only pipeline, copy shader is the last stage
        writeXfbOutput(output, xfbBuffer, xfbOffset, streamId, builder);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else if (isBuiltInOutputExport) {
      const unsigned builtInId = value;

      switch (m_shaderStage.value()) {
      case ShaderStage::Vertex: {
        writeVsBuiltInOutput(output, builtInId, builder);
        break;
      }
      case ShaderStage::TessControl: {
        assert(callInst.arg_size() == 4);
        Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
        Value *vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

        writeTcsBuiltInOutput(output, builtInId, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStage::TessEval: {
        writeTesBuiltInOutput(output, builtInId, builder);
        break;
      }
      case ShaderStage::Geometry: {
        const unsigned streamId = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        writeGsBuiltInOutput(output, builtInId, streamId, builder);
        break;
      }
      case ShaderStage::Mesh: {
        assert(callInst.arg_size() == 5);
        Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
        Value *vertexOrPrimitiveIdx = callInst.getOperand(2);
        bool isPerPrimitive = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue() != 0;

        writeMeshBuiltInOutput(output, builtInId, elemIdx, vertexOrPrimitiveIdx, isPerPrimitive, builder);
        break;
      }
      case ShaderStage::Fragment: {
        writeFsBuiltInOutput(output, builtInId, builder);
        break;
      }
      case ShaderStage::CopyShader: {
        writeCopyShaderBuiltInOutput(output, builtInId, builder);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else {
      assert(isGenericOutputExport);

      bool exist = false;
      unsigned loc = InvalidValue;
      Value *locOffset = nullptr;
      unsigned elemIdx = InvalidValue;

      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(value);
      if (m_shaderStage == ShaderStage::Geometry)
        origLocInfo.setStreamId(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());

      if (m_shaderStage == ShaderStage::TessControl || m_shaderStage == ShaderStage::Mesh) {
        locOffset = callInst.getOperand(1);

        // NOTE: For generic outputs of tessellation control shader or mesh shader, they could be per-patch ones or
        // per-primitive ones.
        if (m_shaderStage == ShaderStage::Mesh && cast<ConstantInt>(callInst.getOperand(4))->getZExtValue() != 0) {
          auto locMapIt = resUsage->inOutUsage.perPrimitiveOutputLocMap.find(value);
          if (locMapIt != resUsage->inOutUsage.perPrimitiveOutputLocMap.end()) {
            loc = locMapIt->second;
            exist = true;
          }
        } else if (m_shaderStage == ShaderStage::TessControl && isDontCareValue(callInst.getOperand(3))) {
          auto locMapIt = resUsage->inOutUsage.perPatchOutputLocMap.find(value);
          if (locMapIt != resUsage->inOutUsage.perPatchOutputLocMap.end()) {
            loc = locMapIt->second;
            exist = true;
          }
        } else {
          // NOTE: We need consider <location, component> key if component index is constant. Because outputs within
          // same location are compacted.
          auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
          if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
            loc = locInfoMapIt->second.getLocation();
            exist = true;
          } else if (isa<ConstantInt>(callInst.getOperand(2))) {
            origLocInfo.setComponent(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());
            auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
            if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
              loc = locInfoMapIt->second.getLocation();
              exist = true;
            }
          }
        }
      } else if (m_shaderStage == ShaderStage::CopyShader) {
        exist = true;
        loc = value;
      } else {
        // Generic output exports of FS should have been handled by the LowerFragmentColorExport pass
        assert(m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::Geometry ||
               m_shaderStage == ShaderStage::TessEval);

        // Check component offset and search the location info map once again
        unsigned component = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        if (output->getType()->getScalarSizeInBits() == 64)
          component *= 2; // Component in location info is dword-based
        origLocInfo.setComponent(component);
        auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);

        if (m_pipelineState->canPackOutput(m_shaderStage.value())) {
          if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
            loc = locInfoMapIt->second.getLocation();
            elemIdx = locInfoMapIt->second.getComponent();
            exist = true;
          } else {
            exist = false;
          }
        } else if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
          exist = true;
          loc = locInfoMapIt->second.getLocation();
        }
      }

      if (exist) {
        // NOTE: Some outputs are not used by next shader stage. They must have been removed already.
        assert(loc != InvalidValue);

        switch (m_shaderStage.value()) {
        case ShaderStage::Vertex: {
          assert(callInst.arg_size() == 3);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          writeVsGenericOutput(output, loc, elemIdx, builder);
          break;
        }
        case ShaderStage::TessControl: {
          assert(callInst.arg_size() == 5);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

          writeTcsGenericOutput(output, loc, locOffset, elemIdx, vertexIdx, builder);
          break;
        }
        case ShaderStage::TessEval: {
          assert(callInst.arg_size() == 3);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          writeTesGenericOutput(output, loc, elemIdx, builder);
          break;
        }
        case ShaderStage::Geometry: {
          assert(callInst.arg_size() == 4);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          const unsigned streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
          writeGsGenericOutput(output, loc, elemIdx, streamId, builder);
          break;
        }
        case ShaderStage::Mesh: {
          assert(callInst.arg_size() == 6);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexOrPrimitiveIdx = callInst.getOperand(3);
          bool isPerPrimitive = cast<ConstantInt>(callInst.getOperand(4))->getZExtValue() != 0;
          writeMeshGenericOutput(output, loc, locOffset, elemIdx, vertexOrPrimitiveIdx, isPerPrimitive, builder);
          break;
        }
        case ShaderStage::CopyShader: {
          addExportInstForGenericOutput(output, loc, 0, builder);
          break;
        }
        default: {
          llvm_unreachable("Should never be called!");
          break;
        }
        }
      }
    }
  } else {
    // Other calls relevant to input/output import/export
    if (isa<GsEmitStreamOp>(callInst)) {
      assert(m_shaderStage == ShaderStage::Geometry); // Must be geometry shader

      const unsigned streamId = cast<GsEmitStreamOp>(callInst).getStreamId();
      assert(streamId < MaxGsStreams);

      // NOTE: Implicitly store the value of view index to GS-VS ring buffer for raster stream if multi-view is
      // enabled. Copy shader will read the value from GS-VS ring and export it to vertex position data.
      if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
        auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry);
        auto rasterStream = m_pipelineState->getRasterizerState().rasterStream;

        if (streamId == rasterStream) {
          // When multiview and viewIndexFromDeviceIndex enable, it can't use the device ID
          // as viewId to storeValueToGsVsRing when multiview in the same device
          auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs;
          auto viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);

          const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
          assert(builtInOutLocMap.find(BuiltInViewIndex) != builtInOutLocMap.end());
          unsigned loc = builtInOutLocMap.find(BuiltInViewIndex)->second;

          storeValueToGsVsRing(viewIndex, loc, 0, rasterStream, builder);
        }
      }

      // Increment emit counter
      auto emitCounterPair = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr();
      auto emitCounterTy = emitCounterPair.first;
      auto emitCounterPtr = emitCounterPair.second[streamId];
      Value *emitCounter = builder.CreateLoad(emitCounterTy, emitCounterPtr);
      emitCounter = builder.CreateAdd(emitCounter, builder.getInt32(1));
      builder.CreateStore(emitCounter, emitCounterPtr);

      // Increment total emit counter
      if (m_pipelineState->getShaderModes()->getGeometryShaderMode().robustGsEmits) {
        auto totalEmitCounterPtr = m_pipelineSysValues.get(m_entryPoint)->getTotalEmitCounterPtr();
        Value *totalEmitCounter = builder.CreateLoad(builder.getInt32Ty(), totalEmitCounterPtr);

        // totalEmitCounter++
        totalEmitCounter = builder.CreateAdd(totalEmitCounter, builder.getInt32(1));
        builder.CreateStore(totalEmitCounter, totalEmitCounterPtr);

        if (!m_pipelineState->getNggControl()->enableNgg) {
          // NOTE: For legacy GS, the counters of primitives written are driven by the message GS_EMIT/GS_CUT.
          // Therefore, we must send such message conditionally by checking if the emit is within expected range.
          assert(m_gfxIp.major < 11);

          // validEmit = totalEmitCounter <= outputVertices
          const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
          auto validEmit = builder.CreateICmpULE(totalEmitCounter, builder.getInt32(geometryMode.outputVertices));

          // Send the GS_EMIT message conditionally
          builder.CreateIf(validEmit, false);
          callInst.moveBefore(builder.GetInsertPoint());
          builder.SetInsertPoint(&callInst); // Restore insert point modified by CreateIf
        }
      }

      // For legacy GS, lower the dialect op GsEmitStreamOp to sendmsg intrinsic
      if (!m_pipelineState->getNggControl()->enableNgg) {
        m_gsMsgCalls.push_back(&callInst);

        auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs;
        auto gsWaveId = getFunctionArgument(m_entryPoint, entryArgIdxs.gsWaveId);

        // [9:8] = stream, [5:4] = 2 (emit), [3:0] = 2 (GS)
        unsigned msg = (streamId << 8) | GsEmit;
        builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {builder.getInt32(msg), gsWaveId}, nullptr);
      }
    } else if (isa<GsCutStreamOp>(callInst)) {
      assert(m_shaderStage == ShaderStage::Geometry); // Must be geometry shader

      const unsigned streamId = cast<GsCutStreamOp>(callInst).getStreamId();
      assert(streamId < MaxGsStreams);

      // For legacy GS, lower the dialect op GsCutStreamOp to sendmsg intrinsic
      if (!m_pipelineState->getNggControl()->enableNgg) {
        m_gsMsgCalls.push_back(&callInst);

        auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs;
        auto gsWaveId = getFunctionArgument(m_entryPoint, entryArgIdxs.gsWaveId);

        // [9:8] = stream, [5:4] = 1 (cut), [3:0] = 2 (GS)
        unsigned msg = (streamId << 8) | GsCut;
        builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {builder.getInt32(msg), gsWaveId}, nullptr);
      }
    }
  }
}

// =====================================================================================================================
// Visits "ret" instruction.
//
// @param retInst : "Ret" instruction
void LowerInOut::visitReturnInst(ReturnInst &retInst) {
  // We only handle the "ret" of shader entry point
  if (!m_shaderStage)
    return;

  const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage.value());

  // Whether this shader stage has to use "exp" instructions to export outputs
  const bool useExpInst = ((m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
                            m_shaderStage == ShaderStage::CopyShader) &&
                           (!nextStage || nextStage == ShaderStage::Fragment));

  BuilderBase builder(&retInst);

  auto zero = ConstantFP::get(builder.getFloatTy(), 0.0);
  auto one = ConstantFP::get(builder.getFloatTy(), 1.0);
  auto poison = PoisonValue::get(builder.getFloatTy());

  const bool enableXfb = m_pipelineState->enableXfb();
  if (m_shaderStage == ShaderStage::CopyShader && enableXfb) {
    if (!m_pipelineState->getNggControl()->enableNgg) {
      // NOTE: For copy shader, if transform feedback is enabled for multiple streams, the following processing doesn't
      // happen in return block. Rather, they happen in the switch-case branch for the raster stream. See the following:
      //
      //   copyShader() {
      //     ...
      //     switch(streamId) {
      //     case 0:
      //       export outputs of stream 0
      //       break
      //     ...
      //     case rasterStream:
      //       export outputs of raster stream
      //       break
      //     ...
      //     case 3:
      //       export outputs of stream 3
      //       break
      //   }
      //
      //   return
      // }
      //
      // If NGG, the copy shader with stream-out is not a real HW VS and will be incorporated into NGG
      // primitive shader later. There is no multiple HW executions. And it has the following structure similar to
      // single stream processing:
      //
      //   copyShader() {
      //     ...
      //     export outputs of stream 0
      //     ...
      //     export outputs of raster stream
      //     ...
      //     export outputs of stream 3
      //
      //     return
      //   }
      //
      bool updated = false;
      for (auto &block : *m_entryPoint) {
        // Search blocks to find the switch-case instruction
        auto switchInst = dyn_cast<SwitchInst>(block.getTerminator());
        if (switchInst) {
          for (auto &caseBranch : switchInst->cases()) {
            if (caseBranch.getCaseValue()->getZExtValue() == m_pipelineState->getRasterizerState().rasterStream) {
              // The insert position is updated to this case branch, before the terminator
              builder.SetInsertPoint(caseBranch.getCaseSuccessor()->getTerminator());
              updated = true;
              // We must go to return block from this case branch
              assert(caseBranch.getCaseSuccessor()->getSingleSuccessor() == retInst.getParent());
              break;
            }
          }

          if (updated)
            break; // Early exit if we have updated the insert position
        }
      }
    }
  }

  if (useExpInst) {
    bool usePosition = false;
    bool usePointSize = false;
    bool usePrimitiveId = false;
    bool useLayer = false;
    bool useViewportIndex = false;
    bool useShadingRate = false;
    bool useEdgeFlag = false;
    unsigned clipDistanceCount = 0;
    unsigned cullDistanceCount = 0;

    auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage.value())->inOutUsage;

    if (m_shaderStage == ShaderStage::Vertex) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Vertex)->builtInUsage.vs;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
      useEdgeFlag = builtInUsage.edgeFlag;
    } else if (m_shaderStage == ShaderStage::TessEval) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessEval)->builtInUsage.tes;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    } else {
      assert(m_shaderStage == ShaderStage::CopyShader);
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::CopyShader)->builtInUsage.gs;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    }

    const auto enableMultiView = m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable;
    if (enableMultiView) {
      if (m_shaderStage == ShaderStage::Vertex) {
        if (m_pipelineState->getShaderOptions(m_shaderStage.value()).viewIndexFromDeviceIndex) {
          m_viewIndex = builder.getInt32(m_pipelineState->getDeviceIndex());
        } else {
          auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex)->entryArgIdxs.vs;
          m_viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);
        }
      } else if (m_shaderStage == ShaderStage::TessEval) {
        if (m_pipelineState->getShaderOptions(m_shaderStage.value()).viewIndexFromDeviceIndex) {
          m_viewIndex = builder.getInt32(m_pipelineState->getDeviceIndex());
        } else {
          auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::TessEval)->entryArgIdxs.tes;
          m_viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);
        }
      } else {
        assert(m_shaderStage == ShaderStage::CopyShader);
        assert(m_viewIndex); // Must have been explicitly loaded in copy shader
      }
    }

    const auto &builtInOutLocs =
        m_shaderStage == ShaderStage::CopyShader ? inOutUsage.gs.builtInOutLocs : inOutUsage.builtInOutputLocMap;
    const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;

    // NOTE: If gl_Position is not present in this shader stage, we have to export a dummy one.
    if (!usePosition)
      exportPosition(0, {zero, zero, zero, one}, builder);

    // NOTE: In such case, last shader in the pre-rasterization doesn't export layer while fragment shader expects to
    // read it. Should export 0 to fragment shader, which is required by the spec.
    if (!useLayer && nextStage == ShaderStage::Fragment && nextBuiltInUsage.layer) {
      assert(m_layer == nullptr);
      m_layer = builder.getInt32(0);
      useLayer = true;
    }

    // Export gl_ClipDistance[] and gl_CullDistance[] before entry-point returns
    if (clipDistanceCount > 0 || cullDistanceCount > 0) {
      assert(clipDistanceCount + cullDistanceCount <= MaxClipCullDistanceCount);

      assert(clipDistanceCount == 0 || (clipDistanceCount > 0 && m_clipDistance));
      assert(cullDistanceCount == 0 || (cullDistanceCount > 0 && m_cullDistance));

      // Extract elements of gl_ClipDistance[] and gl_CullDistance[]
      std::vector<Value *> clipDistance;
      for (unsigned i = 0; i < clipDistanceCount; ++i)
        clipDistance.push_back(builder.CreateExtractValue(m_clipDistance, i));

      std::vector<Value *> cullDistance;
      for (unsigned i = 0; i < cullDistanceCount; ++i)
        cullDistance.push_back(builder.CreateExtractValue(m_cullDistance, i));

      // Merge gl_ClipDistance[] and gl_CullDistance[]
      SmallVector<Value *, 8> clipCullDistance;
      clipCullDistance.reserve(clipDistance.size() + cullDistance.size());
      for (auto clipDistanceElement : clipDistance)
        clipCullDistance.push_back(clipDistanceElement);

      for (auto cullDistanceElement : cullDistance)
        clipCullDistance.push_back(cullDistanceElement);

      // Do array padding
      if (clipCullDistance.size() <= 4) {
        while (clipCullDistance.size() < 4) // [4 x float]
          clipCullDistance.push_back(poison);
      } else {
        while (clipCullDistance.size() < 8) // [8 x float]
          clipCullDistance.push_back(poison);
      }

      bool miscExport =
          usePointSize || useLayer || useViewportIndex || useShadingRate || enableMultiView || useEdgeFlag;
      // NOTE: When misc. export is present, gl_ClipDistance[] or gl_CullDistance[] should start from pos2.
      unsigned exportSlot = miscExport ? 2 : 1;

      unsigned clipPlaneMask = m_pipelineState->getOptions().clipPlaneMask;
      bool needMapClipDistMask = ((clipPlaneMask != 0) && m_pipelineState->getOptions().enableMapClipDistMask);
      assert(!m_pipelineState->getOptions().enableMapClipDistMask || ((clipPlaneMask & 0xF) == 0));

      if (!needMapClipDistMask) {
        exportPosition(exportSlot, {clipCullDistance[0], clipCullDistance[1], clipCullDistance[2], clipCullDistance[3]},
                       builder);
        exportSlot++;
      }

      if (clipCullDistance.size() > 4) {
        // Do the second exporting
        exportPosition(exportSlot, {clipCullDistance[4], clipCullDistance[5], clipCullDistance[6], clipCullDistance[7]},
                       builder);
      }

      // NOTE: We have to export gl_ClipDistance[] or gl_CullDistancep[] via generic outputs as well.
      assert(!nextStage || nextStage == ShaderStage::Fragment);

      bool hasClipCullExport = true;
      if (nextStage == ShaderStage::Fragment) {
        hasClipCullExport = (nextBuiltInUsage.clipDistance > 0 || nextBuiltInUsage.cullDistance > 0);

        if (hasClipCullExport) {
          // NOTE: We adjust the array size of gl_ClipDistance[] and gl_CullDistance[] according to their
          // usages in fragment shader.
          clipDistanceCount = std::min(nextBuiltInUsage.clipDistance, clipDistanceCount);
          cullDistanceCount = std::min(nextBuiltInUsage.cullDistance, cullDistanceCount);

          clipCullDistance.clear();
          for (unsigned i = 0; i < clipDistanceCount; ++i)
            clipCullDistance.push_back(clipDistance[i]);

          for (unsigned i = clipDistanceCount; i < nextBuiltInUsage.clipDistance; ++i)
            clipCullDistance.push_back(poison);

          for (unsigned i = 0; i < cullDistanceCount; ++i)
            clipCullDistance.push_back(cullDistance[i]);

          // Do array padding
          if (clipCullDistance.size() <= 4) {
            while (clipCullDistance.size() < 4) // [4 x float]
              clipCullDistance.push_back(poison);
          } else {
            while (clipCullDistance.size() < 8) // [8 x float]
              clipCullDistance.push_back(poison);
          }
        }
      }

      if (hasClipCullExport) {
        auto it = builtInOutLocs.find(BuiltInClipDistance);
        if (it == builtInOutLocs.end())
          it = builtInOutLocs.find(BuiltInCullDistance);
        assert(it != builtInOutLocs.end());
        const unsigned loc = it->second;

        recordVertexAttribute(loc,
                              {clipCullDistance[0], clipCullDistance[1], clipCullDistance[2], clipCullDistance[3]});

        if (clipCullDistance.size() > 4) {
          // Do the second exporting
          recordVertexAttribute(loc + 1,
                                {clipCullDistance[4], clipCullDistance[5], clipCullDistance[6], clipCullDistance[7]});
        }
      }
    }

    // Export gl_PrimitiveID before entry-point returns
    if (usePrimitiveId) {
      bool hasPrimitiveIdExport = false;
      if (nextStage == ShaderStage::Fragment) {
        hasPrimitiveIdExport = nextBuiltInUsage.primitiveId;
      } else if (!nextStage) {
        if (m_shaderStage == ShaderStage::CopyShader) {
          hasPrimitiveIdExport =
              m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->builtInUsage.gs.primitiveId;
        }
      }

      if (hasPrimitiveIdExport) {
        assert(builtInOutLocs.find(BuiltInPrimitiveId) != builtInOutLocs.end());
        const unsigned loc = builtInOutLocs.find(BuiltInPrimitiveId)->second;

        assert(m_primitiveId);
        Value *primitiveId = builder.CreateBitCast(m_primitiveId, builder.getFloatTy());

        recordVertexAttribute(loc, {primitiveId, poison, poison, poison});
      }
    }

    // Export EdgeFlag
    if (useEdgeFlag)
      addExportInstForBuiltInOutput(m_edgeFlag, BuiltInEdgeFlag, builder);

    // Export gl_Layer and gl_ViewportIndex before entry-point returns
    if (useLayer || useViewportIndex || enableMultiView) {
      Value *viewportIndex = nullptr;
      Value *layer = nullptr;
      Value *viewportIndexAndLayer = builder.getInt32(0);

      if (m_pipelineState->getInputAssemblyState().multiView == MultiViewMode::PerView) {
        assert(m_viewIndex);
        // Get viewportIndex from viewIndex.
        viewportIndex = builder.CreateAnd(builder.CreateLShr(m_viewIndex, builder.getInt32(4)), builder.getInt32(0xF));
        // Get layer from viewIndex
        layer = builder.CreateLShr(m_viewIndex, builder.getInt32(8));
        if (useLayer)
          layer = builder.CreateAdd(m_layer, layer);
      } else if (m_pipelineState->getInputAssemblyState().multiView == MultiViewMode::Simple) {
        assert(m_viewIndex);
        layer = m_viewIndex;
      } else if (useLayer) {
        assert(!enableMultiView && m_layer);
        layer = m_layer;
      }

      if (useViewportIndex) {
        assert(m_viewportIndex);
        if (viewportIndex)
          viewportIndex = builder.CreateAdd(m_viewportIndex, viewportIndex);
        else
          viewportIndex = m_viewportIndex;
      }

      if (viewportIndex) {
        viewportIndexAndLayer = builder.CreateShl(viewportIndex, builder.getInt32(16));
      }

      if (layer) {
        viewportIndexAndLayer = builder.CreateOr(viewportIndexAndLayer, layer);
      }

      viewportIndexAndLayer = builder.CreateBitCast(viewportIndexAndLayer, builder.getFloatTy());
      exportPosition(1, {poison, poison, viewportIndexAndLayer, poison}, builder);

      // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
      if (useViewportIndex) {
        bool hasViewportIndexExport = true;
        if (nextStage == ShaderStage::Fragment) {
          hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
        } else if (!nextStage) {
          hasViewportIndexExport = false;
        }

        if (hasViewportIndexExport) {
          assert(builtInOutLocs.find(BuiltInViewportIndex) != builtInOutLocs.end());
          const unsigned loc = builtInOutLocs.find(BuiltInViewportIndex)->second;

          Value *viewportIndex = builder.CreateBitCast(m_viewportIndex, builder.getFloatTy());

          recordVertexAttribute(loc, {viewportIndex, poison, poison, poison});
        }
      }

      // NOTE: We have to export gl_Layer via generic outputs as well.
      if (useLayer) {
        bool hasLayerExport = true;
        if (nextStage == ShaderStage::Fragment) {
          hasLayerExport = nextBuiltInUsage.layer;
        } else if (!nextStage) {
          hasLayerExport = false;
        }

        if (hasLayerExport) {
          assert(builtInOutLocs.find(BuiltInLayer) != builtInOutLocs.end());
          const unsigned loc = builtInOutLocs.find(BuiltInLayer)->second;

          Value *layer = builder.CreateBitCast(m_layer, builder.getFloatTy());

          recordVertexAttribute(loc, {layer, poison, poison, poison});
        }
      }
    }

    // Export vertex attributes that were recorded previously
    exportAttributes(builder);

    if (m_pipelineState->isUnlinked()) {
      // If we are building unlinked relocatable shaders, it is possible there are
      // generic outputs that are not written to.  We need to count them in
      // the export count.
      auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage.value());
      for (const auto &locInfoPair : resUsage->inOutUsage.outputLocInfoMap) {
        const unsigned newLoc = locInfoPair.second.getLocation();
        if (m_expLocs.count(newLoc) != 0)
          continue;
        inOutUsage.expCount = std::max(inOutUsage.expCount, newLoc + 1); // Update export count
      }
    }
  } else if (m_shaderStage == ShaderStage::TessControl) {
    // NOTE: We will read back tessellation factors from on-chip LDS in later phases and write them to TF buffer.
    // Add fence and barrier before the return instruction to make sure they have been stored already.
    SyncScope::ID syncScope = m_context->getOrInsertSyncScopeID("workgroup");
    builder.CreateFence(AtomicOrdering::Release, syncScope);
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
      builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {});
#else
      builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
#endif
    } else {
      builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_signal, {}, builder.getInt32(WorkgroupNormalBarrierId));
      builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_wait, {},
                              builder.getInt16(static_cast<uint16_t>(WorkgroupNormalBarrierId)));
    }
    builder.CreateFence(AtomicOrdering::Acquire, syncScope);
  } else if (m_shaderStage == ShaderStage::Geometry) {
    // Send GS_DONE message for legacy GS
    if (!m_pipelineState->getNggControl()->enableNgg) {
      // NOTE: Per programming guide, we should do a "s_waitcnt 0,0,0 + s_waitcnt_vscnt 0" before issuing a "done", so
      // we use fence release to generate s_waitcnt vmcnt lgkmcnt/s_waitcnt_vscnt before s_sendmsg(MSG_GS_DONE)
      SyncScope::ID syncScope =
          m_context->getOrInsertSyncScopeID(m_pipelineState->isGsOnChip() ? "workgroup" : "agent");
      builder.CreateFence(AtomicOrdering::Release, syncScope);

      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs;
      auto gsWaveId = getFunctionArgument(m_entryPoint, entryArgIdxs.gsWaveId);
      builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_s_sendmsg, {builder.getInt32(GsDone), gsWaveId});
    }
  } else if (m_shaderStage == ShaderStage::Fragment) {
    // Fragment shader export are handled in LowerFragmentColorExport.
    return;
  }
}

// =====================================================================================================================
// Reads generic inputs of tessellation control shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readTcsGenericInput(Type *inputTy, unsigned location, Value *locOffset, Value *compIdx,
                                       Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx && vertexIdx);

  auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, location, locOffset, compIdx, vertexIdx, builder);
  return readValueFromLds(false, inputTy, ldsOffset, builder);
}

// =====================================================================================================================
// Reads generic inputs of tessellation evaluation shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readTesGenericInput(Type *inputTy, unsigned location, Value *locOffset, Value *compIdx,
                                       Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx);

  auto ldsOffset = calcLdsOffsetForTesInput(inputTy, location, locOffset, compIdx, vertexIdx, builder);
  return readValueFromLds(true, inputTy, ldsOffset, builder);
}

// =====================================================================================================================
// Reads generic inputs of geometry shader.
//
// @param inputTy : Type of input value
// @param location : Location of the input
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readGsGenericInput(Type *inputTy, unsigned location, unsigned compIdx, Value *vertexIdx,
                                      BuilderBase &builder) {
  assert(vertexIdx);

  const unsigned compCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  const unsigned bitWidth = inputTy->getScalarSizeInBits();

  Type *origInputTy = inputTy;

  if (bitWidth == 64) {
    compIdx *= 2; // For 64-bit data type, the component indexing must multiply by 2

    // Cast 64-bit data type to float vector
    inputTy = FixedVectorType::get(builder.getFloatTy(), compCount * 2);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  Value *input = loadValueFromEsGsRing(inputTy, location, compIdx, vertexIdx, builder);

  if (inputTy != origInputTy) {
    // Cast back to original input type
    assert(canBitCast(inputTy, origInputTy));
    assert(inputTy->isVectorTy());

    input = builder.CreateBitCast(input, origInputTy);
  }

  return input;
}

// =====================================================================================================================
// Translate float type interpolation into corresponding LLVM intrinsics
//
// @param builder : The IR builder to create and insert IR instruction
// @param attr : The attribute location to access
// @param channel: The specific attribute channel to access
// @param coordI: Value of I coordinate
// @param coordJ: Value of J coordinate
// @param primMask: Value to fill into m0 register
Value *LowerInOut::performFsFloatInterpolation(BuilderBase &builder, Value *attr, Value *channel, Value *coordI,
                                               Value *coordJ, Value *primMask) {
  Value *result = nullptr;
  if (m_gfxIp.major >= 11) {
    // llvm.amdgcn.lds.param.load(attr_channel, attr, m0)
    Value *param =
        builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_lds_param_load, {channel, attr, primMask});

    // tmp = llvm.amdgcn.interp.inreg.p10(p10, coordI, p0)
    result = builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_interp_inreg_p10, {param, coordI, param});

    // llvm.amdgcn.interp.inreg.p2(p20, coordJ, tmp)
    result = builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_interp_inreg_p2, {param, coordJ, result});
  } else {
    // llvm.amdgcn.interp.p1(coordI, attr_channel, attr, m0)
    result =
        builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_interp_p1, {coordI, channel, attr, primMask});

    // llvm.amdgcn.interp.p2(p1, coordJ, attr_channel, attr, m0)
    result = builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_interp_p2,
                                     {result, coordJ, channel, attr, primMask});
  }
  return result;
}

// =====================================================================================================================
// Translate half type interpolation into corresponding LLVM intrinsics
//
// @param builder : The IR builder to create and insert IR instruction
// @param attr : The attribute location to access
// @param channel: The specific attribute channel to access
// @param coordI: Value of I coordinate
// @param coordJ: Value of J coordinate
// @param primMask: Value to fill into m0 register
// @param highHalf : Whether it is a high half in a 16-bit attribute
Value *LowerInOut::performFsHalfInterpolation(BuilderBase &builder, Value *attr, Value *channel, Value *coordI,
                                              Value *coordJ, Value *primMask, Value *highHalf) {
  Value *result = nullptr;
  if (m_gfxIp.major >= 11) {
    // llvm.amdgcn.lds.param.load(attr_channel, attr, m0)
    Value *param =
        builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_lds_param_load, {channel, attr, primMask});

    auto interpP10Intrinsic = Intrinsic::amdgcn_interp_p10_rtz_f16;
    auto interpP2Intrinsic = Intrinsic::amdgcn_interp_p2_rtz_f16;
    // tmp = interp.p10(p10, coordI, p0, highHalf)
    result = builder.CreateIntrinsic(builder.getFloatTy(), interpP10Intrinsic, {param, coordI, param, highHalf});

    // interp.p2(p20, coordJ, tmp, highHalf)
    result = builder.CreateIntrinsic(builder.getHalfTy(), interpP2Intrinsic, {param, coordJ, result, highHalf});
  } else {
    // llvm.amdgcn.interp.p1.f16(coordI, attr_channel, attr, highhalf, m0)
    result = builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_interp_p1_f16,
                                     {coordI, channel, attr, highHalf, primMask});

    // llvm.amdgcn.interp.p2.f16(p1, coordJ, attr_channel, attr, highhalf, m0)
    result = builder.CreateIntrinsic(builder.getHalfTy(), Intrinsic::amdgcn_interp_p2_f16,
                                     {result, coordJ, channel, attr, highHalf, primMask});
  }
  return result;
}

// =====================================================================================================================
// Load a specified FS parameter (used under flat/custom interpolation).
//
// @param builder : The IR builder to create and insert IR instruction
// @param attr : The attribute location to access
// @param channel : The specific attribute channel to access
// @param interpParam : The parameter to load
// @param primMask : Value to fill into m0 register
// @param bitWidth : The bitwidth of required data type
// @param highHalf : Whether it is a high half in a 16-bit attribute
Value *LowerInOut::performFsParameterLoad(BuilderBase &builder, Value *attr, Value *channel, InterpParam interpParam,
                                          Value *primMask, unsigned bitWidth, bool highHalf) {
  Value *compValue = nullptr;

  if (m_gfxIp.major >= 11) {
    // llvm.amdgcn.lds.param.load(attr_channel, attr, m0)
    compValue =
        builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_lds_param_load, {channel, attr, primMask});
    DppCtrl dppCtrl;
    if (interpParam == INTERP_PARAM_P0)
      dppCtrl = DppCtrl::DppQuadPerm0000;
    else if (interpParam == INTERP_PARAM_P10)
      dppCtrl = DppCtrl::DppQuadPerm1111;
    else
      dppCtrl = DppCtrl::DppQuadPerm2222;

    compValue = builder.CreateBitCast(compValue, builder.getInt32Ty());
    compValue = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
                                        {compValue, builder.getInt32(static_cast<unsigned>(dppCtrl)),
                                         builder.getInt32(15), builder.getInt32(15), builder.getTrue()});
    // NOTE: mov_dpp must run in strict WQM to access lanes potentially inactive with normal exec/WQM.
    // lds_param_load always runs in strict WQM, but exec/WQM may not match this due to discards or divergence.
    // Ideally we would use the FI bit on the mov_dpp, but there is currently no backend support.
    compValue = builder.CreateIntrinsic(Intrinsic::amdgcn_strict_wqm, builder.getInt32Ty(), compValue);
    compValue = builder.CreateBitCast(compValue, builder.getFloatTy());
  } else {
    Value *args[] = {
        builder.getInt32(interpParam), // param
        channel,                       // attr_chan
        attr,                          // attr
        primMask                       // m0
    };
    compValue = builder.CreateIntrinsic(builder.getFloatTy(), Intrinsic::amdgcn_interp_mov, args);
  }
  // Two int8s are also packed like 16-bit in a 32-bit channel in previous export stage
  if (bitWidth == 8 || bitWidth == 16) {
    compValue = builder.CreateBitCast(compValue, builder.getInt32Ty());

    if (highHalf)
      compValue = builder.CreateLShr(compValue, 16);

    if (bitWidth == 8) {
      compValue = builder.CreateTrunc(compValue, builder.getInt8Ty());
    } else {
      compValue = builder.CreateTrunc(compValue, builder.getInt16Ty());
      compValue = builder.CreateBitCast(compValue, builder.getHalfTy());
    }
  }

  return compValue;
}

// =====================================================================================================================
// Reads generic inputs of fragment shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing (could be null)
// @param isPerPrimitive : Whether the input is per-primitive
// @param interpMode : Interpolation mode
// @param interpValue : interpolation value: for "smooth" mode, holds the I,J coordinates; for "custom" mode, holds the
// vertex index; unused for "flat" mode or if the input is per-primitive
// @param highHalf : Whether it is a high half in a 16-bit attribute
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readFsGenericInput(Type *inputTy, unsigned location, Value *locOffset, Value *compIdx,
                                      bool isPerPrimitive, unsigned interpMode, Value *interpValue, bool highHalf,
                                      BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment);
  auto &interpInfo = resUsage->inOutUsage.fs.interpInfo;

  // NOTE: For per-primitive input, the specified location is still per-primitive based. To import the input value, we
  // have to adjust it by adding the total number of per-vertex inputs since per-vertex exports/imports are prior to
  // per-primitive ones.
  if (isPerPrimitive) {
    auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->inOutUsage;
    location += inOutUsage.inputMapLocCount;
  }

  const unsigned locCount = inputTy->getPrimitiveSizeInBits() / 8 > SizeOfVec4 ? 2 : 1;
  while (interpInfo.size() <= location + locCount - 1)
    interpInfo.push_back(InvalidFsInterpInfo);
  // Set the fields of FsInterpInfo except attr1Valid at location when it is not a high half
  if (!highHalf) {
    auto &interpInfoAtLoc = interpInfo[location];
    interpInfoAtLoc.loc = location;
    interpInfoAtLoc.flat = interpMode == InOutInfo::InterpModeFlat;
    interpInfoAtLoc.custom = interpMode == InOutInfo::InterpModeCustom;
    interpInfoAtLoc.is16bit = inputTy->getScalarSizeInBits() == 16;
    interpInfoAtLoc.attr0Valid = true;
    interpInfoAtLoc.isPerPrimitive = isPerPrimitive;
  } else {
    // attr1Valid is false by default and set it true when it is really a high half
    interpInfo[location].attr1Valid = true;
  }

  if (locCount > 1) {
    // The input occupies two consecutive locations
    assert(locCount == 2);
    interpInfo[location + 1] = {
        location + 1,
        (interpMode == InOutInfo::InterpModeFlat),
        (interpMode == InOutInfo::InterpModeCustom),
        false,
        false,
        false,
        isPerPrimitive,
    };
  }

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);
  Value *coordI = nullptr;
  Value *coordJ = nullptr;

  // Not "flat" and "custom" interpolation
  if (interpMode == InOutInfo::InterpModeSmooth) {
    coordI = builder.CreateExtractElement(interpValue, uint64_t(0));
    coordJ = builder.CreateExtractElement(interpValue, 1);
  }

  Type *basicTy = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getElementType() : inputTy;

  const unsigned compCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  const unsigned bitWidth = inputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  const unsigned numChannels = (bitWidth == 64 ? 2 : 1) * compCount;

  Type *interpTy = nullptr;
  if (bitWidth == 8) {
    assert(inputTy->isIntOrIntVectorTy());
    interpTy = builder.getInt8Ty();
  } else if (bitWidth == 16)
    interpTy = builder.getHalfTy();
  else
    interpTy = builder.getFloatTy();
  if (numChannels > 1)
    interpTy = FixedVectorType::get(interpTy, numChannels);
  Value *interp = PoisonValue::get(interpTy);

  unsigned startChannel = 0;
  if (compIdx) {
    startChannel = cast<ConstantInt>(compIdx)->getZExtValue();
    if (bitWidth == 64) {
      // NOTE: For 64-bit input, the component index is always 64-bit based while subsequent interpolation operations
      // is dword-based. We have to change the start channel accordingly.
      startChannel *= 2;
    }
    assert((startChannel + numChannels) <= (bitWidth == 64 ? 8 : 4));
  }

  if (locOffset)
    location += cast<ConstantInt>(locOffset)->getZExtValue();

  Value *loc = builder.getInt32(location);

  for (unsigned i = startChannel; i < startChannel + numChannels; ++i) {
    Value *compValue = nullptr;

    if (i == 4)
      loc = builder.getInt32(location + 1);

    if (interpMode == InOutInfo::InterpModeSmooth) {
      assert((basicTy->isHalfTy() || basicTy->isFloatTy()) && numChannels <= 4);
      (void(basicTy)); // unused

      if (bitWidth == 16) {
        compValue = performFsHalfInterpolation(builder, loc, builder.getInt32(i), coordI, coordJ, primMask,
                                               builder.getInt1(highHalf));

      } else {
        compValue = performFsFloatInterpolation(builder, loc, builder.getInt32(i), coordI, coordJ, primMask);
      }
    } else {
      InterpParam interpParam = INTERP_PARAM_P0;

      if (interpMode == InOutInfo::InterpModeCustom) {
        assert(isa<ConstantInt>(interpValue));
        unsigned vertexNo = cast<ConstantInt>(interpValue)->getZExtValue();

        switch (vertexNo) {
        case 0:
          interpParam = INTERP_PARAM_P0;
          break;
        case 1:
          interpParam = INTERP_PARAM_P10;
          break;
        case 2:
          interpParam = INTERP_PARAM_P20;
          break;
        default:
          llvm_unreachable("Should never be called!");
          break;
        }
      } else {
        assert(interpMode == InOutInfo::InterpModeFlat);
      }

      compValue =
          performFsParameterLoad(builder, loc, builder.getInt32(i % 4), interpParam, primMask, bitWidth, highHalf);
    }

    if (numChannels == 1)
      interp = compValue;
    else
      interp = builder.CreateInsertElement(interp, compValue, i - startChannel);
  }

  // Store interpolation results to inputs
  Value *input;
  if (interpTy == inputTy) {
    input = interp;
  } else {
    assert(canBitCast(interpTy, inputTy));
    input = builder.CreateBitCast(interp, inputTy);
  }

  return input;
}

// =====================================================================================================================
// Reads generic outputs of tessellation control shader.
//
// @param outputTy : Type of output value
// @param location : Base location of the output
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readTcsGenericOutput(Type *outputTy, unsigned location, Value *locOffset, Value *compIdx,
                                        Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx);
  auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, location, locOffset, compIdx, vertexIdx, builder);
  return readValueFromLds(false, outputTy, ldsOffset, builder);
}

// =====================================================================================================================
// Writes generic outputs of vertex shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeVsGenericOutput(Value *output, unsigned location, unsigned compIdx, BuilderBase &builder) {
  auto outputTy = output->getType();

  if (m_hasTs) {
    auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, location, compIdx, builder);
    writeValueToLds(false, output, ldsOffset, builder);
  } else {
    if (m_hasGs) {
      assert(outputTy->isIntOrIntVectorTy() || outputTy->isFPOrFPVectorTy());

      const unsigned bitWidth = outputTy->getScalarSizeInBits();
      if (bitWidth == 64) {
        // For 64-bit data type, the component indexing must multiply by 2
        compIdx *= 2;

        unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() * 2 : 2;

        outputTy = FixedVectorType::get(builder.getFloatTy(), compCount);
        output = builder.CreateBitCast(output, outputTy);
      } else
        assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

      storeValueToEsGsRing(output, location, compIdx, builder);
    } else
      addExportInstForGenericOutput(output, location, compIdx, builder);
  }
}

// =====================================================================================================================
// Writes generic outputs of tessellation control shader.
//
// @param output : Output value
// @param location : Base location of the output
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeTcsGenericOutput(Value *output, unsigned location, Value *locOffset, Value *compIdx,
                                       Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx);
  auto ldsOffset = calcLdsOffsetForTcsOutput(output->getType(), location, locOffset, compIdx, vertexIdx, builder);
  writeValueToLds(false, output, ldsOffset, builder);
}

// =====================================================================================================================
// Writes generic outputs of tessellation evaluation shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeTesGenericOutput(Value *output, unsigned location, unsigned compIdx, BuilderBase &builder) {
  if (m_hasGs) {
    auto outputTy = output->getType();
    assert(outputTy->isIntOrIntVectorTy() || outputTy->isFPOrFPVectorTy());

    const unsigned bitWidth = outputTy->getScalarSizeInBits();
    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx *= 2;

      unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() * 2 : 2;
      outputTy = FixedVectorType::get(builder.getFloatTy(), compCount);

      output = builder.CreateBitCast(output, outputTy);
    } else
      assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

    storeValueToEsGsRing(output, location, compIdx, builder);
  } else
    addExportInstForGenericOutput(output, location, compIdx, builder);
}

// =====================================================================================================================
// Writes generic outputs of geometry shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param streamId : ID of output vertex stream
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeGsGenericOutput(Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                                      BuilderBase &builder) {
  auto outputTy = output->getType();

  // Cast double or double vector to float vector.
  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  if (bitWidth == 64) {
    // For 64-bit data type, the component indexing must multiply by 2
    compIdx *= 2;

    if (outputTy->isVectorTy())
      outputTy = FixedVectorType::get(builder.getFloatTy(), cast<FixedVectorType>(outputTy)->getNumElements() * 2);
    else
      outputTy = FixedVectorType::get(builder.getFloatTy(), 2);

    output = builder.CreateBitCast(output, outputTy);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word to dword and
  // store dword to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size is based on number of dwords.

  assert(compIdx <= 4);

  storeValueToGsVsRing(output, location, compIdx, streamId, builder);
}

// =====================================================================================================================
// Writes generic outputs of mesh shader.
//
// @param output : Output value
// @param location : Base location of the output
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexOrPrimitiveIdx : Input array outermost index used for vertex or primitive indexing
// @param isPerPrimitive : Whether the output is per-primitive
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeMeshGenericOutput(Value *output, unsigned location, Value *locOffset, Value *compIdx,
                                        Value *vertexOrPrimitiveIdx, bool isPerPrimitive, BuilderBase &builder) {
  if (output->getType()->getScalarSizeInBits() == 64)
    compIdx = builder.CreateShl(compIdx, 1);

  builder.create<WriteMeshOutputOp>(isPerPrimitive, location, locOffset, compIdx, vertexOrPrimitiveIdx, output);
}

// =====================================================================================================================
// Reads built-in inputs of tessellation control shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readTcsBuiltInInput(Type *inputTy, unsigned builtInId, Value *elemIdx, Value *vertexIdx,
                                       BuilderBase &builder) {
  Value *input = PoisonValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::TessControl)->entryArgIdxs.tcs;
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl);
  const auto &inoutUsage = resUsage->inOutUsage;
  const auto &builtInInLocMap = inoutUsage.builtInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
    input = readValueFromLds(false, inputTy, ldsOffset, builder);

    break;
  }
  case BuiltInPointSize:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, nullptr, vertexIdx, builder);
    input = readValueFromLds(false, inputTy, ldsOffset, builder);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    if (!elemIdx) {
      // gl_ClipDistanceIn[]/gl_CullDistanceIn[] is treated as 2 x vec4
      assert(inputTy->isArrayTy());

      auto elemTy = inputTy->getArrayElementType();
      for (unsigned i = 0; i < inputTy->getArrayNumElements(); ++i) {
        auto elemIdx = builder.getInt32(i);
        auto ldsOffset = calcLdsOffsetForTcsInput(elemTy, loc, nullptr, elemIdx, vertexIdx, builder);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, builder);
        input = builder.CreateInsertValue(input, elem, i);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      input = readValueFromLds(false, inputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInPatchVertices: {
    input = builder.getInt32(m_pipelineState->getNumPatchControlPoints());
    break;
  }
  case BuiltInPrimitiveId: {
    input = m_pipelineSysValues.get(m_entryPoint)->getPrimitiveId();
    break;
  }
  case BuiltInInvocationId: {
    input = m_pipelineSysValues.get(m_entryPoint)->getInvocationId();
    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      if (m_pipelineState->getShaderOptions(m_shaderStage.value()).viewIndexFromDeviceIndex) {
        input = builder.getInt32(m_pipelineState->getDeviceIndex());
      } else {
        input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);
      }
    } else
      input = builder.getInt32(0);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  return input;
}

// =====================================================================================================================
// Reads built-in inputs of tessellation evaluation shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readTesBuiltInInput(Type *inputTy, unsigned builtInId, Value *elemIdx, Value *vertexIdx,
                                       BuilderBase &builder) {
  Value *input = PoisonValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::TessEval)->entryArgIdxs.tes;

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessEval);
  const auto &inOutUsage = resUsage->inOutUsage;
  const auto &builtInInLocMap = inOutUsage.builtInInputLocMap;
  const auto &perPatchBuiltInInLocMap = inOutUsage.perPatchBuiltInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
    input = readValueFromLds(true, inputTy, ldsOffset, builder);

    break;
  }
  case BuiltInPointSize:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, nullptr, vertexIdx, builder);
    input = readValueFromLds(true, inputTy, ldsOffset, builder);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    if (!elemIdx) {
      // gl_ClipDistanceIn[]/gl_CullDistanceIn[] is treated as 2 x vec4
      assert(inputTy->isArrayTy());

      auto elemTy = inputTy->getArrayElementType();
      for (unsigned i = 0; i < inputTy->getArrayNumElements(); ++i) {
        auto elemIdx = builder.getInt32(i);
        auto ldsOffset = calcLdsOffsetForTesInput(elemTy, loc, nullptr, elemIdx, vertexIdx, builder);
        auto elem = readValueFromLds(true, elemTy, ldsOffset, builder);
        input = builder.CreateInsertValue(input, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      input = readValueFromLds(true, inputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInPatchVertices: {
    unsigned patchVertices = MaxTessPatchVertices;
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStage::TessControl);
    if (hasTcs)
      patchVertices = m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;

    input = builder.getInt32(patchVertices);

    break;
  }
  case BuiltInPrimitiveId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.patchId);
    break;
  }
  case BuiltInTessCoord: {
    auto tessCoord = m_pipelineSysValues.get(m_entryPoint)->getTessCoord();

    if (elemIdx)
      input = builder.CreateExtractElement(tessCoord, elemIdx);
    else
      input = tessCoord;

    break;
  }
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner: {
    assert(perPatchBuiltInInLocMap.find(builtInId) != perPatchBuiltInInLocMap.end());
    unsigned loc = perPatchBuiltInInLocMap.find(builtInId)->second;

    if (!elemIdx) {
      // gl_TessLevelOuter[4] is treated as vec4
      // gl_TessLevelInner[2] is treated as vec2
      assert(inputTy->isArrayTy());

      auto elemTy = inputTy->getArrayElementType();
      for (unsigned i = 0; i < inputTy->getArrayNumElements(); ++i) {
        auto elemIdx = builder.getInt32(i);
        auto ldsOffset = calcLdsOffsetForTesInput(elemTy, loc, nullptr, elemIdx, vertexIdx, builder);
        auto elem = readValueFromLds(true, elemTy, ldsOffset, builder);
        input = builder.CreateInsertValue(input, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      input = readValueFromLds(true, inputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      if (m_pipelineState->getShaderOptions(m_shaderStage.value()).viewIndexFromDeviceIndex) {
        input = builder.getInt32(m_pipelineState->getDeviceIndex());
      } else {
        input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);
      }
    } else
      input = builder.getInt32(0);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  return input;
}

// =====================================================================================================================
// Reads built-in inputs of geometry shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readGsBuiltInInput(Type *inputTy, unsigned builtInId, Value *vertexIdx, BuilderBase &builder) {
  Value *input = nullptr;

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs;
  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize:
  case BuiltInClipDistance:
  case BuiltInCullDistance:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    assert(inOutUsage.builtInInputLocMap.find(builtInId) != inOutUsage.builtInInputLocMap.end());
    const unsigned loc = inOutUsage.builtInInputLocMap.find(builtInId)->second;
    assert(loc != InvalidValue);
    input = loadValueFromEsGsRing(inputTy, loc, 0, vertexIdx, builder);
    break;
  }
  case BuiltInPrimitiveId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.primitiveId);
    break;
  }
  case BuiltInInvocationId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.invocationId);
    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      if (m_pipelineState->getShaderOptions(m_shaderStage.value()).viewIndexFromDeviceIndex) {
        input = builder.getInt32(m_pipelineState->getDeviceIndex());
      } else {
        input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);
      }
    } else
      input = builder.getInt32(0);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  return input;
}

// =====================================================================================================================
// Reads built-in inputs of mesh shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readMeshBuiltInInput(Type *inputTy, unsigned builtInId, BuilderBase &builder) {
  // Handle work group size built-in
  if (builtInId == BuiltInWorkgroupSize) {
    // WorkgroupSize is a constant vector supplied by mesh shader mode.
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    return ConstantVector::get({builder.getInt32(meshMode.workgroupSizeX), builder.getInt32(meshMode.workgroupSizeY),
                                builder.getInt32(meshMode.workgroupSizeZ)});
  }

  // Handle other built-ins
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh)->builtInUsage.mesh;
  (void(builtInUsage)); // Unused

  switch (builtInId) {
  case BuiltInDrawIndex:
    assert(builtInUsage.drawIndex);
    break;
  case BuiltInViewIndex:
    assert(builtInUsage.viewIndex);
    break;
  case BuiltInNumWorkgroups:
    assert(builtInUsage.numWorkgroups);
    break;
  case BuiltInWorkgroupId:
    assert(builtInUsage.workgroupId);
    break;
  case BuiltInLocalInvocationId:
    assert(builtInUsage.localInvocationId);
    break;
  case BuiltInGlobalInvocationId:
    assert(builtInUsage.globalInvocationId);
    break;
  case BuiltInLocalInvocationIndex:
    assert(builtInUsage.localInvocationIndex);
    break;
  case BuiltInSubgroupId:
    assert(builtInUsage.subgroupId);
    break;
  case BuiltInNumSubgroups:
    assert(builtInUsage.numSubgroups);
    break;
  default:
    llvm_unreachable("Unknown mesh shader built-in!");
    break;
  }

  return builder.create<GetMeshBuiltinInputOp>(inputTy, builtInId);
}

// =====================================================================================================================
// Reads built-in inputs of fragment shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param generalVal : Sample ID, only needed for BuiltInSamplePosOffset; InterpLoc, only needed for BuiltInBaryCoord
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readFsBuiltInInput(Type *inputTy, unsigned builtInId, Value *generalVal, BuilderBase &builder) {
  Value *input = PoisonValue::get(inputTy);

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->inOutUsage;

  switch (builtInId) {
  case BuiltInSampleMask: {
    assert(inputTy->isArrayTy());

    auto sampleCoverage = getFunctionArgument(m_entryPoint, entryArgIdxs.sampleCoverage);
    auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

    // gl_SampleID = Ancillary[11:8]
    auto sampleId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                            {ancillary, builder.getInt32(8), builder.getInt32(4)});

    Value *sampleMaskIn = sampleCoverage;

    // RunAtSampleRate is used to identify whether fragment shader run at sample rate, which will
    // be set from API side. PixelShaderSamples is used to controls the pixel shader execution rate,
    // which will be set when compile shader.
    // There is a special case when vkCreateGraphicsPipelines but not set sampleRate, but compiling shader
    // will ask to set runAtSampleRate, this case is valid but current code will cause hang.
    // So in this case, it will not use broadcast sample mask.
    if (!m_pipelineState->getOptions().disableSampleCoverageAdjust &&
        (m_pipelineState->getRasterizerState().perSampleShading || builtInUsage.runAtSampleRate)) {
      unsigned baseMask = 1;
      if (!builtInUsage.sampleId) {
        if (m_pipelineState->getRasterizerState().pixelShaderSamples != 0) {
          // Only broadcast sample mask when the value has already been set
          // Fix the failure for multisample_shader_builtin.sample_mask cases "gl_SampleMaskIn" should contain one
          // or multiple covered sample bit.
          // (1) If the 4 samples is divided into 2 sub invocation groups, broadcast sample mask bit <0, 1>
          // to sample <2, 3>.
          // (2) If the 8 samples is divided into 2 sub invocation groups, broadcast sample mask bit <0, 1>
          // to sample <2, 3>, then re-broadcast sample mask bit <0, 1, 2, 3> to sample <4, 5, 6, 7>.
          // (3) If the 8 samples is divided into 4 sub invocation groups, patch to broadcast sample mask bit
          // <0, 1, 2, 3> to sample <4, 5, 6, 7>.
          unsigned baseMaskSamples = m_pipelineState->getRasterizerState().pixelShaderSamples;
          while (baseMaskSamples < m_pipelineState->getRasterizerState().numSamples) {
            baseMask |= baseMask << baseMaskSamples;
            baseMaskSamples *= 2;
          }
        }
      }

      // gl_SampleMaskIn[0] = (SampleCoverage & (baseMask << gl_SampleID))
      sampleMaskIn = builder.CreateShl(builder.getInt32(baseMask), sampleId);
      sampleMaskIn = builder.CreateAnd(sampleCoverage, sampleMaskIn);
    }

    // NOTE: Only gl_SampleMaskIn[0] is valid for us.
    input = builder.CreateInsertValue(input, sampleMaskIn, 0);

    break;
  }
  case BuiltInFragCoord: {
    Value *fragCoord[4] = {
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.x),
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.y),
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.z),
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.w),
    };

    if (m_pipelineState->getShaderModes()->getFragmentShaderMode().pixelCenterInteger) {
      fragCoord[0] = builder.CreateFSub(fragCoord[0], ConstantFP::get(builder.getFloatTy(), 0.5));
      fragCoord[1] = builder.CreateFSub(fragCoord[1], ConstantFP::get(builder.getFloatTy(), 0.5));
    }

    // Adjust gl_FragCoord.z value for the shading rate X,
    //
    // adjustedFragCoordZ = gl_FragCood.z + dFdxFine(gl_FragCood.z) * 1/16
    // adjustedFragCoordZ = gl_ShadingRate.x == 1? adjustedFragCoordZ : gl_FragCood.z
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waAdjustDepthImportVrs &&
        m_pipelineState->getShaderOptions(ShaderStage::Fragment).adjustDepthImportVrs) {
      const unsigned firstDppCtrl = 0xF5;  // FineX:   [0,1,2,3]->[1,1,3,3]
      const unsigned secondDppCtrl = 0xA0; // FineX:  [0,1,2,3]->[0,0,2,2]
      Value *fragCoordZAsInt = builder.CreateBitCast(fragCoord[2], builder.getInt32Ty());
      Value *firstDppValue = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
                                                     {fragCoordZAsInt, builder.getInt32(firstDppCtrl),
                                                      builder.getInt32(15), builder.getInt32(15), builder.getTrue()});
      firstDppValue = builder.CreateBitCast(firstDppValue, builder.getFloatTy());
      Value *secondDppValue = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
                                                      {fragCoordZAsInt, builder.getInt32(secondDppCtrl),
                                                       builder.getInt32(15), builder.getInt32(15), builder.getTrue()});
      secondDppValue = builder.CreateBitCast(secondDppValue, builder.getFloatTy());
      Value *adjustedFragCoordZ = builder.CreateFSub(firstDppValue, secondDppValue);
      adjustedFragCoordZ = builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, adjustedFragCoordZ, nullptr);
      Value *sixteenth = ConstantFP::get(builder.getFloatTy(), 1.0 / 16.0f);
      adjustedFragCoordZ =
          builder.CreateIntrinsic(Intrinsic::fma, builder.getFloatTy(), {adjustedFragCoordZ, sixteenth, fragCoord[2]});
      auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);
      Value *xRate = builder.CreateAnd(ancillary, 0xC);
      xRate = builder.CreateLShr(xRate, 2);
      // xRate = xRate == 0x1 ? Horizontal2Pixels : None
      auto xRate2Pixels = builder.CreateICmpEQ(xRate, builder.getInt32(1));
      adjustedFragCoordZ = builder.CreateSelect(xRate2Pixels, adjustedFragCoordZ, fragCoord[2]);
      fragCoord[2] = adjustedFragCoordZ;
    }

    if (!m_pipelineState->getShaderModes()->getFragmentShaderMode().noReciprocalFragCoordW)
      fragCoord[3] = builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_rcp, fragCoord[3]);

    for (unsigned i = 0; i < 4; ++i) {
      input = builder.CreateInsertElement(input, fragCoord[i], i);
    }

    break;
  }
  case BuiltInFrontFacing: {
    auto frontFacing = getFunctionArgument(m_entryPoint, entryArgIdxs.frontFacing);
    input = builder.CreateICmpNE(frontFacing, builder.getInt32(0));
    break;
  }
  case BuiltInPointCoord: {
    assert(inOutUsage.builtInInputLocMap.find(BuiltInPointCoord) != inOutUsage.builtInInputLocMap.end());
    const unsigned loc = inOutUsage.builtInInputLocMap[BuiltInPointCoord];

    // Emulation for "in vec2 gl_PointCoord"
    const unsigned builtInId =
        m_pipelineState->getRasterizerState().perSampleShading ? BuiltInInterpPerspSample : BuiltInInterpPerspCenter;
    Value *interpValue = readFsBuiltInInput(FixedVectorType::get(builder.getFloatTy(), 2), builtInId, nullptr, builder);
    input = readFsGenericInput(inputTy, loc, nullptr, nullptr, false, InOutInfo::InterpModeSmooth, interpValue, false,
                               builder);
    break;
  }
  case BuiltInHelperInvocation: {
#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
    input = builder.CreateIntrinsic(Intrinsic::amdgcn_ps_live, {});
#else
    input = builder.CreateIntrinsic(Intrinsic::amdgcn_ps_live, {}, {});
#endif
    input = builder.CreateNot(input);
    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      if (m_pipelineState->getShaderOptions(m_shaderStage.value()).viewIndexFromDeviceIndex) {
        input = builder.getInt32(m_pipelineState->getDeviceIndex());
      } else {
        input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewId);
      }
    } else
      input = builder.getInt32(0);
    break;
  }
  case BuiltInPrimitiveId:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    unsigned loc = InvalidValue;
    const auto prevStage = m_pipelineState->getPrevShaderStage(ShaderStage::Fragment);

    bool isPerPrimitive = false;
    if (prevStage == ShaderStage::Mesh) {
      assert(inOutUsage.perPrimitiveBuiltInInputLocMap.count(builtInId) > 0);
      loc = inOutUsage.perPrimitiveBuiltInInputLocMap[builtInId];
      // NOTE: If the previous shader stage is mesh shader, those built-ins are exported via primitive attributes.
      isPerPrimitive = true;
    } else {
      assert(inOutUsage.builtInInputLocMap.count(builtInId) > 0);
      loc = inOutUsage.builtInInputLocMap[builtInId];
    }

    // Emulation for "in int gl_PrimitiveID" or "in int gl_Layer" or "in int gl_ViewportIndex".
    input = readFsGenericInput(inputTy, loc, nullptr, nullptr, isPerPrimitive, InOutInfo::InterpModeFlat, nullptr,
                               false, builder);
    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    assert(inputTy->isArrayTy());

    unsigned loc = InvalidValue;
    unsigned locCount = 0;
    unsigned startChannel = 0;

    if (builtInId == BuiltInClipDistance) {
      assert(inOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != inOutUsage.builtInInputLocMap.end());
      loc = inOutUsage.builtInInputLocMap[BuiltInClipDistance];
      locCount = builtInUsage.clipDistance > 4 ? 2 : 1;
      startChannel = 0;
    } else {
      assert(builtInId == BuiltInCullDistance);

      assert(inOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInInputLocMap.end());
      loc = inOutUsage.builtInInputLocMap[BuiltInCullDistance];
      startChannel = builtInUsage.clipDistance % 4;
      locCount = startChannel + builtInUsage.cullDistance > 4 ? 2 : 1;
    }

    auto &interpInfo = inOutUsage.fs.interpInfo;
    while (interpInfo.size() <= loc + locCount - 1)
      interpInfo.push_back(InvalidFsInterpInfo);

    interpInfo[loc] = {loc, false, false};
    if (locCount > 1)
      interpInfo[loc + 1] = {loc + 1, false, false};

    // Emulation for "in float gl_ClipDistance[]" or "in float gl_CullDistance[]"
    auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);
    Value *ij = getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center);

    ij = builder.CreateBitCast(ij, FixedVectorType::get(builder.getFloatTy(), 2));
    auto coordI = builder.CreateExtractElement(ij, static_cast<uint64_t>(0));
    auto coordJ = builder.CreateExtractElement(ij, 1);

    const unsigned elemCount = inputTy->getArrayNumElements();
    assert(elemCount <= MaxClipCullDistanceCount);

    for (unsigned i = 0; i < elemCount; ++i) {
      auto compValue = performFsFloatInterpolation(builder, builder.getInt32(loc + (startChannel + i) / 4) /* attr */,
                                                   builder.getInt32((startChannel + i) % 4) /* attr_chan */, coordI,
                                                   coordJ, primMask);
      input = builder.CreateInsertValue(input, compValue, i);
    }

    break;
  }
  case BuiltInSampleId: {
    auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

    // gl_SampleID = Ancillary[11:8]
    input = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                    {ancillary, builder.getInt32(8), builder.getInt32(4)});

    break;
  }
  case BuiltInShadingRate: {
    // gl_ShadingRate is not supported on pre-GFX10.3
    assert(m_gfxIp >= GfxIpVersion({10, 3}));

    input = getShadingRate(builder);
    break;
  }
  case BuiltInPrimType: {
    input = getPrimType(builder);
    break;
  }
  case BuiltInLineStipple: {
    input = getLineStipple(builder);
    break;
  }
  case BuiltInPrimCoord: {
    assert(inOutUsage.builtInInputLocMap.find(BuiltInPrimCoord) != inOutUsage.builtInInputLocMap.end());
    const unsigned loc = inOutUsage.builtInInputLocMap[BuiltInPrimCoord];

    // Emulation for primCoord vGpr, specially, its value comes from z/w (ST) value, hence should be vec4 when interp.
    const unsigned builtInId =
        m_pipelineState->getRasterizerState().perSampleShading ? BuiltInInterpPerspSample : BuiltInInterpPerspCenter;
    Value *interpValue = readFsBuiltInInput(FixedVectorType::get(builder.getFloatTy(), 4), builtInId, nullptr, builder);
    Value *result = readFsGenericInput(FixedVectorType::get(builder.getFloatTy(), 4), loc, nullptr, nullptr, false,
                                       InOutInfo::InterpModeSmooth, interpValue, false, builder);
    input = PoisonValue::get(FixedVectorType::get(builder.getFloatTy(), 2));
    input = builder.CreateInsertElement(input, builder.CreateExtractElement(result, 2), builder.getInt32(0));
    input = builder.CreateInsertElement(input, builder.CreateExtractElement(result, 3), builder.getInt32(1));
    break;
  }
  // Handle internal-use built-ins for sample position emulation
  case BuiltInNumSamples: {
    if (m_pipelineState->isUnlinked() || m_pipelineState->getRasterizerState().dynamicSampleInfo) {
      assert(entryArgIdxs.compositeData != 0);
      auto sampleInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.compositeData);
      input = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                      {sampleInfo, builder.getInt32(2), builder.getInt32(5)});
    } else {
      assert(m_pipelineState->getRasterizerState().numSamples != 0);
      input = builder.getInt32(m_pipelineState->getRasterizerState().numSamples);
    }
    break;
  }
  case BuiltInSamplePatternIdx: {
    if (m_pipelineState->isUnlinked() || m_pipelineState->getRasterizerState().dynamicSampleInfo) {
      assert(entryArgIdxs.compositeData != 0);
      auto sampleInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.compositeData);
      Value *numSamples = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                                  {sampleInfo, builder.getInt32(2), builder.getInt32(5)});
      numSamples = builder.CreateBinaryIntrinsic(Intrinsic::cttz, numSamples, builder.getTrue());
      input = builder.CreateMul(
          numSamples, builder.getInt32(m_pipelineState->getTargetInfo().getGpuProperty().maxMsaaRasterizerSamples));
    } else {
      input = builder.getInt32(m_pipelineState->getRasterizerState().samplePatternIdx);
    }

    break;
  }
  // Handle internal-use built-ins for interpolation functions and AMD extension (AMD_shader_explicit_vertex_parameter)
  case BuiltInInterpPerspSample:
  case BuiltInBaryCoordSmoothSample: {
    assert(entryArgIdxs.perspInterp.sample != 0);
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.sample);
    break;
  }
  case BuiltInInterpPerspCenter:
  case BuiltInBaryCoordSmooth: {
    assert(entryArgIdxs.perspInterp.center != 0);
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.center);
    break;
  }
  case BuiltInInterpPerspCentroid:
  case BuiltInBaryCoordSmoothCentroid: {
    assert(entryArgIdxs.perspInterp.centroid != 0);
    input = adjustCentroidIj(getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.centroid),
                             getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.center), builder);
    break;
  }
  case BuiltInInterpPullMode:
  case BuiltInBaryCoordPullModel: {
    assert(entryArgIdxs.perspInterp.pullMode != 0);
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.pullMode);
    break;
  }
  case BuiltInInterpLinearSample:
  case BuiltInBaryCoordNoPerspSample: {
    assert(entryArgIdxs.linearInterp.sample != 0);
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.sample);
    break;
  }
  case BuiltInInterpLinearCenter:
  case BuiltInBaryCoordNoPersp: {
    assert(entryArgIdxs.linearInterp.center != 0);
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center);
    break;
  }
  case BuiltInInterpLinearCentroid:
  case BuiltInBaryCoordNoPerspCentroid: {
    assert(entryArgIdxs.linearInterp.centroid != 0);
    input = adjustCentroidIj(getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.centroid),
                             getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center), builder);
    break;
  }
  case BuiltInSamplePosOffset: {
    input = getSamplePosOffset(inputTy, generalVal, builder);
    break;
  }
  case BuiltInSamplePosition: {
    input = getSamplePosition(inputTy, builder);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  return input;
}

// =====================================================================================================================
// Generate code to read BuiltInSamplePosOffset
//
// @param inputTy : Type of BuiltInSamplePosOffset
// @param sampleId : Sample ID
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::getSamplePosOffset(Type *inputTy, Value *sampleId, BuilderBase &builder) {
  // Gets the offset of sample position relative to the pixel center for the specified sample ID
  Value *numSamples = readFsBuiltInInput(builder.getInt32Ty(), BuiltInNumSamples, nullptr, builder);
  Value *patternIdx = readFsBuiltInInput(builder.getInt32Ty(), BuiltInSamplePatternIdx, nullptr, builder);
  Value *validOffset = builder.CreateAdd(patternIdx, sampleId);
  // offset = (sampleCount > sampleId) ? (samplePatternOffset + sampleId) : 0
  Value *sampleValid = builder.CreateICmpUGT(numSamples, sampleId);
  Value *offset = builder.CreateSelect(sampleValid, validOffset, builder.getInt32(0));
  // Load sample position descriptor.
  Value *desc = m_pipelineSysValues.get(m_entryPoint)->loadDescFromDriverTable(SiDrvTableSamplepos, builder);
  // Load the value using the descriptor.
  offset = builder.CreateShl(offset, builder.getInt32(4));
  return builder.CreateIntrinsic(inputTy, Intrinsic::amdgcn_raw_buffer_load,
                                 {desc, offset, builder.getInt32(0), builder.getInt32(0)});
}

// =====================================================================================================================
// Generate code to read BuiltInSamplePosition
//
// @param inputTy : Type of BuiltInSamplePosition
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::getSamplePosition(Type *inputTy, BuilderBase &builder) {
  Value *sampleId = readFsBuiltInInput(builder.getInt32Ty(), BuiltInSampleId, nullptr, builder);
  Value *input = readFsBuiltInInput(inputTy, BuiltInSamplePosOffset, sampleId, builder);
  return builder.CreateFAdd(input, ConstantFP::get(inputTy, 0.5));
}

// =====================================================================================================================
// Reads built-in outputs of tessellation control shader.
//
// @param outputTy : Type of output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Output array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readTcsBuiltInOutput(Type *outputTy, unsigned builtInId, Value *elemIdx, Value *vertexIdx,
                                        BuilderBase &builder) {
  Value *output = PoisonValue::get(outputTy);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl);
  const auto &builtInUsage = resUsage->builtInUsage.tcs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
  const auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize: {
    assert(builtInId != BuiltInPosition || builtInUsage.position);
    assert(builtInId != BuiltInPointSize || (builtInUsage.pointSize && !elemIdx));
    (void(builtInUsage)); // unused

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
    output = readValueFromLds(false, outputTy, ldsOffset, builder);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    if (builtInId == BuiltInClipDistance) {
      assert(builtInUsage.clipDistance > 0);
      (void(builtInUsage)); // unused
    } else {
      assert(builtInId == BuiltInCullDistance);
      assert(builtInUsage.cullDistance > 0);
      (void(builtInUsage)); // unused
    }

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    if (!elemIdx) {
      // gl_ClipDistance[]/gl_CullDistance[] is treated as 2 x vec4
      assert(outputTy->isArrayTy());

      auto elemTy = outputTy->getArrayElementType();
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elemIdx = builder.getInt32(i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elemTy, loc, nullptr, elemIdx, vertexIdx, builder);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, builder);
        output = builder.CreateInsertValue(output, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      output = readValueFromLds(false, outputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner: {
    assert(builtInId != BuiltInTessLevelOuter || builtInUsage.tessLevelOuter);
    assert(builtInId != BuiltInTessLevelInner || builtInUsage.tessLevelInner);
    (void(builtInUsage)); // Unused

    assert(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
    unsigned loc = perPatchBuiltInOutLocMap.find(builtInId)->second;

    if (outputTy->isArrayTy()) {
      // Handle the whole array
      auto elemTy = outputTy->getArrayElementType();
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto ldsOffset = calcLdsOffsetForTcsOutput(elemTy, loc, nullptr, builder.getInt32(i), nullptr, builder);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, builder);
        output = builder.CreateInsertValue(output, elem, {i});
      }
    } else {
      // Handle a single element of the array
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, nullptr, builder);
      output = readValueFromLds(false, outputTy, ldsOffset, builder);
    }

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  return output;
}

// =====================================================================================================================
// Writes built-in outputs of vertex shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param builder : the builder to use
void LowerInOut::writeVsBuiltInOutput(Value *output, unsigned builtInId, BuilderBase &builder) {
  auto outputTy = output->getType();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Vertex);
  auto &builtInUsage = resUsage->builtInUsage.vs;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize: {
    if ((builtInId == BuiltInPosition && !builtInUsage.position) ||
        (builtInId == BuiltInPointSize && !builtInUsage.pointSize))
      return;

    if (builtInId == BuiltInPointSize && (isa<UndefValue>(output) || isa<PoisonValue>(output))) {
      // NOTE: gl_PointSize is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInOutLocMap.erase(BuiltInPointSize);
      builtInUsage.pointSize = false;
      return;
    }

    if (m_hasTs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, builder);
      writeValueToLds(false, output, ldsOffset, builder);
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap.find(builtInId)->second;

        storeValueToEsGsRing(output, loc, 0, builder);
      } else
        addExportInstForBuiltInOutput(output, builtInId, builder);
    }

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    if ((builtInId == BuiltInClipDistance && builtInUsage.clipDistance == 0) ||
        (builtInId == BuiltInCullDistance && builtInUsage.cullDistance == 0))
      return;

    if ((isa<UndefValue>(output) || isa<PoisonValue>(output))) {
      // NOTE: gl_{Clip,Cull}Distance[] is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      if (builtInId == BuiltInClipDistance) {
        builtInOutLocMap.erase(BuiltInClipDistance);
        builtInUsage.clipDistance = 0;
      } else {
        builtInOutLocMap.erase(BuiltInCullDistance);
        builtInUsage.cullDistance = 0;
      }
      return;
    }

    if (m_hasTs) {
      assert(outputTy->isArrayTy());

      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy->getArrayElementType(), loc, 0, builder);

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = builder.CreateExtractValue(output, {i});
        writeValueToLds(false, elem, ldsOffset, builder);

        ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(1));
      }
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap.find(builtInId)->second;

        storeValueToEsGsRing(output, loc, 0, builder);
      } else {
        // NOTE: The export of gl_{Clip,Cull}Distance[] is delayed and is done before entry-point returns.
        if (builtInId == BuiltInClipDistance)
          m_clipDistance = output;
        else
          m_cullDistance = output;
      }
    }

    break;
  }
  case BuiltInLayer: {
    if (!static_cast<bool>(builtInUsage.layer))
      return;

    // NOTE: Only last vertex processing shader stage has to export the value of gl_Layer.
    if (!m_hasTs && !m_hasGs) {
      // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
      m_layer = output;
    } else if (m_hasTs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, builder);
      writeValueToLds(false, output, ldsOffset, builder);
    } else if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, builder);
    }

    break;
  }
  case BuiltInViewportIndex: {
    if (!static_cast<bool>(builtInUsage.viewportIndex))
      return;

    // NOTE: Only last vertex processing shader stage has to export the value of gl_ViewportIndex.
    if (!m_hasTs && !m_hasGs) {
      // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
      m_viewportIndex = output;
    } else if (m_hasTs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, builder);
      writeValueToLds(false, output, ldsOffset, builder);
    } else if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, builder);
    }

    break;
  }
  case BuiltInPrimitiveShadingRate: {
    if (!static_cast<bool>(builtInUsage.primitiveShadingRate))
      return;

    // NOTE: Only last non-fragment shader stage has to export the value of gl_PrimitiveShadingRate.
    if (!m_hasTs && !m_hasGs) {
      // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
      assert(m_gfxIp >= GfxIpVersion({10, 3}));
      addExportInstForBuiltInOutput(output, builtInId, builder);
    }

    break;
  }
  case BuiltInEdgeFlag: {
    if (!m_hasTs && !m_hasGs) {
      m_edgeFlag = output;
    }
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Writes built-in outputs of tessellation control shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Output array outermost index used for vertex indexing (could be null)
// @param builder : the builder to use
void LowerInOut::writeTcsBuiltInOutput(Value *output, unsigned builtInId, Value *elemIdx, Value *vertexIdx,
                                       BuilderBase &builder) {
  auto outputTy = output->getType();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl);
  const auto &builtInUsage = resUsage->builtInUsage.tcs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
  const auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;
  const auto &hwConfig = resUsage->inOutUsage.tcs.hwConfig;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    if ((builtInId == BuiltInPosition && !builtInUsage.position) ||
        (builtInId == BuiltInPointSize && !builtInUsage.pointSize) ||
        (builtInId == BuiltInLayer && !builtInUsage.layer) ||
        (builtInId == BuiltInViewportIndex && !builtInUsage.viewportIndex))
      return;

    assert(builtInId != BuiltInPointSize || !elemIdx);

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
    writeValueToLds(false, output, ldsOffset, builder);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    if ((builtInId == BuiltInClipDistance && builtInUsage.clipDistance == 0) ||
        (builtInId == BuiltInCullDistance && builtInUsage.cullDistance == 0))
      return;

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    if (!elemIdx) {
      // gl_ClipDistance[]/gl_CullDistance[] is treated as 2 x vec4
      assert(outputTy->isArrayTy());

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = builder.CreateExtractValue(output, i);
        auto elemIdx = builder.getInt32(i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, elemIdx, vertexIdx, builder);
        writeValueToLds(false, elem, ldsOffset, builder);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      writeValueToLds(false, output, ldsOffset, builder);
    }

    break;
  }
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner: {
    if ((builtInId == BuiltInTessLevelOuter && builtInUsage.tessLevelOuter) ||
        (builtInId == BuiltInTessLevelInner && builtInUsage.tessLevelInner)) {
      unsigned loc = perPatchBuiltInOutLocMap.find(builtInId)->second;

      if (outputTy->isArrayTy()) {
        // Handle the whole array
        for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
          auto elem = builder.CreateExtractValue(output, {i});
          auto ldsOffset =
              calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, builder.getInt32(i), nullptr, builder);
          writeValueToLds(false, elem, ldsOffset, builder);
        }
      } else {
        // Handle a single element of the array
        auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, nullptr, builder);
        writeValueToLds(false, output, ldsOffset, builder);
      }
    }

    // Write TFs to the dedicated region of on-chip LDS for later HW TF buffer store (read by HW tessellator)
    unsigned numOuterTfs = 0;
    unsigned numInnerTfs = 0;
    unsigned numTfs = 0;

    const auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
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
    numTfs = (builtInId == BuiltInTessLevelOuter) ? numOuterTfs : numInnerTfs;

    auto relPatchId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

    // tessLevelOuter (numOuterTfs) + tessLevelInner (numInnerTfs)
    // ldsOffset = tessFactorStart + relPatchId * tessFactorStride + elemIdx
    uint32_t tessOffset = 0;
    if (builtInId == BuiltInTessLevelInner)
      tessOffset += numOuterTfs;

    Value *baseOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.onChip.tessFactorStride));
    baseOffset = builder.CreateAdd(baseOffset, builder.getInt32(hwConfig.onChip.tessFactorStart));

    if (outputTy->isArrayTy()) {
      // Handle the whole array, skip irrelevant TFs
      for (unsigned i = 0; i < numTfs; ++i) {
        Value *ldsOffset = builder.CreateAdd(baseOffset, builder.getInt32(tessOffset + i));
        auto elem = builder.CreateExtractValue(output, {i});
        writeValueToLds(false, elem, ldsOffset, builder);
      }
    } else {
      // Handle a single element of the array
      Value *ldsOffset = builder.CreateAdd(baseOffset, builder.getInt32(tessOffset));
      if (isa<ConstantInt>(elemIdx)) {
        // Skip irrelevant TFs
        if (cast<ConstantInt>(elemIdx)->getZExtValue() < numTfs) {
          ldsOffset = builder.CreateAdd(ldsOffset, elemIdx);
          writeValueToLds(false, output, ldsOffset, builder);
        }
      } else {
        // NOTE: We use odd-dword stride to avoid LDS bank conflict. Since the number of TFs is always even, the last
        // TF slot is unused. We can reuse it to store irrelevant TFs.
        assert(numOuterTfs + numInnerTfs + 1 == hwConfig.onChip.tessFactorStride);
        unsigned invalidElemIdx = hwConfig.onChip.tessFactorStride - 1;

        // elemIdx = elemIdx < numTfs ? elemIdx : invalidElemIdx
        auto relevantTf = builder.CreateICmpULT(elemIdx, builder.getInt32(numTfs));
        elemIdx = builder.CreateSelect(relevantTf, elemIdx, builder.getInt32(invalidElemIdx));
        ldsOffset = builder.CreateAdd(ldsOffset, elemIdx);
        writeValueToLds(false, output, ldsOffset, builder);
      }
    }

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Writes built-in outputs of tessellation evaluation shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param builder : the builder to use
void LowerInOut::writeTesBuiltInOutput(Value *output, unsigned builtInId, BuilderBase &builder) {
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessEval);
  auto &builtInUsage = resUsage->builtInUsage.tes;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize:
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    if ((builtInId == BuiltInPosition && !builtInUsage.position) ||
        (builtInId == BuiltInPointSize && !builtInUsage.pointSize) ||
        (builtInId == BuiltInClipDistance && builtInUsage.clipDistance == 0) ||
        (builtInId == BuiltInCullDistance && builtInUsage.cullDistance == 0))
      return;

    if ((isa<UndefValue>(output) || isa<PoisonValue>(output))) {
      // NOTE: gl_* builtins are always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      switch (builtInId) {
      case BuiltInPosition:
        builtInOutLocMap.erase(BuiltInPosition);
        builtInUsage.position = false;
        return;
      case BuiltInPointSize:
        builtInOutLocMap.erase(BuiltInPointSize);
        builtInUsage.pointSize = false;
        return;
      case BuiltInClipDistance:
        builtInOutLocMap.erase(BuiltInClipDistance);
        builtInUsage.clipDistance = 0;
        return;
      case BuiltInCullDistance:
        builtInOutLocMap.erase(BuiltInCullDistance);
        builtInUsage.cullDistance = 0;
        return;
      default:
        llvm_unreachable("unhandled builtInId");
      }
    }

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, builder);
    } else {
      switch (builtInId) {
      case BuiltInPosition:
      case BuiltInPointSize:
        addExportInstForBuiltInOutput(output, builtInId, builder);
        break;
      case BuiltInClipDistance:
        // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
        m_clipDistance = output;
        break;
      case BuiltInCullDistance:
        // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
        m_cullDistance = output;
        break;
      default:
        llvm_unreachable("unhandled builtInId");
      }
    }

    break;
  }
  case BuiltInLayer: {
    if (!static_cast<bool>(builtInUsage.layer))
      return;

    // NOTE: Only last vertex processing shader stage has to export the value of gl_Layer.
    if (!m_hasGs) {
      // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
      m_layer = output;
    } else {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, builder);
    }

    break;
  }
  case BuiltInViewportIndex: {
    if (!static_cast<bool>(builtInUsage.viewportIndex))
      return;

    // NOTE: Only last vertex processing shader stage has to export the value of gl_ViewportIndex.
    if (!m_hasGs) {
      // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
      m_viewportIndex = output;
    } else {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, builder);
    }

    break;
  }
  case BuiltInPrimitiveShadingRate: {
    if (!builtInUsage.primitiveShadingRate)
      return;

    // NOTE: Only last non-fragment shader stage has to export the value of gl_PrimitiveShadingRate.
    if (!m_hasGs) {
      // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
      assert(m_gfxIp >= GfxIpVersion({10, 3}));
      addExportInstForBuiltInOutput(output, builtInId, builder);
    }

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Writes built-in outputs of geometry shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param streamId : ID of output vertex stream
// @param builder : the builder to use
void LowerInOut::writeGsBuiltInOutput(Value *output, unsigned builtInId, unsigned streamId, BuilderBase &builder) {
  if (streamId != m_pipelineState->getRasterizerState().rasterStream)
    return; // Skip built-in export if this stream is not the rasterization stream.

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry);
  const auto &builtInUsage = resUsage->builtInUsage.gs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
  const unsigned loc = builtInOutLocMap.find(builtInId)->second;

  switch (builtInId) {
  case BuiltInPosition:
    assert(builtInUsage.position);
    break;
  case BuiltInPointSize:
    assert(builtInUsage.pointSize);
    break;
  case BuiltInClipDistance:
    assert(builtInUsage.clipDistance);
    break;
  case BuiltInCullDistance:
    assert(builtInUsage.cullDistance);
    break;
  case BuiltInPrimitiveId:
    assert(builtInUsage.primitiveId);
    break;
  case BuiltInLayer:
    assert(builtInUsage.layer);
    break;
  case BuiltInViewportIndex:
    assert(builtInUsage.viewportIndex);
    break;
  case BuiltInPrimitiveShadingRate:
    assert(builtInUsage.primitiveShadingRate);
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  (void(builtInUsage)); // unused
  storeValueToGsVsRing(output, loc, 0, streamId, builder);
}

// =====================================================================================================================
// Writes built-in outputs of mesh shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexOrPrimitiveIdx : Output array outermost index used for vertex or primitive indexing
// @param isPerPrimitive : Whether the output is per-primitive
// @param builder : the builder to use
void LowerInOut::writeMeshBuiltInOutput(Value *output, unsigned builtInId, Value *elemIdx, Value *vertexOrPrimitiveIdx,
                                        bool isPerPrimitive, BuilderBase &builder) {
  // Handle primitive indices built-ins
  if (builtInId == BuiltInPrimitivePointIndices || builtInId == BuiltInPrimitiveLineIndices ||
      builtInId == BuiltInPrimitiveTriangleIndices) {
    // Output primitive type must match primitive indices built-in
    auto outputPrimitive = m_pipelineState->getShaderModes()->getMeshShaderMode().outputPrimitive;
    assert((builtInId == BuiltInPrimitivePointIndices && outputPrimitive == OutputPrimitives::Points) ||
           (builtInId == BuiltInPrimitiveLineIndices && outputPrimitive == OutputPrimitives::Lines) ||
           (builtInId == BuiltInPrimitiveTriangleIndices && outputPrimitive == OutputPrimitives::Triangles));
    (void(outputPrimitive)); // Unused

    // Element indexing is forbidden. This is required by the spec that says "Each array element must be written as a
    // whole, partial writes to the vector components for line and triangle primitives is not allowed."
    assert(!elemIdx);

    builder.create<SetMeshPrimitiveIndicesOp>(vertexOrPrimitiveIdx, output);
    return;
  }

  // Handle cull primitive built-in
  if (builtInId == BuiltInCullPrimitive) {
    assert(isPerPrimitive);
    assert(output->getType()->isIntegerTy(1)); // Must be boolean
    builder.create<SetMeshPrimitiveCulledOp>(vertexOrPrimitiveIdx, output);
    return;
  }

  // Handle normal per-vertex or per-primitive built-ins
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Mesh);
  const auto &builtInUsage = resUsage->builtInUsage.mesh;
  unsigned loc = InvalidValue;

  if (isPerPrimitive) {
    switch (builtInId) {
    case BuiltInPrimitiveId:
      assert(builtInUsage.primitiveId);
      break;
    case BuiltInLayer:
      assert(builtInUsage.layer);
      break;
    case BuiltInViewportIndex:
      assert(builtInUsage.viewportIndex);
      break;
    case BuiltInPrimitiveShadingRate:
      assert(builtInUsage.primitiveShadingRate);
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }

    auto &perPrimitiveBuiltInOutputLocMap = resUsage->inOutUsage.perPrimitiveBuiltInOutputLocMap;
    assert(perPrimitiveBuiltInOutputLocMap.find(builtInId) != perPrimitiveBuiltInOutputLocMap.end());
    loc = perPrimitiveBuiltInOutputLocMap[builtInId];
  } else {
    switch (builtInId) {
    case BuiltInPosition:
      assert(builtInUsage.position);
      break;
    case BuiltInPointSize:
      assert(builtInUsage.pointSize);
      break;
    case BuiltInClipDistance:
      assert(builtInUsage.clipDistance);
      break;
    case BuiltInCullDistance:
      assert(builtInUsage.cullDistance);
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }

    auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    loc = builtInOutLocMap[builtInId];
  }

  (void(builtInUsage)); // Unused

  if (!elemIdx)
    elemIdx = builder.getInt32(0);

  builder.create<WriteMeshOutputOp>(isPerPrimitive, loc, builder.getInt32(0), elemIdx, vertexOrPrimitiveIdx, output);
}

// =====================================================================================================================
// Writes built-in outputs of fragment shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param builder : the builder to use
void LowerInOut::writeFsBuiltInOutput(Value *output, unsigned builtInId, BuilderBase &builder) {
  switch (builtInId) {
  case BuiltInFragDepth: {
    m_fragDepth = output;
    break;
  }
  case BuiltInSampleMask: {
    assert(output->getType()->isArrayTy());

    // NOTE: Only gl_SampleMask[0] is valid for us.
    m_sampleMask = builder.CreateExtractValue(output, 0);
    m_sampleMask = builder.CreateBitCast(m_sampleMask, builder.getFloatTy());
    break;
  }
  case BuiltInFragStencilRef: {
    m_fragStencilRef = builder.CreateBitCast(output, builder.getFloatTy());
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Writes built-in outputs of copy shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param builder : the builder to use
void LowerInOut::writeCopyShaderBuiltInOutput(Value *output, unsigned builtInId, BuilderBase &builder) {
  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize: {
    addExportInstForBuiltInOutput(output, builtInId, builder);
    break;
  }
  case BuiltInClipDistance: {
    // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
    m_clipDistance = output;
    break;
  }
  case BuiltInCullDistance: {
    // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
    m_cullDistance = output;
    break;
  }
  case BuiltInPrimitiveId: {
    // NOTE: The export of gl_PrimitiveID is delayed and is done before entry-point returns.
    m_primitiveId = output;
    break;
  }
  case BuiltInLayer: {
    // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
    m_layer = output;
    break;
  }
  case BuiltInViewIndex: {
    // NOTE: The export of gl_ViewIndex is delayed and is done before entry-point returns.
    m_viewIndex = output;
    break;
  }
  case BuiltInViewportIndex: {
    // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
    m_viewportIndex = output;
    break;
  }
  case BuiltInPrimitiveShadingRate: {
    // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
    assert(m_gfxIp >= GfxIpVersion({10, 3}));
    addExportInstForBuiltInOutput(output, builtInId, builder);

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Writes XFB outputs of vertex shader, tessellation evaluation shader, and copy shader.
//
// @param output : Output value
// @param xfbBuffer : Transform feedback buffer ID
// @param xfbOffset : Transform feedback offset
// @param streamId : Output stream ID
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeXfbOutput(Value *output, unsigned xfbBuffer, unsigned xfbOffset, unsigned streamId,
                                BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
         m_shaderStage == ShaderStage::CopyShader);

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  unsigned xfbStride = xfbStrides[xfbBuffer];

  auto outputTy = output->getType();
  unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;
  unsigned bitWidth = outputTy->getScalarSizeInBits();

  if (bitWidth == 64) {
    // Cast 64-bit output to 32-bit
    compCount *= 2;
    bitWidth = 32;
    outputTy = FixedVectorType::get(builder.getFloatTy(), compCount);
    output = builder.CreateBitCast(output, outputTy);
  }
  assert(bitWidth == 16 || bitWidth == 32);

  if (compCount == 8) {
    // vec8 -> vec4 + vec4
    assert(bitWidth == 32);

    Value *compX4 = builder.CreateShuffleVector(output, {0, 1, 2, 3});
    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamId, builder);

    compX4 = builder.CreateShuffleVector(output, {4, 5, 6, 7});
    xfbOffset += 4 * (bitWidth / 8);
    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamId, builder);
  } else if (compCount == 6) {
    // vec6 -> vec4 + vec2
    assert(bitWidth == 32);

    // NOTE: This case is generated by copy shader, which casts 64-bit outputs to float.
    Value *compX4 = builder.CreateShuffleVector(output, {0, 1, 2, 3});
    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamId, builder);

    Value *compX2 = builder.CreateShuffleVector(output, {4, 5});
    xfbOffset += 4 * (bitWidth / 8);
    storeValueToStreamOutBuffer(compX2, xfbBuffer, xfbOffset, xfbStride, streamId, builder);
  } else {
    // 16vec4, 16vec3, 16vec2, 16scalar
    // vec4, vec3, vec2, scalar
    if (outputTy->isVectorTy() && compCount == 1) {
      // NOTE: We translate vec1 to scalar. SPIR-V translated from DX has such usage.
      output = builder.CreateExtractElement(output, static_cast<uint64_t>(0));
    }

    storeValueToStreamOutBuffer(output, xfbBuffer, xfbOffset, xfbStride, streamId, builder);
  }
}

// =====================================================================================================================
// Combines scalar values store to vector store
//
// @param storeValues : Values to store
// @param startIdx : Starting index for load operation in the load value array
// @param valueOffset : Value offset as a bias of buffer store offset
// @param bufDesc : Buffer descriptor
// @param storeOffset : Buffer store offset
// @param bufBase : Buffer base offset
// @param coherent : Buffer coherency
// @param builder : The IR builder to create and insert IR instruction
unsigned LowerInOut::combineBufferStore(const std::vector<Value *> &storeValues, unsigned startIdx,
                                        unsigned valueOffset, Value *bufDesc, Value *storeOffset, Value *bufBase,
                                        CoherentFlag coherent, BuilderBase &builder) {
  Type *storeTys[4] = {
      builder.getInt32Ty(),
      FixedVectorType::get(builder.getInt32Ty(), 2),
      FixedVectorType::get(builder.getInt32Ty(), 3),
      FixedVectorType::get(builder.getInt32Ty(), 4),
  };

  std::string funcName = "llvm.amdgcn.raw.tbuffer.store.";

  // Start from 4-component combination
  unsigned compCount = 4;
  for (; compCount > 0; compCount--) {
    if (startIdx + compCount <= storeValues.size()) {
      funcName += getTypeName(storeTys[compCount - 1]);
      Value *storeValue = nullptr;
      if (compCount > 1) {
        auto storeTy = FixedVectorType::get(builder.getInt32Ty(), compCount);
        storeValue = PoisonValue::get(storeTy);

        for (unsigned i = 0; i < compCount; ++i) {
          storeValue = builder.CreateInsertElement(storeValue, storeValues[startIdx + i], i);
        }
      } else
        storeValue = storeValues[startIdx];

      auto writeOffset = builder.CreateAdd(storeOffset, builder.getInt32(valueOffset * 4));
      Value *args[] = {
          storeValue,                                        // vdata
          bufDesc,                                           // rsrc
          writeOffset,                                       // voffset
          bufBase,                                           // soffset
          builder.getInt32((*m_buffFormats)[compCount - 1]), // format
          builder.getInt32(coherent.u32All)                  // glc
      };
      builder.CreateNamedCall(funcName, Type::getVoidTy(*m_context), args, {});

      break;
    }
  }

  return compCount;
}

// =====================================================================================================================
// Combines scalar values load to vector load
//
// @param [in/out] loadValues : Values to load
// @param startIdx : Starting index for load operation in the load value array
// @param bufDesc : Buffer descriptor
// @param loadOffset : Buffer load offset
// @param bufBase : Buffer base offset
// @param coherent : Buffer coherency
// @param builder : The IR builder to create and insert IR instruction
unsigned LowerInOut::combineBufferLoad(std::vector<Value *> &loadValues, unsigned startIdx, Value *bufDesc,
                                       Value *loadOffset, Value *bufBase, CoherentFlag coherent, BuilderBase &builder) {
  Type *loadTyps[4] = {
      builder.getInt32Ty(),
      FixedVectorType::get(builder.getInt32Ty(), 2),
      FixedVectorType::get(builder.getInt32Ty(), 3),
      FixedVectorType::get(builder.getInt32Ty(), 4),
  };

  std::string funcName = "llvm.amdgcn.raw.tbuffer.load.";
  assert(loadValues.size() > 0);

  // 4-component combination
  unsigned compCount = 4;
  for (; compCount > 0; compCount--) {
    if (startIdx + compCount <= loadValues.size()) {
      funcName += getTypeName(loadTyps[compCount - 1]);

      Value *loadValue = nullptr;
      auto writeOffset = builder.CreateAdd(loadOffset, builder.getInt32(startIdx * 4));
      Value *args[] = {
          bufDesc,                                           // rsrc
          writeOffset,                                       // voffset
          bufBase,                                           // soffset
          builder.getInt32((*m_buffFormats)[compCount - 1]), // format
          builder.getInt32(coherent.u32All)                  // glc
      };
      loadValue = builder.CreateNamedCall(funcName, loadTyps[compCount - 1], args, {});
      assert(loadValue);
      if (compCount > 1) {
        for (unsigned i = 0; i < compCount; i++)
          loadValues[startIdx + i] = builder.CreateExtractElement(loadValue, i);
      } else
        loadValues[startIdx] = loadValue;

      break;
    }
  }

  return compCount;
}

// =====================================================================================================================
// Store value to stream-out buffer
//
// @param storeValue : Value to store
// @param xfbBuffer : Transform feedback buffer
// @param xfbOffset : Offset of the store value within transform feedback buffer
// @param xfbStride : Transform feedback stride
// @param streamId : Output stream ID
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::storeValueToStreamOutBuffer(Value *storeValue, unsigned xfbBuffer, unsigned xfbOffset,
                                             unsigned xfbStride, unsigned streamId, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
         m_shaderStage == ShaderStage::CopyShader);
  assert(xfbBuffer < MaxTransformFeedbackBuffers);

  auto storeTy = storeValue->getType();
  assert(storeTy->getScalarSizeInBits() == 32); // Must be 32-bit type

  unsigned compCount = storeTy->isVectorTy() ? cast<FixedVectorType>(storeTy)->getNumElements() : 1;
  assert(compCount <= 4);

  if (m_pipelineState->getNggControl()->enableNgg) {
    assert(m_pipelineState->enableSwXfb());
    builder.create<WriteXfbOutputOp>(xfbBuffer, xfbOffset, streamId, storeValue);
    return;
  }

  // NOTE: SW XFB must have been handled. Here we only handle HW XFB on pre-GFX11 generations.
  assert(m_gfxIp.major == 10);

  Value *streamInfo = nullptr;
  Value *writeIndex = nullptr;
  Value *streamOffset = nullptr;

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs;
  if (m_shaderStage == ShaderStage::Vertex) {
    streamInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.streamOutData.streamInfo);
    writeIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.streamOutData.writeIndex);
    streamOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.streamOutData.streamOffsets[xfbBuffer]);
  } else if (m_shaderStage == ShaderStage::TessEval) {
    streamInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.streamOutData.streamInfo);
    writeIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.streamOutData.writeIndex);
    streamOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.streamOutData.streamOffsets[xfbBuffer]);
  } else {
    assert(m_shaderStage == ShaderStage::CopyShader);

    streamInfo = getFunctionArgument(m_entryPoint, CopyShaderEntryArgIdxStreamInfo);
    writeIndex = getFunctionArgument(m_entryPoint, CopyShaderEntryArgIdxWriteIndex);

    const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
    assert(xfbStrides[xfbBuffer] > 0);

    // NOTE: The correct mapping between xfbBuffer[X] and streamOffset[X] must be determined according to the enablement
    // of previous streamOffsets. This is controlled by the register field SO_BASEX_EN.
    unsigned entryArgIdx = CopyShaderEntryArgIdxStreamOffset;
    if (xfbBuffer > 0) {
      for (unsigned i = 0; i < xfbBuffer; ++i) {
        if (xfbStrides[i] > 0)
          ++entryArgIdx;
      }
    }
    streamOffset = getFunctionArgument(m_entryPoint, entryArgIdx);
  }

  // streamOutVertexCount = streamInfo[22:16]
  Value *streamOutVertexCount = builder.CreateAnd(builder.CreateLShr(streamInfo, 16), 0x7F);

  // The stream offset provided by GE is dword-based. Convert it to byte-based.
  streamOffset = builder.CreateShl(streamOffset, 2);

  // GPU will drop stream-out buffer store when the thread ID is invalid (OOB_select is set to SQ_OOB_INDEX_ONLY).
  const unsigned outOfRangeWriteIndex = InvalidValue - (m_pipelineState->getShaderWaveSize(m_shaderStage.value()) - 1);
  // validStreamOutVertex = threadId < streamOutVertexCount
  auto validStreamOutVertex = builder.CreateICmpULT(m_threadId, streamOutVertexCount);
  // writeIndex = validStreamOutVertex ? writeIndex : outOfRangeWriteIndex
  writeIndex = builder.CreateSelect(validStreamOutVertex, writeIndex, builder.getInt32(outOfRangeWriteIndex));
  // writeIndex += threadId
  writeIndex = builder.CreateAdd(writeIndex, m_threadId);

  static unsigned char formatTable[] = {
      BUF_FORMAT_32_FLOAT,
      BUF_FORMAT_32_32_FLOAT_GFX10,
      BUF_FORMAT_32_32_32_FLOAT_GFX10,
      BUF_FORMAT_32_32_32_32_FLOAT_GFX10,
  };
  unsigned format = formatTable[compCount - 1];

  CoherentFlag coherent = {};
  coherent.bits.glc = true;
  coherent.bits.slc = true;

  builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_struct_tbuffer_store,
                          {storeValue, m_pipelineSysValues.get(m_entryPoint)->getStreamOutBufDesc(xfbBuffer),
                           writeIndex, builder.getInt32(xfbOffset), streamOffset, builder.getInt32(format),
                           builder.getInt32(coherent.u32All)});
}

// =====================================================================================================================
// Stores value to ES-GS ring (buffer or LDS).
//
// @param storeValue : Value to store
// @param location : Output location
// @param compIdx : Output component index
// @param builder : the builder to use
void LowerInOut::storeValueToEsGsRing(Value *storeValue, unsigned location, unsigned compIdx, BuilderBase &builder) {
  auto storeTy = storeValue->getType();

  Type *elemTy = storeTy;
  if (storeTy->isArrayTy())
    elemTy = cast<ArrayType>(storeTy)->getElementType();
  else if (storeTy->isVectorTy())
    elemTy = cast<VectorType>(storeTy)->getElementType();

  const uint64_t bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  if (storeTy->isArrayTy() || storeTy->isVectorTy()) {
    const unsigned elemCount = storeTy->isArrayTy() ? cast<ArrayType>(storeTy)->getNumElements()
                                                    : cast<FixedVectorType>(storeTy)->getNumElements();

    for (unsigned i = 0; i < elemCount; ++i) {
      Value *storeElem = nullptr;
      if (storeTy->isArrayTy())
        storeElem = builder.CreateExtractValue(storeValue, i);
      else {
        storeElem = builder.CreateExtractElement(storeValue, builder.getInt32(i));
      }

      storeValueToEsGsRing(storeElem, location + (compIdx + i) / 4, (compIdx + i) % 4, builder);
    }
  } else {
    if (bitWidth == 8 || bitWidth == 16) {
      if (storeTy->isFloatingPointTy()) {
        assert(bitWidth == 16);
        storeValue = builder.CreateBitCast(storeValue, builder.getInt16Ty());
      }

      storeValue = builder.CreateZExt(storeValue, builder.getInt32Ty());
    } else {
      assert(bitWidth == 32);
      if (storeTy->isFloatingPointTy())
        storeValue = builder.CreateBitCast(storeValue, builder.getInt32Ty());
    }

    // Call buffer store intrinsic or LDS store
    const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs;
    Value *esGsOffset = nullptr;
    if (m_shaderStage == ShaderStage::Vertex)
      esGsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.esGsOffset);
    else {
      assert(m_shaderStage == ShaderStage::TessEval);
      esGsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.esGsOffset);
    }

    auto ringOffset = calcEsGsRingOffsetForOutput(location, compIdx, esGsOffset, builder);

    // ES -> GS ring is always on-chip on GFX10+
    auto lds = LgcLowering::getLdsVariable(m_pipelineState, m_entryPoint);
    Value *storePtr = builder.CreateGEP(builder.getInt32Ty(), lds, ringOffset);
    builder.CreateAlignedStore(storeValue, storePtr, lds->getPointerAlignment(m_module->getDataLayout()));
  }
}

// =====================================================================================================================
// Loads value from ES-GS ring (buffer or LDS).
//
// @param loadTy : Load value type
// @param location : Input location
// @param compIdx : Input component index
// @param vertexIdx : Vertex index
// @param builder : the builder to use
Value *LowerInOut::loadValueFromEsGsRing(Type *loadTy, unsigned location, unsigned compIdx, Value *vertexIdx,
                                         BuilderBase &builder) {
  Type *elemTy = loadTy;
  if (loadTy->isArrayTy())
    elemTy = cast<ArrayType>(loadTy)->getElementType();
  else if (loadTy->isVectorTy())
    elemTy = cast<VectorType>(loadTy)->getElementType();

  const uint64_t bitWidth = elemTy->getScalarSizeInBits();
  (void)bitWidth; // unused in release builds
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  Value *loadValue = PoisonValue::get(loadTy);

  if (loadTy->isArrayTy() || loadTy->isVectorTy()) {
    const unsigned elemCount = loadTy->isArrayTy() ? cast<ArrayType>(loadTy)->getNumElements()
                                                   : cast<FixedVectorType>(loadTy)->getNumElements();

    for (unsigned i = 0; i < elemCount; ++i) {
      auto loadElem =
          loadValueFromEsGsRing(elemTy, location + (compIdx + i) / 4, (compIdx + i) % 4, vertexIdx, builder);

      if (loadTy->isArrayTy())
        loadValue = builder.CreateInsertValue(loadValue, loadElem, i);
      else {
        loadValue = builder.CreateInsertElement(loadValue, loadElem, i);
      }
    }
  } else {
    Value *ringOffset = calcEsGsRingOffsetForInput(location, compIdx, vertexIdx, builder);
    // ES -> GS ring is always on-chip on GFX10+
    auto lds = LgcLowering::getLdsVariable(m_pipelineState, m_entryPoint);
    auto *loadPtr = builder.CreateGEP(builder.getInt32Ty(), lds, ringOffset);
    loadValue = builder.CreateAlignedLoad(loadTy, loadPtr, lds->getPointerAlignment(m_module->getDataLayout()));
  }

  return loadValue;
}

// =====================================================================================================================
// Stores value to GS-VS ring (buffer or LDS).
//
// @param storeValue : Value to store
// @param location : Output location
// @param compIdx : Output component index
// @param streamId : Output stream ID
// @param builder : the builder to use
void LowerInOut::storeValueToGsVsRing(Value *storeValue, unsigned location, unsigned compIdx, unsigned streamId,
                                      BuilderBase &builder) {
  auto storeTy = storeValue->getType();

  Type *elemTy = storeTy;
  if (storeTy->isArrayTy())
    elemTy = cast<ArrayType>(storeTy)->getElementType();
  else if (storeTy->isVectorTy())
    elemTy = cast<VectorType>(storeTy)->getElementType();

  const unsigned bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  if (m_pipelineState->getNggControl()->enableNgg) {
    builder.create<NggWriteGsOutputOp>(location, compIdx, streamId, storeValue);
    return;
  }

  // NOTE: NGG with GS must have been handled. Here we only handle pre-GFX11 generations.
  assert(m_gfxIp.major < 11);

  if (storeTy->isArrayTy() || storeTy->isVectorTy()) {
    const unsigned elemCount = storeTy->isArrayTy() ? cast<ArrayType>(storeTy)->getNumElements()
                                                    : cast<FixedVectorType>(storeTy)->getNumElements();

    for (unsigned i = 0; i < elemCount; ++i) {
      Value *storeElem = nullptr;
      if (storeTy->isArrayTy())
        storeElem = builder.CreateExtractValue(storeValue, {i});
      else {
        storeElem = builder.CreateExtractElement(storeValue, i);
      }

      storeValueToGsVsRing(storeElem, location + (compIdx + i) / 4, (compIdx + i) % 4, streamId, builder);
    }
  } else {
    if (bitWidth == 8 || bitWidth == 16) {
      // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word
      // to dword. This is because copy shader does not know the actual data type. It only generates output
      // export calls based on number of dwords.
      if (storeTy->isFloatingPointTy()) {
        assert(bitWidth == 16);
        storeValue = builder.CreateBitCast(storeValue, builder.getInt16Ty());
      }

      storeValue = builder.CreateZExt(storeValue, builder.getInt32Ty());
    } else {
      assert(bitWidth == 32);
      if (storeTy->isFloatingPointTy())
        storeValue = builder.CreateBitCast(storeValue, builder.getInt32Ty());
    }

    const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs;
    Value *gsVsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.gs.gsVsOffset);

    auto emitCounterPair = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr();
    auto emitCounterTy = emitCounterPair.first;
    auto emitCounterPtr = emitCounterPair.second[streamId];
    auto emitCounter = builder.CreateLoad(emitCounterTy, emitCounterPtr);

    auto ringOffset = calcGsVsRingOffsetForOutput(location, compIdx, streamId, emitCounter, gsVsOffset, builder);

    IRBuilder<>::InsertPointGuard guard(builder);

    // Skip GS-VS ring write if the emit is invalid
    const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    if (geometryMode.robustGsEmits) {
      auto totalEmitCounterPtr = m_pipelineSysValues.get(m_entryPoint)->getTotalEmitCounterPtr();
      auto totalEmitCounter = builder.CreateLoad(builder.getInt32Ty(), totalEmitCounterPtr);
      // validEmit = totalEmitCounter < outputVertices
      auto validEmit = builder.CreateICmpULT(totalEmitCounter, builder.getInt32(geometryMode.outputVertices));
      builder.CreateIf(validEmit, false);
    }

    if (m_pipelineState->isGsOnChip()) {
      auto lds = LgcLowering::getLdsVariable(m_pipelineState, m_entryPoint);
      Value *storePtr = builder.CreateGEP(builder.getInt32Ty(), lds, ringOffset);
      builder.CreateAlignedStore(storeValue, storePtr, lds->getPointerAlignment(m_module->getDataLayout()));
    } else {
      // NOTE: Here we use tbuffer_store instruction instead of buffer_store because we have to do explicit
      // control of soffset. This is required by swizzle enabled mode when address range checking should be
      // complied with.
      CoherentFlag coherent = {};
      coherent.bits.glc = true;
      coherent.bits.slc = true;
      coherent.bits.swz = true;

      Value *args[] = {
          storeValue,                                                          // vdata
          m_pipelineSysValues.get(m_entryPoint)->getGsVsRingBufDesc(streamId), // rsrc
          ringOffset,                                                          // voffset
          gsVsOffset,                                                          // soffset
          builder.getInt32(BUF_FORMAT_32_UINT),
          builder.getInt32(coherent.u32All) // glc, slc, swz
      };
      builder.CreateIntrinsic(builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store, args);
    }
  }
}

// =====================================================================================================================
// Calculates the byte offset to store the output value to ES-GS ring based on the specified output info.
//
// @param location : Output location
// @param compIdx : Output component index
// @param esGsOffset : ES-GS ring offset in bytes
// @param builder : the builder to use
Value *LowerInOut::calcEsGsRingOffsetForOutput(unsigned location, unsigned compIdx, Value *esGsOffset,
                                               BuilderBase &builder) {
  // ES -> GS ring is always on-chip on GFX10+
  // ringOffset = esGsOffset + threadId * esGsRingItemSize + location * 4 + compIdx
  assert(m_pipelineState->hasShaderStage(ShaderStage::Geometry));
  const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;

  Value *ringOffset = builder.CreateMul(m_threadId, builder.getInt32(hwConfig.esGsRingItemSize));
  ringOffset = builder.CreateAdd(ringOffset, esGsOffset);
  ringOffset = builder.CreateAdd(ringOffset, builder.getInt32(location * 4 + compIdx));

  return ringOffset;
}

// =====================================================================================================================
// Calculates the byte offset to load the input value from ES-GS ring based on the specified input info.
//
// @param location : Input location
// @param compIdx : Input Component index
// @param vertexIdx : Vertex index
// @param builder : the builder to use
Value *LowerInOut::calcEsGsRingOffsetForInput(unsigned location, unsigned compIdx, Value *vertexIdx,
                                              BuilderBase &builder) {
  // ES -> GS ring is always on-chip on GFX10+
  assert(m_pipelineState->hasShaderStage(ShaderStage::Geometry));
  const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;

  auto esGsOffsets = m_pipelineSysValues.get(m_entryPoint)->getEsGsOffsets();
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

  Value *vertexOffset = nullptr;
  if (geometryMode.inputPrimitive == InputPrimitives::Patch) {
    assert(geometryMode.controlPoints > 0); // Must have control points

    // NOTE: If the input primitive is a patch, the calculation of vertex offset is different from other input primitive
    // types as follow:
    //
    //   vertexOffset = esGsOffset0 + vertexIdx * esGsRingItemSize
    //
    // The esGsOffset0 is the starting offset of control points for each patch with such HW layout:
    //
    // +-----------------+-----------------+-----+-------------------+
    // | Control Point 0 | Control Point 1 | ... | Control Point N-1 |
    // +-----------------+-----------------+-----+-------------------+
    // |<-------------------------- Patch -------------------------->|
    //
    vertexOffset = builder.CreateMul(vertexIdx, builder.getInt32(hwConfig.esGsRingItemSize));
    vertexOffset = builder.CreateAdd(builder.CreateExtractElement(esGsOffsets, static_cast<uint64_t>(0)), vertexOffset);
  } else {
    // vertexOffset = esGsOffsets[vertexIdx] (vertexIdx < 6)
    vertexOffset = builder.CreateExtractElement(esGsOffsets, vertexIdx);
  }

  // ringOffset = vertexOffset + (location * 4 + compIdx);
  Value *ringOffset = builder.CreateAdd(vertexOffset, builder.getInt32(location * 4 + compIdx));
  return ringOffset;
}

// =====================================================================================================================
// Calculates the offset to store the output value to GS-VS ring based on the specified output info.
//
// @param location : Output location
// @param compIdx : Output component
// @param streamId : Output stream ID
// @param vertexIdx : Vertex index
// @param gsVsOffset : ES-GS ring offset in bytes
// @param builder : the builder to use
Value *LowerInOut::calcGsVsRingOffsetForOutput(unsigned location, unsigned compIdx, unsigned streamId, Value *vertexIdx,
                                               Value *gsVsOffset, BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry);

  Value *ringOffset = nullptr;

  unsigned streamBases[MaxGsStreams];
  unsigned streamBase = 0;
  for (int i = 0; i < MaxGsStreams; ++i) {
    streamBases[i] = streamBase;
    streamBase += (resUsage->inOutUsage.gs.hwConfig.gsVsVertexItemSize[i] *
                   m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices);
  }

  if (m_pipelineState->isGsOnChip()) {
    // ringOffset = esGsLdsSize +
    //              gsVsOffset +
    //              threadId * gsVsRingItemSize +
    //              (vertexIdx * vertexSizePerStream) + location * 4 + compIdx + streamBase (in dwords)

    auto esGsLdsSize = builder.getInt32(resUsage->inOutUsage.gs.hwConfig.esGsLdsSize);

    gsVsOffset = builder.CreateLShr(gsVsOffset, 2, "", /*isExact=*/true);

    auto ringItemOffset =
        builder.CreateMul(m_threadId, builder.getInt32(resUsage->inOutUsage.gs.hwConfig.gsVsRingItemSize));

    // VertexSize is stream output vertexSize x 4 (in dwords)
    unsigned vertexItemSize = resUsage->inOutUsage.gs.hwConfig.gsVsVertexItemSize[streamId];
    auto vertexItemOffset = builder.CreateMul(vertexIdx, builder.getInt32(vertexItemSize));
    ringOffset = builder.CreateAdd(esGsLdsSize, gsVsOffset);
    ringOffset = builder.CreateAdd(ringOffset, ringItemOffset);
    ringOffset = builder.CreateAdd(ringOffset, vertexItemOffset);

    unsigned attribOffset = (location * 4) + compIdx + streamBases[streamId];
    ringOffset = builder.CreateAdd(ringOffset, builder.getInt32(attribOffset));
  } else {
    // ringOffset = ((location * 4 + compIdx) * maxVertices + vertexIdx) * 4 (in bytes);
    unsigned outputVertices = m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices;

    ringOffset = builder.CreateAdd(vertexIdx, builder.getInt32((location * 4 + compIdx) * outputVertices));
    ringOffset = builder.CreateMul(ringOffset, builder.getInt32(4));
  }

  return ringOffset;
}

// =====================================================================================================================
// Reads value from LDS.
//
// @param offChip : Whether to use off-chip LDS or not
// @param isOutput : Is the value from output variable
// @param readTy : Type of value read from LDS
// @param ldsOffset : Start offset to do LDS read operations
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::readValueFromLds(bool offChip, Type *readTy, Value *ldsOffset, BuilderBase &builder) {
  assert(readTy->isSingleValueType());

  // Read dwords from LDS
  const unsigned compCount = readTy->isVectorTy() ? cast<FixedVectorType>(readTy)->getNumElements() : 1;
  const unsigned bitWidth = readTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);
  const unsigned numChannels = compCount * (bitWidth == 64 ? 2 : 1);

  std::vector<Value *> loadValues(numChannels);

  if (offChip) {
    // Read from off-chip LDS buffer
    const auto &offChipLdsBaseArgIdx =
        m_shaderStage == ShaderStage::TessEval
            ? m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs.tes.offChipLdsBase
            : m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs.tcs.offChipLdsBase;

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, offChipLdsBaseArgIdx);

    // Convert dword off-chip LDS offset to byte offset
    ldsOffset = builder.CreateMul(ldsOffset, builder.getInt32(4));

    CoherentFlag coherent = {};
    if (m_gfxIp.major == 10) {
      coherent.bits.glc = true;
      coherent.bits.dlc = true;
    } else if (m_gfxIp.major == 11) {
      // NOTE: dlc depends on MALL NOALLOC which isn't used by now.
      coherent.bits.glc = true;
    } else if (m_gfxIp.major >= 12) {
      coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
      coherent.gfx12.th = m_pipelineState->getTemporalHint(TH::TH_RT, TemporalHintTessRead);
    } else
      llvm_unreachable("Not implemented!");

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount)
      combineCount = combineBufferLoad(loadValues, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, builder);
  } else {
    // Read from on-chip LDS
    for (unsigned i = 0; i < numChannels; ++i) {
      auto loadTy = builder.getInt32Ty();
      auto lds = LgcLowering::getLdsVariable(m_pipelineState, m_entryPoint);
      auto *loadPtr = builder.CreateGEP(loadTy, lds, ldsOffset);
      loadValues[i] = builder.CreateLoad(loadTy, loadPtr);

      ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(1));
    }
  }

  if (bitWidth == 8 || bitWidth == 16) {
    Type *ty = bitWidth == 8 ? builder.getInt8Ty() : builder.getInt16Ty();
    for (unsigned i = 0; i < numChannels; ++i)
      loadValues[i] = builder.CreateTrunc(loadValues[i], ty);
  }

  // Construct <n x i8>, <n x i16>, or <n x i32> vector from load values (dwords)
  Value *castValue = nullptr;
  if (numChannels > 1) {
    auto intTy = bitWidth == 32 || bitWidth == 64 ? builder.getInt32Ty()
                                                  : (bitWidth == 16 ? builder.getInt16Ty() : builder.getInt8Ty());
    auto castTy = FixedVectorType::get(intTy, numChannels);
    castValue = PoisonValue::get(castTy);

    for (unsigned i = 0; i < numChannels; ++i) {
      castValue = builder.CreateInsertElement(castValue, loadValues[i], i);
    }
  } else
    castValue = loadValues[0];

  // Cast <n x i8>, <n x i16> or <n x i32> vector to read value
  return builder.CreateBitCast(castValue, readTy);
}

// =====================================================================================================================
// Writes value to LDS.
//
// @param offChip : Whether to use off-chip LDS or not
// @param writeValue : Value written to LDS
// @param ldsOffset : Start offset to do LDS write operations
// @param builder : The IR builder to create and insert IR instruction
void LowerInOut::writeValueToLds(bool offChip, Value *writeValue, Value *ldsOffset, BuilderBase &builder) {
  auto writeTy = writeValue->getType();
  assert(writeTy->isSingleValueType());

  const unsigned compCout = writeTy->isVectorTy() ? cast<FixedVectorType>(writeTy)->getNumElements() : 1;
  const unsigned bitWidth = writeTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);
  const unsigned numChannels = compCout * (bitWidth == 64 ? 2 : 1);

  // Cast write value to <n x i32> vector
  Type *intTy = bitWidth == 32 || bitWidth == 64 ? builder.getInt32Ty()
                                                 : (bitWidth == 16 ? builder.getInt16Ty() : builder.getInt8Ty());
  Type *castTy = numChannels > 1 ? cast<Type>(FixedVectorType::get(intTy, numChannels)) : intTy;
  Value *castValue = builder.CreateBitCast(writeValue, castTy);

  // Extract store values (dwords) from <n x i8>, <n x i16> or <n x i32> vector
  std::vector<Value *> storeValues(numChannels);
  if (numChannels > 1) {
    for (unsigned i = 0; i < numChannels; ++i)
      storeValues[i] = builder.CreateExtractElement(castValue, i);
  } else {
    storeValues[0] = castValue;
  }

  if (bitWidth == 8 || bitWidth == 16) {
    for (unsigned i = 0; i < numChannels; ++i)
      storeValues[i] = builder.CreateZExt(storeValues[i], builder.getInt32Ty());
  }

  if (offChip) {
    // Write to off-chip LDS buffer
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs.tcs;

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, entryArgIdxs.offChipLdsBase);
    // Convert dword off-chip LDS offset to byte offset
    ldsOffset = builder.CreateMul(ldsOffset, builder.getInt32(4));

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    CoherentFlag coherent = {};
    if (m_gfxIp.major <= 11)
      coherent.bits.glc = true;
    else {
      coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
      coherent.gfx12.th = m_pipelineState->getTemporalHint(TH::TH_WB, TemporalHintTessWrite);
    }

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount) {
      combineCount =
          combineBufferStore(storeValues, i, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, builder);
    }
  } else {
    // Write to on-chip LDS
    for (unsigned i = 0; i < numChannels; ++i) {
      auto lds = LgcLowering::getLdsVariable(m_pipelineState, m_entryPoint);
      Value *storePtr = builder.CreateGEP(builder.getInt32Ty(), lds, ldsOffset);
      builder.CreateStore(storeValues[i], storePtr);

      ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(1));
    }
  }
}

// =====================================================================================================================
// Calculates the dword offset to write value to LDS based on the specified VS output info.
//
// @param outputTy : Type of the output
// @param location : Base location of the output
// @param compIdx : Index used for vector element indexing
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::calcLdsOffsetForVsOutput(Type *outputTy, unsigned location, unsigned compIdx, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Vertex);

  // attribOffset = location * 4 + compIdx
  Value *attribOffset = builder.getInt32(location * 4);

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  if (bitWidth == 64) {
    // For 64-bit data type, the component indexing must multiply by 2
    compIdx *= 2;
  }

  attribOffset = builder.CreateAdd(attribOffset, builder.getInt32(compIdx));

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex)->entryArgIdxs.vs;
  auto relVertexId = getFunctionArgument(m_entryPoint, entryArgIdxs.relVertexId);

  const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.hwConfig;
  // dwordOffset = inputPatchStart + relVertexId * vertexStride + attribOffset
  Value *ldsOffset = builder.getInt32(hwConfig.onChip.inputPatchStart);
  ldsOffset =
      builder.CreateAdd(ldsOffset, builder.CreateMul(relVertexId, builder.getInt32(hwConfig.onChip.inputVertexStride)));
  ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);

  return ldsOffset;
}

// =====================================================================================================================
// Calculates the dword offset to read value from LDS based on the specified TCS input info.
//
// @param inputTy : Type of the input
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing (could be null)
// @param vertexIdx : Vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::calcLdsOffsetForTcsInput(Type *inputTy, unsigned location, Value *locOffset, Value *compIdx,
                                            Value *vertexIdx, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::TessControl);

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs;
  const auto &hwConfig = inOutUsage.hwConfig;

  // attribOffset = (location + locOffset) * 4 + compIdx
  Value *attribOffset = builder.getInt32(location);

  if (locOffset)
    attribOffset = builder.CreateAdd(attribOffset, locOffset);

  attribOffset = builder.CreateMul(attribOffset, builder.getInt32(4));

  if (compIdx) {
    const unsigned bitWidth = inputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = builder.CreateMul(compIdx, builder.getInt32(2));
    }

    attribOffset = builder.CreateAdd(attribOffset, compIdx);
  }

  // dwordOffset = inputPatchStart + (relativeId * inputVertexCount + vertexIdx) * inputVertexStride + attribOffset
  auto inputVertexCount = m_pipelineState->getNumPatchControlPoints();
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
  Value *ldsOffset = builder.CreateMul(relativeId, builder.getInt32(inputVertexCount));
  ldsOffset = builder.CreateAdd(ldsOffset, vertexIdx);
  ldsOffset = builder.CreateMul(ldsOffset, builder.getInt32(hwConfig.onChip.inputVertexStride));
  ldsOffset =
      builder.CreateAdd(builder.getInt32(hwConfig.onChip.inputPatchStart), builder.CreateAdd(ldsOffset, attribOffset));

  return ldsOffset;
}

// =====================================================================================================================
// Calculates the dword offset to read/write value from/to LDS based on the specified TCS output info.
//
// @param outputTy : Type of the output
// @param location : Base location of the output
// @param locOffset : Relative location offset (could be null)
// @param compIdx : Index used for vector element indexing (could be null)
// @param vertexIdx : Vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::calcLdsOffsetForTcsOutput(Type *outputTy, unsigned location, Value *locOffset, Value *compIdx,
                                             Value *vertexIdx, BuilderBase &builder) {
  // NOTE: TCS outputs are always stored to on-chip LDS first. Then, they are store to off-chip LDS buffer (which will
  // be loaded by TES).
  assert(m_shaderStage == ShaderStage::TessControl);

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs;
  const auto &hwConfig = inOutUsage.hwConfig;

  // attribOffset = (location + locOffset) * 4 + compIdx * bitWidth / 32
  Value *attribOffset = builder.getInt32(location);

  if (locOffset)
    attribOffset = builder.CreateAdd(attribOffset, locOffset);

  attribOffset = builder.CreateMul(attribOffset, builder.getInt32(4));

  if (compIdx) {
    const unsigned bitWidth = outputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = builder.CreateMul(compIdx, builder.getInt32(2));
    }

    attribOffset = builder.CreateAdd(attribOffset, compIdx);
  }

  Value *ldsOffset = nullptr;
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
  if (vertexIdx) {
    // dwordOffset = outputPatchStart + (relativeId * outputVertexCount + vertexIdx) * outputVertexStride + attribOffset
    //             = outputPatchStart + relativeId * outputPatchSize + vertexIdx * outputVertexStride + attribOffset
    ldsOffset = builder.CreateMul(relativeId, builder.getInt32(hwConfig.onChip.outputPatchSize));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(hwConfig.onChip.outputPatchStart));
    ldsOffset = builder.CreateAdd(ldsOffset,
                                  builder.CreateMul(vertexIdx, builder.getInt32(hwConfig.onChip.outputVertexStride)));
    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  } else {
    // dwordOffset = patchConstStart + relativeId * patchConstSize + attribOffset
    ldsOffset = builder.CreateMul(relativeId, builder.getInt32(hwConfig.onChip.patchConstSize));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(hwConfig.onChip.patchConstStart));
    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  }

  return ldsOffset;
}

// =====================================================================================================================
// Calculates the dword offset to read/write value from/to LDS based on the specified TES input info.
//
// @param inputTy : Type of the input
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing (could be null)
// @param vertexIdx : Vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::calcLdsOffsetForTesInput(Type *inputTy, unsigned location, Value *locOffset, Value *compIdx,
                                            Value *vertexIdx, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::TessEval);

  const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.hwConfig;
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage.value())->entryArgIdxs.tes;

  auto relPatchId = getFunctionArgument(m_entryPoint, entryArgIdxs.relPatchId);

  // attribOffset = (location + locOffset) * 4 + compIdx
  Value *attribOffset = builder.getInt32(location);

  if (locOffset)
    attribOffset = builder.CreateAdd(attribOffset, locOffset);

  attribOffset = builder.CreateMul(attribOffset, builder.getInt32(4));

  if (compIdx) {
    const unsigned bitWidth = inputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = builder.CreateMul(compIdx, builder.getInt32(2));
    }

    attribOffset = builder.CreateAdd(attribOffset, compIdx);
  }

  Value *ldsOffset = nullptr;
  if (vertexIdx) {
    // dwordOffset = patchStart + (relPatchId * vertexCount + vertexIdx) * vertexStride + attribOffset
    //             = patchStart + relPatchId * patchSize + vertexIdx * vertexStride + attribOffset
    ldsOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.offChip.outputPatchSize));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(hwConfig.offChip.outputPatchStart));
    ldsOffset = builder.CreateAdd(ldsOffset,
                                  builder.CreateMul(vertexIdx, builder.getInt32(hwConfig.offChip.outputVertexStride)));
    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  } else {
    // dwordOffset = patchConstStart + relPatchId * patchConstSize + attribOffset
    ldsOffset = builder.CreateMul(relPatchId, builder.getInt32(hwConfig.offChip.patchConstSize));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(hwConfig.offChip.patchConstStart));
    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  }

  return ldsOffset;
}

// =====================================================================================================================
// Calculates maximum number of HS patches per thread group.
//
// @param inputVertexCount : Count of vertices of input patch
// @param outputVertexCount : Count of vertices of output patch
// @param tessFactorCount : Count of tessellation factors
// @param ldsSizePerPatch : On-chip LDS size per patch (in dwords)
// @param ldsBufferSizePerPatch : Off-chip LDS buffer size per patch (in dwords)
unsigned LowerInOut::calcMaxNumPatchesPerGroup(unsigned inputVertexCount, unsigned outputVertexCount,
                                               unsigned tessFactorCount, unsigned ldsSizePerPatch,
                                               unsigned ldsBufferSizePerPatch) const {
  unsigned maxNumThreadsPerGroup = MaxHsThreadsPerSubgroup;

  // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
  // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
  // restriction by providing the capability of querying thread ID in the group rather than in wave.
  unsigned rayQueryLdsStackSize = 0;
  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Vertex);
  const auto tcsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl);
  if (vsResUsage->useRayQueryLdsStack || tcsResUsage->useRayQueryLdsStack) {
    maxNumThreadsPerGroup = std::min(MaxRayQueryThreadsPerGroup, maxNumThreadsPerGroup);
    rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;
  }

  const unsigned maxNumThreadsPerPatch = std::max(inputVertexCount, outputVertexCount);
  const unsigned numPatchesLimitedByThread = maxNumThreadsPerGroup / maxNumThreadsPerPatch;

  unsigned ldsSizePerGroup = m_pipelineState->getTargetInfo().getGpuProperty().ldsSizePerThreadGroup;
  if (m_pipelineState->canOptimizeTessFactor()) {
    // NOTE: If we are going to optimize TF store, we need additional on-chip LDS size. The required size is
    // 2 dwords per HS wave (1 dword all-ones flag or 1 dword all-zeros flag) plus an extra dword to
    // count actual HS patches.
    assert(m_gfxIp.major >= 11);
    const unsigned maxNumHsWaves =
        MaxHsThreadsPerSubgroup / m_pipelineState->getShaderWaveSize(ShaderStage::TessControl);
    ldsSizePerGroup -= 1 + maxNumHsWaves * 2;
  }
  ldsSizePerGroup -= rayQueryLdsStackSize; // Exclude LDS space used as ray query stack

  unsigned numPatchesLimitedByLds = ldsSizePerGroup / ldsSizePerPatch;

  unsigned maxNumPatchesPerGroup = std::min(numPatchesLimitedByThread, numPatchesLimitedByLds);

  // NOTE: Performance analysis shows that 16 patches per group is an optimal upper-bound. The value is only
  // an experimental number.
  const unsigned optimalNumPatchesPerGroup = 64;
  maxNumPatchesPerGroup = std::min(maxNumPatchesPerGroup, optimalNumPatchesPerGroup);

  unsigned outputPatchLdsBufferSize = ldsBufferSizePerPatch * sizeof(unsigned);
  auto offChipNumHsPatchesPerGroup =
      m_pipelineState->getTargetInfo().getGpuProperty().tessOffChipLdsBufferSize / outputPatchLdsBufferSize;
  maxNumPatchesPerGroup = std::min(maxNumPatchesPerGroup, offChipNumHsPatchesPerGroup);

  // TF-Buffer-based limit for Patchers per Thread Group:
  // ---------------------------------------------------------------------------------------------

  // There is one TF Buffer per shader engine. We can do the below calculation on a per-SE basis.  It is also safe to
  // assume that one thread-group could at most utilize all of the TF Buffer.
  const unsigned tfBufferSizeInBytes =
      sizeof(unsigned) * m_pipelineState->getTargetInfo().getGpuProperty().tessFactorBufferSizePerSe;
  unsigned tfBufferNumPatchesLimit = tfBufferSizeInBytes / (tessFactorCount * sizeof(unsigned));

  const auto workarounds = &m_pipelineState->getTargetInfo().getGpuWorkarounds();
  if (workarounds->gfx10.waTessFactorBufferSizeLimitGeUtcl1Underflow) {
    tfBufferNumPatchesLimit /= 2;
  }

  maxNumPatchesPerGroup = std::min(maxNumPatchesPerGroup, tfBufferNumPatchesLimit);

  // For all-offchip tessellation, we need to write an additional 4-byte TCS control word to the TF buffer whenever
  // the patch-ID is zero.
  const unsigned offChipTfBufferNumPatchesLimit =
      (tfBufferSizeInBytes - (maxNumPatchesPerGroup * sizeof(unsigned))) / (tessFactorCount * sizeof(unsigned));
  maxNumPatchesPerGroup = std::min(maxNumPatchesPerGroup, offChipTfBufferNumPatchesLimit);

  return maxNumPatchesPerGroup;
}

// =====================================================================================================================
// Inserts "exp" instruction to export generic output.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param builder : the builder to use
void LowerInOut::addExportInstForGenericOutput(Value *output, unsigned location, unsigned compIdx,
                                               BuilderBase &builder) {
  // Check if the shader stage is valid to use "exp" instruction to export output
  const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage.value());
  const bool useExpInst = ((m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
                            m_shaderStage == ShaderStage::CopyShader) &&
                           (!nextStage || nextStage == ShaderStage::Fragment));
  assert(useExpInst);
  (void(useExpInst)); // unused

  auto outputTy = output->getType();

  const unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;
  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  // Convert the output value to floating-point export value
  Value *exportInst = nullptr;
  const unsigned numChannels = bitWidth == 64 ? compCount * 2 : compCount;
  unsigned startChannel = bitWidth == 64 ? compIdx * 2 : compIdx;
  Type *exportTy = numChannels > 1 ? FixedVectorType::get(builder.getFloatTy(), numChannels) : builder.getFloatTy();

  if (outputTy != exportTy) {
    if (bitWidth == 8) {
      // NOTE: For 16-bit output export, we have to cast the 8-bit value to 32-bit floating-point value.
      assert(outputTy->isIntOrIntVectorTy());
      Type *zExtTy = builder.getInt32Ty();
      zExtTy = outputTy->isVectorTy() ? cast<Type>(FixedVectorType::get(zExtTy, compCount)) : zExtTy;
      exportInst = builder.CreateZExt(output, zExtTy);
      exportInst = builder.CreateBitCast(exportInst, exportTy);
    } else if (bitWidth == 16) {
      // NOTE: For 16-bit output export, we have to cast the 16-bit value to 32-bit floating-point value.
      if (outputTy->isFPOrFPVectorTy()) {
        Type *bitCastTy = builder.getInt16Ty();
        bitCastTy = outputTy->isVectorTy() ? cast<Type>(FixedVectorType::get(bitCastTy, compCount)) : bitCastTy;
        exportInst = builder.CreateBitCast(output, bitCastTy);
      } else {
        assert(outputTy->isIntOrIntVectorTy());
        exportInst = output;
      }

      Type *zExtTy = builder.getInt32Ty();
      zExtTy = outputTy->isVectorTy() ? cast<Type>(FixedVectorType::get(zExtTy, compCount)) : zExtTy;
      exportInst = builder.CreateZExt(exportInst, zExtTy);
      exportInst = builder.CreateBitCast(exportInst, exportTy);
    } else {
      assert(canBitCast(outputTy, exportTy));
      exportInst = builder.CreateBitCast(output, exportTy);
    }
  } else
    exportInst = output;

  assert(numChannels <= 8);
  Value *exportValues[8] = {nullptr};

  if (numChannels == 1)
    exportValues[0] = exportInst;
  else {
    for (unsigned i = 0; i < numChannels; ++i) {
      exportValues[i] = builder.CreateExtractElement(exportInst, i);
    }
  }

  auto poison = PoisonValue::get(builder.getFloatTy());
  if (numChannels <= 4) {
    assert(startChannel + numChannels <= 4);

    Value *attribValues[4] = {poison, poison, poison, poison};
    for (unsigned i = startChannel; i < startChannel + numChannels; ++i)
      attribValues[i] = exportValues[i - startChannel];

    m_expLocs.insert(location);
    recordVertexAttribute(location, {attribValues[0], attribValues[1], attribValues[2], attribValues[3]});
  } else {
    // We have to do exporting twice for this output
    assert(startChannel == 0); // Other values are disallowed according to GLSL spec
    assert(numChannels == 6 || numChannels == 8);

    Value *attribValues[8] = {poison, poison, poison, poison, poison, poison, poison, poison};
    for (unsigned i = 0; i < numChannels; ++i)
      attribValues[i] = exportValues[i];

    m_expLocs.insert(location); // First export
    recordVertexAttribute(location, {attribValues[0], attribValues[1], attribValues[2], attribValues[3]});

    m_expLocs.insert(location + 1); // Second export
    recordVertexAttribute(location + 1, {attribValues[4], attribValues[5], attribValues[6], attribValues[7]});
  }
}

// =====================================================================================================================
// Inserts "exp" instruction to export built-in output.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param builder : the builder to use
void LowerInOut::addExportInstForBuiltInOutput(Value *output, unsigned builtInId, BuilderBase &builder) {
  const auto poison = PoisonValue::get(builder.getFloatTy());

  switch (builtInId) {
  case BuiltInPosition: {
    SmallVector<Value *, 4> positions;
    for (unsigned i = 0; i < 4; ++i)
      positions.push_back(builder.CreateExtractElement(output, builder.getInt32(i)));

    exportPosition(0, positions, builder);
    break;
  }
  case BuiltInPointSize: {
    exportPosition(1, {output, poison, poison, poison}, builder);
    break;
  }
  case BuiltInPrimitiveShadingRate: {
    // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
    assert(m_gfxIp >= GfxIpVersion({10, 3}));

    exportShadingRate(output, builder);
    break;
  }
  case BuiltInEdgeFlag: {
    Value *edgeFlag = builder.CreateBitCast(output, builder.getFloatTy());
    exportPosition(1, {poison, edgeFlag, poison, poison}, builder);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Adjusts I/J calculation for "centroid" interpolation mode by taking "center" mode into account.
//
// @param centroidIj : Centroid I/J provided by hardware natively
// @param centerIj : Center I/J provided by hardware natively
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::adjustCentroidIj(Value *centroidIj, Value *centerIj, BuilderBase &builder) {
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Fragment)->builtInUsage.fs;
  Value *ij = nullptr;

  if (builtInUsage.centroid && builtInUsage.center) {
    // NOTE: If both centroid and center are enabled, centroid I/J provided by hardware natively may be invalid. We have
    // to adjust it with center I/J on condition of bc_optimize flag. bc_optimize = primMask[31], when bc_optimize is
    // on, primMask is less than zero
    auto cond = builder.CreateICmpSLT(primMask, builder.getInt32(0));
    ij = builder.CreateSelect(cond, centerIj, centroidIj);
  } else
    ij = centroidIj;

  return ij;
}

// =====================================================================================================================
// Get Subgroup local invocation Id
//
// @param builder : The IR builder to create and insert IR instruction
Value *LowerInOut::getSubgroupLocalInvocationId(BuilderBase &builder) {
  Value *subgroupLocalInvocationId =
      builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});

  unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage.value());
  if (waveSize == 64) {
    subgroupLocalInvocationId =
        builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), subgroupLocalInvocationId});
  }

  return subgroupLocalInvocationId;
}

// =====================================================================================================================
// Creates the LGC intrinsic "lgc.swizzle.thread.group" to swizzle thread group for optimization purposes.
//
void LowerInOut::createSwizzleThreadGroupFunction() {

  // Generate IR instructions to swizzle thread groups with repeating N x N tiles of morton patterns. If the X or Y
  // dimensions are not divisible by N, thread groups along the right and bottom sections of the dispatch get row-major
  // and column-major ordering. Only the XY groups are swizzled, the Z value for thread ID and group ID are preserved.
  // Swizzling happens when there is more than 1 morton tile.
  //
  // Z - Swizzled set of N x N thread groups
  // R - Row-major thread groups
  // C - Column-major thread groups
  //
  // |ZZZZZZZZZZZZZZZZZZ|R|
  // |ZZZZZZZZZZZZZZZZZZ|R|
  // |ZZZZZZZZZZZZZZZZZZ|R|
  // |ZZZZZZZZZZZZZZZZZZ|R|
  // |CCCCCCCCCCCCCCCCCCCC|

  // The basic algorithm is that (in pseudo-code):
  //
  // define <3 x i32> @lgc.swizzle.thread.group(<3 x i32> %numWorkgroups, <3 x i32> %nativeWorkgroupId) {

  //   threadGroupFlatId = nativeWorkgroupId.y * numWorkgroups.x + nativeWorkgroupId.x
  //   numTiles = numWorkgroups / tileDim
  //   if (isMoreThanOneTile.x && isMoreThanOneTile.y)
  //     perform swizzle
  //   else
  //     disable swizzle
  //   ret swizzledWorkgroupId
  // }

  // Perform swizzle:
  //   // Calculate the size of section need to be swizzled
  //   numSwizzledThreadGroup = numTiles * tileDim
  //
  //   // Calculate the size of the side section
  //   sideStart = numSwizzledThreadGroup.x * numSwizzledThreadGroup.y
  //   sideWidth = numWorkgroups.x - numSwizzledThreadGroup.x
  //   sideSize = sideWidth * numSwizzledThreadGroup.y
  //
  //   // Calculate the size of the bottom section
  //   bottomStart = sideStart + sideSize
  //   bottomHeight = numWorkgroups.y - numSwizzledThreadGroup.y
  //
  //   if (threadGroupFlatId >= bottomStart)
  //     // Bottom tile
  //     // Get new thread group ID for thread group in the bottom section
  //     // Thread groups are reordered up->down then left->right
  //     localThreadGroupFlatId = threadGroupFlatId - bottomStart
  //     swizzledWorkgroupId.x = localThreadGroupFlatId / bottomHeight
  //     swizzledWorkgroupId.y = (localThreadGroupFlatId % bottomHeight) + numSwizzledThreadGroup.y
  //   else if (threadGroupFlatId >= sideStart)
  //     // Side tile
  //     // Get new thread group ID for thread group in the side section
  //     // Thread groups are reordered left->right then up->down
  //     localThreadGroupFlatId = threadGroupFlatId - sideStart
  //     swizzledWorkgroupId.x = (localThreadGroupFlatId % sideWidth) + numSwizzledThreadGroup.x
  //     swizzledWorkgroupId.y = localThreadGroupFlatId / sideWidth
  //   else
  //     // Morton tile
  //     localThreadGroupFlatId = threadGroupFlatId % tileSize
  //     // Extract to xy dimension based on Z-order curved
  //     localThreadGroupId.x = Compact1By1Bits(tileBits, localThreadGroupFlatId)
  //     localThreadGroupId.y = Compact1By1Bits(tileBits, localThreadGroupFlatId >> 1)
  //     flatTileId = threadGroupFlatId / tileSize
  //     swizzledWorkgroupId.x = (flatTileId % numTiles.x) * tileDim + localThreadGroupId.x
  //     swizzledWorkgroupId.y = (flatTileId / numTiles.x) * tileDim + localThreadGroupId.y
  //
  //   // Finalize
  //   swizzledWorkgroupId.z = nativeWorkgroupId.z
  //
  // Disable swizzle:
  //   swizzledWorkgroupId = nativeWorkgroupId

  BuilderBase builder(*m_context);

  Type *ivec3Ty = FixedVectorType::get(builder.getInt32Ty(), 3);

  auto func = m_module->getFunction(lgcName::SwizzleWorkgroupId);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::NoUnwind);
  func->addFnAttr(Attribute::AlwaysInline);
  func->setDoesNotAccessMemory();
  func->setLinkage(GlobalValue::InternalLinkage);

  auto argIt = func->arg_begin();

  Value *numWorkgroups = argIt++;
  numWorkgroups->setName("numWorkgroups");

  Value *nativeWorkgroupId = argIt++;
  nativeWorkgroupId->setName("nativeWorkgroupId");

  static constexpr unsigned tileDims[] = {InvalidValue, 4, 8, 16, 32, 64};
  static constexpr unsigned tileBits[] = {InvalidValue, 2, 3, 4, 5, 6};
  static_assert((sizeof(tileDims) / sizeof(unsigned)) == static_cast<unsigned>(ThreadGroupSwizzleMode::Count),
                "The length of tileDims is not as expected.");
  static_assert((sizeof(tileBits) / sizeof(unsigned)) == static_cast<unsigned>(ThreadGroupSwizzleMode::Count),
                "The length of tileBits is not as expected.");

  assert(m_pipelineState->getOptions().threadGroupSwizzleMode != ThreadGroupSwizzleMode::Default);
  const unsigned tileIndex = static_cast<unsigned>(m_pipelineState->getOptions().threadGroupSwizzleMode);

  auto entryBlock = BasicBlock::Create(*m_context, ".entry", func);
  builder.SetInsertPoint(entryBlock);

  Constant *tileDim = builder.getInt32(tileDims[tileIndex]);
  Constant *tileSize = builder.getInt32(tileDims[tileIndex] * tileDims[tileIndex]);
  Constant *one = builder.getInt32(1);

  ElementCount ec = ElementCount::get(3, false);

  auto swizzledWorkgroupIdPtr = builder.CreateAlloca(ivec3Ty);

  // Calculate flat thread group ID
  // threadGroupFlatId = nativeWorkgroupId.y * numWorkgroups.x + nativeWorkgroupId.x
  auto threadGroupFlatId =
      builder.CreateAdd(builder.CreateMul(builder.CreateExtractElement(nativeWorkgroupId, 1),
                                          builder.CreateExtractElement(numWorkgroups, uint64_t(0))),
                        builder.CreateExtractElement(nativeWorkgroupId, uint64_t(0)));

  // Calculate the number of thread group tiles that need to be swizzled
  // numTiles = numWorkgroups / tileDim
  auto numTiles = builder.CreateUDiv(numWorkgroups, ConstantVector::getSplat(ec, tileDim));

  // Calculate whether there is more than one tile
  auto isMoreThanOneTile = builder.CreateICmpUGT(numTiles, ConstantVector::getSplat(ec, one));

  // if (isMoreThanOneTile.x && isMoreThanOneTile.y)
  //   perform swizzle
  // else
  //   disable swizzle
  auto performSwizzleBlock = BasicBlock::Create(*m_context, ".performSwizzle", func);
  auto disableSwizzleBlock = BasicBlock::Create(*m_context, ".disableSwizzle", func);
  auto finalizeBlock = BasicBlock::Create(*m_context, ".finalize", func);
  auto returnBlock = BasicBlock::Create(*m_context, ".return", func);
  auto isXAndYMoreThanOneTile = builder.CreateAnd(builder.CreateExtractElement(isMoreThanOneTile, uint64_t(0)),
                                                  builder.CreateExtractElement(isMoreThanOneTile, 1));
  builder.CreateCondBr(isXAndYMoreThanOneTile, performSwizzleBlock, disableSwizzleBlock);

  {
    // Perform swizzle
    builder.SetInsertPoint(performSwizzleBlock);
    // Calculate the size of section need to be swizzled
    // numSwizzledThreadGroup = numTiles * tileDim
    auto numSwizzledThreadGroup = builder.CreateMul(numTiles, ConstantVector::getSplat(ec, tileDim));

    // Calculate the size of the side section
    // sideStart = numSwizzledThreadGroup.x * numSwizzledThreadGroup.y
    // sideWidth = numWorkgroups.x - numSwizzledThreadGroup.x
    // sideSize = sideWidth * numSwizzledThreadGroup.y
    auto sideStart = builder.CreateMul(builder.CreateExtractElement(numSwizzledThreadGroup, uint64_t(0)),
                                       builder.CreateExtractElement(numSwizzledThreadGroup, 1));
    auto sideWidth = builder.CreateSub(builder.CreateExtractElement(numWorkgroups, uint64_t(0)),
                                       builder.CreateExtractElement(numSwizzledThreadGroup, uint64_t(0)));
    auto sideSize = builder.CreateMul(sideWidth, builder.CreateExtractElement(numSwizzledThreadGroup, 1));

    // Calculate the size of the bottom section
    // bottomStart = sideStart + sideSize
    // bottomHeight = numWorkgroups.y - numSwizzledThreadGroup.y
    auto bottomStart = builder.CreateAdd(sideStart, sideSize);
    auto bottomHeight = builder.CreateSub(builder.CreateExtractElement(numWorkgroups, 1),
                                          builder.CreateExtractElement(numSwizzledThreadGroup, 1));

    // if (threadGroupFlatId >= bottomStart)
    //   bottom tile
    // else if (threadGroupFlatId >= sideStart)
    //   side tile
    // else
    //   morton tile
    // finalize
    auto bottomTileBlock = BasicBlock::Create(*m_context, "bottomTile", func, finalizeBlock);
    auto bottomTileElseIfBlock = BasicBlock::Create(*m_context, ".bottomTile.elseIf", func, finalizeBlock);
    auto sideTileBlock = BasicBlock::Create(*m_context, ".sideTile", func, finalizeBlock);
    auto mortonTileBlock = BasicBlock::Create(*m_context, ".mortonTile", func, finalizeBlock);
    auto isInBottomTile = builder.CreateICmpUGE(threadGroupFlatId, bottomStart);
    builder.CreateCondBr(isInBottomTile, bottomTileBlock, bottomTileElseIfBlock);

    {
      // Bottom tile
      builder.SetInsertPoint(bottomTileBlock);
      // Get new thread group ID for thread group in the bottom section
      // Thread groups are reordered up->down then left->right

      // localThreadGroupFlatId = threadGroupFlatId - bottomStart
      // swizzledWorkgroupId.x = localThreadGroupFlatId / bottomHeight
      // swizzledWorkgroupId.y = (localThreadGroupFlatId % bottomHeight) + numSwizzledThreadGroup.y
      auto localThreadGroupFlatId = builder.CreateSub(threadGroupFlatId, bottomStart);
      auto swizzledWorkgroupId = builder.CreateInsertElement(
          PoisonValue::get(ivec3Ty), builder.CreateUDiv(localThreadGroupFlatId, bottomHeight), uint64_t(0));
      swizzledWorkgroupId =
          builder.CreateInsertElement(swizzledWorkgroupId,
                                      builder.CreateAdd(builder.CreateURem(localThreadGroupFlatId, bottomHeight),
                                                        builder.CreateExtractElement(numSwizzledThreadGroup, 1)),
                                      1);

      builder.CreateStore(swizzledWorkgroupId, swizzledWorkgroupIdPtr);
      builder.CreateBr(finalizeBlock);
    }
    {
      // else if (threadGroupFlatId >= sideStart)
      builder.SetInsertPoint(bottomTileElseIfBlock);

      auto isInSideTile = builder.CreateICmpUGE(threadGroupFlatId, sideStart);
      builder.CreateCondBr(isInSideTile, sideTileBlock, mortonTileBlock);
    }
    {
      // Side tile
      builder.SetInsertPoint(sideTileBlock);

      // Get new thread group ID for thread group in the side section
      // Thread groups are reordered left->right then up->down

      // localThreadGroupFlatId = threadGroupFlatId - sideStart
      // swizzledWorkgroupId.x = (localThreadGroupFlatId % sideWidth) + numSwizzledThreadGroup.x
      // swizzledWorkgroupId.y = localThreadGroupFlatId / sideWidth
      auto localThreadGroupFlatId = builder.CreateSub(threadGroupFlatId, sideStart);
      auto swizzledWorkgroupId = builder.CreateInsertElement(
          PoisonValue::get(ivec3Ty),
          builder.CreateAdd(builder.CreateURem(localThreadGroupFlatId, sideWidth),
                            builder.CreateExtractElement(numSwizzledThreadGroup, uint64_t(0))),
          uint64_t(0));
      swizzledWorkgroupId =
          builder.CreateInsertElement(swizzledWorkgroupId, builder.CreateUDiv(localThreadGroupFlatId, sideWidth), 1);

      builder.CreateStore(swizzledWorkgroupId, swizzledWorkgroupIdPtr);
      builder.CreateBr(finalizeBlock);
    }
    {
      // Morton tile
      builder.SetInsertPoint(mortonTileBlock);

      // Helper to compact bits for Z-order curve
      auto createCompact1By1Bits = [&](unsigned bitsToExtract, Value *src) {
        auto createCompactShift = [&](unsigned shift, unsigned mask, Value *src) {
          auto result = builder.CreateLShr(src, builder.getInt32(shift));
          result = builder.CreateOr(result, src);
          result = builder.CreateAnd(result, builder.getInt32(mask));
          return result;
        };

        // x &= 0x55555555;                   // x = -f-e -d-c -b-a -9-8 -7-6 -5-4 -3-2 -1-0
        auto result = builder.CreateAnd(src, builder.getInt32(0x55555555));

        // x = (x | (x >> 1)) & 0x33333333;   // x = --fe --dc --ba --98 --76 --54 --32 --10 // NOLINT
        result = createCompactShift(1, 0x33333333, result);

        if (bitsToExtract > 2)
          // x = (x | (x >> 2)) & 0x0F0F0F0F; // x = ---- fedc ---- ba98 ---- 7654 ---- 3210 // NOLINT
          result = createCompactShift(2, 0x0F0F0F0F, result);

        if (bitsToExtract > 4)
          // x = (x | (x >> 4)) & 0x00FF00FF; // x = ---- ---- fedc ba98 ---- ---- 7654 3210 // NOLINT
          result = createCompactShift(4, 0x00FF00FF, result);

        if (bitsToExtract > 8)
          // x = (x | (x >> 8)) & 0x0000FFFF; // x = ---- ---- ---- ---- fedc ba98 7654 3210 // NOLINT
          result = createCompactShift(8, 0x0000FFFF, result);

        return result;
      };

      // localThreadGroupFlatId = threadGroupFlatId % tileSize
      auto localThreadGroupFlatId = builder.CreateURem(threadGroupFlatId, tileSize);

      // Extract to xy dimension based on Z-order curved
      auto localThreadGroupIdX = createCompact1By1Bits(tileBits[tileIndex], localThreadGroupFlatId);
      auto localThreadGroupIdY =
          createCompact1By1Bits(tileBits[tileIndex], builder.CreateLShr(localThreadGroupFlatId, one));

      // flatTileId = threadGroupFlatId / tileSize
      auto flatTileId = builder.CreateUDiv(threadGroupFlatId, tileSize);

      // swizzledWorkgroupId.x = (flatTileId % numTiles.x) * tileDim + localThreadGroupId.x
      // swizzledWorkgroupId.y = (flatTileId / numTiles.x) * tileDim + localThreadGroupId.y
      auto swizzledWorkgroupIdX = builder.CreateAdd(
          builder.CreateMul(builder.CreateURem(flatTileId, builder.CreateExtractElement(numTiles, uint64_t(0))),
                            tileDim),
          localThreadGroupIdX);
      auto swizzledWorkgroupIdY = builder.CreateAdd(
          builder.CreateMul(builder.CreateUDiv(flatTileId, builder.CreateExtractElement(numTiles, uint64_t(0))),
                            tileDim),
          localThreadGroupIdY);

      auto swizzledWorkgroupId =
          builder.CreateInsertElement(PoisonValue::get(ivec3Ty), swizzledWorkgroupIdX, uint64_t(0));
      swizzledWorkgroupId = builder.CreateInsertElement(swizzledWorkgroupId, swizzledWorkgroupIdY, 1);

      builder.CreateStore(swizzledWorkgroupId, swizzledWorkgroupIdPtr);
      builder.CreateBr(finalizeBlock);
    }

    // Finalize
    builder.SetInsertPoint(finalizeBlock);

    // swizzledWorkgroupId.z = nativeWorkgroupId.z
    Value *swizzledWorkgroupId = builder.CreateLoad(ivec3Ty, swizzledWorkgroupIdPtr);
    swizzledWorkgroupId =
        builder.CreateInsertElement(swizzledWorkgroupId, builder.CreateExtractElement(nativeWorkgroupId, 2), 2);

    builder.CreateStore(swizzledWorkgroupId, swizzledWorkgroupIdPtr);

    builder.CreateBr(returnBlock);
  }
  {
    // Disable swizzle
    builder.SetInsertPoint(disableSwizzleBlock);

    builder.CreateStore(nativeWorkgroupId, swizzledWorkgroupIdPtr);

    builder.CreateBr(returnBlock);
  }

  // Return
  builder.SetInsertPoint(returnBlock);

  auto swizzledWorkgroupId = builder.CreateLoad(ivec3Ty, swizzledWorkgroupIdPtr);
  builder.CreateRet(swizzledWorkgroupId);
}

// =====================================================================================================================
// Exports HW shading rate, extracting the values from LGC shading rate (a mask of ShadingRateFlags)
//
// @param shadingRate : LGC shading rate
// @param builder : the builder to use
void LowerInOut::exportShadingRate(Value *shadingRate, BuilderBase &builder) {
  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  Value *hwShadingRate = nullptr;

  if (m_gfxIp.major >= 11) {
    // NOTE: In GFX11, the graphics pipeline is to support VRS rates till 4x4 which includes 2x4 and 4x2 along with
    // the legacy rates. And 1x4 and 4x1 are not supported, hence clamp 1x4 and 4x1 to 1x2 and 2x1 respectively.
    // The HW shading rate representations are as following:
    //     SHADING_RATE_1x1    0x0
    //     SHADING_RATE_1x2    0x1
    //     SHADING_RATE_2x1    0x4
    //     SHADING_RATE_2x2    0x5
    //     SHADING_RATE_2x4    0x6
    //     SHADING_RATE_4x2    0x9
    //     SHADING_RATE_4x4    0xA
    //
    // [5:2] = HW rate enum
    // hwShadingRate = shadingRate & (ShadingRateHorizontal2Pixels | ShadingRateHorizontal4Pixels |
    //                                 ShadingRateVertical2Pixels | ShadingRateVertical4Pixels)
    hwShadingRate =
        builder.CreateAnd(shadingRate, builder.getInt32(ShadingRateHorizontal2Pixels | ShadingRateHorizontal4Pixels |
                                                        ShadingRateVertical2Pixels | ShadingRateVertical4Pixels));

    // hwShadingRate = hwShadingRate == 1x4 ? 1x2 : hwShadingRate
    Value *shadingRate1x4 = builder.CreateICmpEQ(hwShadingRate, builder.getInt32(2));
    hwShadingRate = builder.CreateSelect(shadingRate1x4, builder.getInt32(1), hwShadingRate);

    // hwShadingRate = hwShadingRate == 4x1 ? 2x1 : hwShadingRate
    Value *shadingRate4x1 = builder.CreateICmpEQ(hwShadingRate, builder.getInt32(8));
    hwShadingRate = builder.CreateSelect(shadingRate4x1, builder.getInt32(4), hwShadingRate);

    // hwShadingRate = hwShadingRate << 2
    hwShadingRate = builder.CreateShl(hwShadingRate, 2);
    hwShadingRate = builder.CreateBitCast(hwShadingRate, builder.getFloatTy());
  } else {
    // NOTE: The shading rates have different meanings in HW and LGC interface. Current HW only supports 2-pixel mode
    // and 4-pixel mode is not supported. But the spec requires us to accept unsupported rates and clamp them to
    // maxFragmentSize of HW. The mapping is therefore as follow:
    //
    //   VRS X rate: MaskNone -> 0b00, Horizontal2Pixels | Horizontal4Pixels -> 0b01
    //   VRS Y rate: MaskNone -> 0b00, Vertical2Pixels | Vertical4Pixels -> 0b01
    //
    // xRate = (shadingRate & (Horizontal2Pixels | Horizontal4Pixels) ? 0x1 : 0x0
    Value *xRate2Pixels =
        builder.CreateAnd(shadingRate, builder.getInt32(ShadingRateHorizontal2Pixels | ShadingRateHorizontal4Pixels));
    xRate2Pixels = builder.CreateICmpNE(xRate2Pixels, builder.getInt32(0));
    Value *xRate = builder.CreateSelect(xRate2Pixels, builder.getInt32(1), builder.getInt32(0));

    // yRate = (shadingRate & (Vertical2Pixels | Vertical4Pixels)) ? 0x1 : 0x0
    Value *yRate2Pixels =
        builder.CreateAnd(shadingRate, builder.getInt32(ShadingRateVertical2Pixels | ShadingRateVertical4Pixels));
    yRate2Pixels = builder.CreateICmpNE(yRate2Pixels, builder.getInt32(0));
    Value *yRate = builder.CreateSelect(yRate2Pixels, builder.getInt32(1), builder.getInt32(0));

    // [5:4] = Y rate, [3:2] = X rate
    // hwShadingRate = (xRate << 2) | (yRate << 4)
    xRate = builder.CreateShl(xRate, 2);
    yRate = builder.CreateShl(yRate, 4);
    hwShadingRate = builder.CreateOr(xRate, yRate);
    hwShadingRate = builder.CreateBitCast(hwShadingRate, builder.getFloatTy());
  }

  auto poison = PoisonValue::get(builder.getFloatTy());
  exportPosition(1, {poison, hwShadingRate, poison, poison}, builder);
}

// =====================================================================================================================
// Gets HW primitive type from ancillary bits.
Value *LowerInOut::getPrimType(BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Fragment);
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

  // Prim Type = Ancillary[1:0]
  return builder.CreateAnd(ancillary, 0x3);
}

// =====================================================================================================================
// Gets HW line stipple value from lineStipple value.
//
// @param builder : the builder to use
Value *LowerInOut::getLineStipple(BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Fragment);
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  auto line_stipple = getFunctionArgument(m_entryPoint, entryArgIdxs.lineStipple);

  return builder.CreateBitCast(line_stipple, builder.getFloatTy());
}

// =====================================================================================================================
// Gets HW shading rate and converts them to LGC definitions.
//
// @param builder : the builder to use
Value *LowerInOut::getShadingRate(BuilderBase &builder) {
  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  assert(m_shaderStage == ShaderStage::Fragment);
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

  // Y rate = Ancillary[5:4], X rate = Ancillary[3:2]
  Value *xRate = builder.CreateAnd(ancillary, 0xC);
  xRate = builder.CreateLShr(xRate, 2);
  Value *yRate = builder.CreateAnd(ancillary, 0x30);
  yRate = builder.CreateLShr(yRate, 4);

  Value *shadingRate = nullptr;

  if (m_gfxIp.major >= 11) {
    // xRate = xRate == 0x1 ? Horizontal2Pixels : None
    auto xRate2Pixels = builder.CreateICmpEQ(xRate, builder.getInt32(1));
    xRate = builder.CreateSelect(xRate2Pixels, xRate, builder.getInt32(0));

    // yRate = yRate == 0x1 ? Vertical2Pixels : None
    auto yRate2Pixels = builder.CreateICmpEQ(yRate, builder.getInt32(1));
    yRate = builder.CreateSelect(yRate2Pixels, yRate, builder.getInt32(0));

    // shadingRate = (xRate << 2) | yRate
    shadingRate = builder.CreateOr(builder.CreateShl(xRate, 2), yRate);
  } else {
    // NOTE: The shading rates have different meanings in HW and LGC interface. Current HW only supports 2-pixel mode
    // and 4-pixel mode is not supported. The mapping is as follow:
    //
    //   VRS X rate: 0b00 -> MaskNone, 0b01 -> Horizontal2Pixels
    //   VRS Y rate: 0b00 -> MaskNone, 0b01 -> Vertical2Pixels
    //
    // xRate = xRate == 0x1 ? Horizontal2Pixels : None
    auto xRate2Pixels = builder.CreateICmpEQ(xRate, builder.getInt32(1));
    xRate = builder.CreateSelect(xRate2Pixels, builder.getInt32(ShadingRateHorizontal2Pixels),
                                 builder.getInt32(ShadingRateNone));

    // yRate = yRate == 0x1 ? Vertical2Pixels : None
    auto yRate2Pixels = builder.CreateICmpEQ(yRate, builder.getInt32(1));
    yRate = builder.CreateSelect(yRate2Pixels, builder.getInt32(ShadingRateVertical2Pixels),
                                 builder.getInt32(ShadingRateNone));

    // shadingRate = xRate | yRate
    shadingRate = builder.CreateOr(xRate, yRate);
  }

  return shadingRate;
}

// =====================================================================================================================
// Record export info of vertex attributes.
//
// @param exportSlot : Export slot
// @param exportValues : Values of this vertex attribute to export
void LowerInOut::recordVertexAttribute(unsigned exportSlot, ArrayRef<Value *> exportValues) {
  assert(m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
         m_shaderStage == ShaderStage::CopyShader); // Valid shader stages
  assert(exportSlot <= MaxInOutLocCount);           // 32 attributes at most
  assert(exportValues.size() == 4);                 // Must have 4 elements, corresponds to <4 x float>

  auto poison = PoisonValue::get(Type::getFloatTy(*m_context));

  // Vertex attribute not existing, insert a new one and initialize it
  if (m_attribExports.count(exportSlot) == 0) {
    for (unsigned i = 0; i < 4; ++i)
      m_attribExports[exportSlot][i] = poison;
  }

  for (unsigned i = 0; i < 4; ++i) {
    assert(exportValues[i]);
    if (isa<UndefValue>(exportValues[i]) || isa<PoisonValue>(exportValues[i]))
      continue; // Here, we only record new attribute values that are valid (not unspecified ones)

    // NOTE: The existing values must have been initialized to unspecified ones already. Overlapping is disallowed (see
    // such cases):
    //   - Valid:
    //       Existing: attrib0, <1.0, 2.0, undef/poison, undef/poison>
    //       New:      attrib0, <undef/poison, undef/poison, 3.0, 4.0>
    //   - Invalid:
    //       Existing: attrib0, <1.0, 2.0, 3.0, undef/poison>
    //       New:      attrib0, <undef/poison, undef/poison, 4.0, 5.0>
    assert(isa<UndefValue>(m_attribExports[exportSlot][i]) || isa<PoisonValue>(m_attribExports[exportSlot][i]));
    m_attribExports[exportSlot][i] = exportValues[i]; // Update values that are valid
  }

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage.value())->inOutUsage;
  inOutUsage.expCount = std::max(inOutUsage.expCount, exportSlot + 1); // Update export count
}

// =====================================================================================================================
// Export vertex attributes that were recorded previously.
//
// @param builder : IR builder
void LowerInOut::exportAttributes(BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
         m_shaderStage == ShaderStage::CopyShader); // Valid shader stages
  if (m_attribExports.empty()) {
    assert(m_pipelineState->getShaderResourceUsage(m_shaderStage.value())->inOutUsage.expCount == 0);
    return;
  }

  for (auto &attribExport : m_attribExports) {
    const auto &[exportSlot, exportValues] = attribExport;
    assert(exportValues.size() == 4); // Must be <4 x float>

    if (m_pipelineState->getNggControl()->enableNgg) {
      builder.create<NggExportAttributeOp>(exportSlot, exportValues[0], exportValues[1], exportValues[2],
                                           exportValues[3]);
    } else {
      unsigned channelMask = 0;
      for (unsigned i = 0; i < 4; ++i) {
        assert(exportValues[i]);
        if (!isa<UndefValue>(exportValues[i]) && !isa<PoisonValue>(exportValues[i]))
          channelMask |= (1u << i); // Update channel mask if the value is valid (not unspecified)
      }

      builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(),
                              {builder.getInt32(EXP_TARGET_PARAM_0 + exportSlot), // tgt
                               builder.getInt32(channelMask),                     // en
                               exportValues[0],                                   // src0
                               exportValues[1],                                   // src1
                               exportValues[2],                                   // src2
                               exportValues[3],                                   // src3
                               builder.getFalse(),                                // done
                               builder.getFalse()});                              // vm
    }
  }
}

// =====================================================================================================================
static Value *adjustIj(Value *value, Value *offset, BuilderImpl &builder) {
  offset = builder.CreateFPExt(offset, FixedVectorType::get(builder.getFloatTy(), 2));
  Value *offsetX = builder.CreateExtractElement(offset, uint64_t(0));
  Value *offsetY = builder.CreateExtractElement(offset, 1);
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    offsetX = builder.CreateVectorSplat(vecTy->getNumElements(), offsetX);
    offsetY = builder.CreateVectorSplat(vecTy->getNumElements(), offsetY);
  }
  Value *derivX = builder.CreateDerivative(value, /*isY=*/false, /*isFine=*/true);
  Value *derivY = builder.CreateDerivative(value, /*isY=*/true, /*isFine=*/true);
  Value *adjustX = builder.CreateFAdd(value, builder.CreateFMul(derivX, offsetX));
  Value *adjustY = builder.CreateFAdd(adjustX, builder.CreateFMul(derivY, offsetY));
  return adjustY;
}

// =====================================================================================================================
// Evaluate I,J for interpolation: center offset, smooth (perspective) version
void LowerInOut::visitEvalIjOffsetSmoothOp(EvalIjOffsetSmoothOp &op) {
  BuilderBase builderBase(&op);
  // Get <I/W, J/W, 1/W>
  Value *pullModel = readFsBuiltInInput(FixedVectorType::get(builderBase.getFloatTy(), 3), BuiltInInterpPullMode,
                                        nullptr, builderBase);
  BuilderImpl builder(m_pipelineState);
  builder.SetInsertPoint(builderBase.GetInsertPoint());
  builder.setFastMathFlags(op.getFastMathFlags());
  // Adjust each coefficient by offset.
  Value *adjusted = adjustIj(pullModel, op.getValue(), builder);
  // Extract <I/W, J/W, 1/W> part of that
  Value *ijDivW = builder.CreateShuffleVector(adjusted, adjusted, ArrayRef<int>{0, 1});
  Value *rcpW = builder.CreateExtractElement(adjusted, 2);
  // Get W by making a reciprocal of 1/W
  Value *w = builder.CreateFDiv(ConstantFP::get(builder.getFloatTy(), 1.0), rcpW);
  w = builder.CreateVectorSplat(2, w);
  auto res = builder.CreateFMul(ijDivW, w);

  op.replaceAllUsesWith(res);
  op.eraseFromParent();
}

// =====================================================================================================================
// Adjusts value by its X and Y derivatives times the X and Y components of offset.
void LowerInOut::visitAdjustIjOp(AdjustIjOp &op) {
  BuilderImpl builder(m_pipelineState);
  builder.SetInsertPoint(&op);
  builder.setFastMathFlags(op.getFastMathFlags());
  Value *adjusted = adjustIj(op.getValue(), op.getOffset(), builder);

  op.replaceAllUsesWith(adjusted);
  op.eraseFromParent();
}

// =====================================================================================================================
// Export vertex position.
//
// @param exportSlot : Export slot
// @param exportValues : Vertex position values to export
// @param builder : IR builder
void LowerInOut::exportPosition(unsigned exportSlot, ArrayRef<llvm::Value *> exportValues, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStage::Vertex || m_shaderStage == ShaderStage::TessEval ||
         m_shaderStage == ShaderStage::CopyShader); // Valid shader stages
  assert(exportValues.size() == 4);                 // Must be <4 x float>

  if (m_pipelineState->getNggControl()->enableNgg) {
    builder.create<NggExportPositionOp>(exportSlot, exportValues[0], exportValues[1], exportValues[2], exportValues[3]);
  } else {
    unsigned channelMask = 0;
    for (unsigned i = 0; i < 4; ++i) {
      assert(exportValues[i]);
      if (!isa<UndefValue>(exportValues[i]) && !isa<PoisonValue>(exportValues[i]))
        channelMask |= (1u << i); // Update channel mask if the value is valid (not unspecified)
    }

    builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(),
                            {builder.getInt32(EXP_TARGET_POS_0 + exportSlot), // tgt
                             builder.getInt32(channelMask),                   // en
                             exportValues[0],                                 // src0
                             exportValues[1],                                 // src1
                             exportValues[2],                                 // src2
                             exportValues[3],                                 // src3
                             builder.getFalse(),                              // done
                             builder.getFalse()});                            // vm
  }
}

} // namespace lgc
