/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchInOutImportExport.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchInOutImportExport.
 *
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchInOutImportExport.h"
#include "Gfx6Chip.h"
#include "Gfx9Chip.h"
#include "lgc/Builder.h"
#include "lgc/BuiltIns.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>

#define DEBUG_TYPE "lgc-patch-in-out-import-export"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
PatchInOutImportExport::PatchInOutImportExport() : m_lds(nullptr) {
  memset(&m_gfxIp, 0, sizeof(m_gfxIp));
  initPerShader();
}

// =====================================================================================================================
// Initialize per-shader members
void PatchInOutImportExport::initPerShader() {
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
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchInOutImportExport::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  auto getPDT = [&](Function &f) -> PostDominatorTree & {
    auto &fam = analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
    return fam.getResult<PostDominatorTreeAnalysis>(f);
  };
  if (runImpl(module, pipelineShaders, pipelineState, getPDT))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @param getPostDominatorTree : Function to get the PostDominatorTree of the given Function object
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchInOutImportExport::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                     PipelineState *pipelineState,
                                     const std::function<PostDominatorTree &(Function &)> &getPostDominatorTree) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-In-Out-Import-Export\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  m_pipelineSysValues.initialize(m_pipelineState);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = (stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0;
  m_hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;

  SmallVector<Function *, 16> inputCallees, otherCallees;
  for (auto &func : module.functions()) {
    auto name = func.getName();
    if (name.startswith("lgc.input"))
      inputCallees.push_back(&func);
    else if (name.startswith("lgc.output") || name == "llvm.amdgcn.s.sendmsg")
      otherCallees.push_back(&func);
  }

  // Create the global variable that is to model LDS
  // NOTE: ES -> GS ring is always on-chip on GFX9.
  if (m_hasTs || (m_hasGs && (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9)))
    m_lds = Patch::getLdsVariable(m_pipelineState, m_module);

  // Set buffer formats based on specific GFX
  static const std::array<unsigned char, 4> BufferFormatsGfx9 = {
      BUF_NUM_FORMAT_FLOAT << 4 | BUF_DATA_FORMAT_32,
      BUF_NUM_FORMAT_FLOAT << 4 | BUF_DATA_FORMAT_32_32,
      BUF_NUM_FORMAT_FLOAT << 4 | BUF_DATA_FORMAT_32_32_32,
      BUF_NUM_FORMAT_FLOAT << 4 | BUF_DATA_FORMAT_32_32_32_32,
  };
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
  default:
    m_buffFormats = &BufferFormatsGfx9;
    break;
  case 10:
    m_buffFormats = &BufferFormatsGfx10;
    break;
  case 11:
    m_buffFormats = &BufferFormatsGfx11;
    break;
  }

  // Process each shader in turn, in reverse order (because for example VS uses inOutUsage.tcs.calcFactor
  // set by TCS).
  for (int shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage) {
    auto entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (entryPoint) {
      processFunction(*entryPoint, static_cast<ShaderStage>(shaderStage), inputCallees, otherCallees,
                      getPostDominatorTree);
    }
  }

  // Process non-entry-point shaders
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;
    auto shaderStage = getShaderStage(&func);
    if (shaderStage == ShaderStage::ShaderStageInvalid || &func == pipelineShaders.getEntryPoint(shaderStage))
      continue;
    processFunction(func, shaderStage, inputCallees, otherCallees, getPostDominatorTree);
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

  m_pipelineSysValues.clear();

  return true;
}

void PatchInOutImportExport::processFunction(
    Function &func, ShaderStage shaderStage, SmallVectorImpl<Function *> &inputCallees,
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
void PatchInOutImportExport::markExportDone(Function *func, PostDominatorTree &postDomTree) {
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
void PatchInOutImportExport::processShader() {
  // Initialize the output value for gl_PrimitiveID
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->builtInUsage;
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
  if (m_shaderStage == ShaderStageVertex) {
    if (builtInUsage.vs.primitiveId)
      m_primitiveId = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.primitiveId);
  } else if (m_shaderStage == ShaderStageTessEval) {
    if (builtInUsage.tes.primitiveId) {
      m_primitiveId = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.patchId);
    }
  }

  // Thread ID will be used in on-chip GS offset calculation (ES -> GS ring is always on-chip on GFX9)
  bool useThreadId = (m_hasGs && (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9));

  // Thread ID will also be used for stream-out buffer export
  const bool enableXfb = m_pipelineState->enableXfb();
  useThreadId = useThreadId || enableXfb;

  if (useThreadId) {
    // Calculate and store thread ID
    BuilderBase builder(*m_context);
    builder.SetInsertPointPastAllocas(m_entryPoint);
    m_threadId = getSubgroupLocalInvocationId(builder);
  }

  // Initialize calculation factors for tessellation shader
  if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval) {
    const unsigned stageMask = m_pipelineState->getShaderStageMask();
    const bool hasTcs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0);

    auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    if (!calcFactor.initialized) {
      calcFactor.initialized = true;

      //
      // NOTE: The LDS for tessellation is as follow:
      //
      //          +-------------+--------------+-------------+-------------+
      // On-chip  | Input Patch | Output Patch | Patch Const | Tess Factor | (LDS)
      //          +-------------+--------------+-------------+-------------+
      //
      //          +-------------+-------------+----------------+------------------+
      // Off-chip | Input Patch | Tess Factor | HS Patch Count | Special TF Value | (LDS)
      //          +-------------+-------------+----------------+------------------+
      //          +--------------+-------------+
      //          | Output Patch | Patch Const | (LDS Buffer)
      //          +--------------+-------------+
      //
      // inPatchTotalSize = inVertexCount * inVertexStride * patchCountPerThreadGroup
      // outPatchTotalSize = outVertexCount * outVertexStride * patchCountPerThreadGroup
      // patchConstTotalSize = patchConstCount * 4 * patchCountPerThreadGroup
      // tessFactorTotalSize = 6 * patchCountPerThreadGroup
      //
      const auto &tcsInOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage;
      const auto &tesInOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->inOutUsage;

      const unsigned inLocCount = std::max(tcsInOutUsage.inputMapLocCount, 1u);
      const unsigned outLocCount =
          hasTcs ? std::max(tcsInOutUsage.outputMapLocCount, 1u) : std::max(tesInOutUsage.inputMapLocCount, 1u);

      const unsigned inVertexCount = m_pipelineState->getNumPatchControlPoints();
      const unsigned outVertexCount =
          hasTcs ? m_pipelineState->getShaderModes()->getTessellationMode().outputVertices : MaxTessPatchVertices;

      unsigned tessFactorStride = 0;
      switch (m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode) {
      case PrimitiveMode::Triangles:
        tessFactorStride = 4;
        break;
      case PrimitiveMode::Quads:
        tessFactorStride = 6;
        break;
      case PrimitiveMode::Isolines:
        tessFactorStride = 2;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }

      calcFactor.inVertexStride = inLocCount * 4;
      calcFactor.outVertexStride = outLocCount * 4;

      const unsigned patchConstCount =
          hasTcs ? tcsInOutUsage.perPatchOutputMapLocCount : tesInOutUsage.perPatchInputMapLocCount;
      calcFactor.patchConstSize = patchConstCount * 4;

      calcFactor.patchCountPerThreadGroup =
          calcPatchCountPerThreadGroup(inVertexCount, calcFactor.inVertexStride, outVertexCount,
                                       calcFactor.outVertexStride, patchConstCount, tessFactorStride);

      const unsigned inPatchSize = inVertexCount * calcFactor.inVertexStride;
      const unsigned inPatchTotalSize = calcFactor.patchCountPerThreadGroup * inPatchSize;

      const unsigned outPatchSize = outVertexCount * calcFactor.outVertexStride;
      const unsigned outPatchTotalSize = calcFactor.patchCountPerThreadGroup * outPatchSize;

      const unsigned patchConstTotalSize = calcFactor.patchCountPerThreadGroup * calcFactor.patchConstSize;
      const unsigned tessFactorTotalSize = calcFactor.patchCountPerThreadGroup * MaxTessFactorsPerPatch;

      calcFactor.outPatchSize = outPatchSize;
      calcFactor.inPatchSize = inPatchSize;

      // NOTE: Tess factors are always stored to on-chip LDS first. Then, they are store to TF buffer and on-chip LDS
      // or off-chip LDS buffer (which will be loaded by TES).
      if (m_pipelineState->isTessOffChip()) {
        calcFactor.offChip.outPatchStart = 0;
        calcFactor.offChip.patchConstStart = calcFactor.offChip.outPatchStart + outPatchTotalSize;

        calcFactor.onChip.tessFactorStart = inPatchTotalSize;
      } else {
        calcFactor.onChip.outPatchStart = inPatchTotalSize;
        calcFactor.onChip.patchConstStart = calcFactor.onChip.outPatchStart + outPatchTotalSize;
        calcFactor.onChip.tessFactorStart = calcFactor.onChip.patchConstStart + patchConstTotalSize;
      }

      calcFactor.tessFactorStride = tessFactorStride;
      calcFactor.tessOnChipLdsSize = calcFactor.onChip.tessFactorStart + tessFactorTotalSize;

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
        calcFactor.onChip.hsPatchCountStart = calcFactor.tessOnChipLdsSize; // One dword to store actual HS wave count
        calcFactor.onChip.specialTfValueStart = calcFactor.onChip.hsPatchCountStart + 1;

        const unsigned maxNumHsWaves =
            Gfx9::MaxHsThreadsPerSubgroup / m_pipelineState->getMergedShaderWaveSize(ShaderStageTessControl);
        calcFactor.specialTfValueSize = maxNumHsWaves * 2;

        calcFactor.tessOnChipLdsSize += 1 + calcFactor.specialTfValueSize;
      }

      // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
      // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
      // restriction by providing the capability of querying thread ID in group rather than in wave.
      const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
      const auto tcsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
      if (vsResUsage->useRayQueryLdsStack || tcsResUsage->useRayQueryLdsStack)
        calcFactor.rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;

      LLPC_OUTS("===============================================================================\n");
      LLPC_OUTS("// LLPC tessellation calculation factor results\n\n");
      LLPC_OUTS("Patch count per thread group: " << calcFactor.patchCountPerThreadGroup << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Input vertex count: " << inVertexCount << "\n");
      LLPC_OUTS("Input vertex stride: " << calcFactor.inVertexStride << "\n");
      LLPC_OUTS("Input patch size (in dwords): " << inPatchSize << "\n");
      LLPC_OUTS("Input patch start: 0 (LDS)\n");
      LLPC_OUTS("Input patch total size (in dwords): " << inPatchTotalSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Output vertex count: " << outVertexCount << "\n");
      LLPC_OUTS("Output vertex stride: " << calcFactor.outVertexStride << "\n");
      LLPC_OUTS("Output patch size (in dwords): " << outPatchSize << "\n");
      LLPC_OUTS("Output patch start: " << (m_pipelineState->isTessOffChip() ? calcFactor.offChip.outPatchStart
                                                                            : calcFactor.onChip.outPatchStart)
                                       << (m_pipelineState->isTessOffChip() ? " (LDS buffer)" : "(LDS)") << "\n");
      LLPC_OUTS("Output patch total size (in dwords): " << outPatchTotalSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Patch constant count: " << patchConstCount << "\n");
      LLPC_OUTS("Patch constant size (in dwords): " << calcFactor.patchConstSize << "\n");
      LLPC_OUTS("Patch constant start: " << (m_pipelineState->isTessOffChip() ? calcFactor.offChip.patchConstStart
                                                                              : calcFactor.onChip.patchConstStart)
                                         << (m_pipelineState->isTessOffChip() ? " (LDS buffer)" : "(LDS)") << "\n");
      LLPC_OUTS("Patch constant total size (in dwords): " << patchConstTotalSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Tess factor start: " << calcFactor.onChip.tessFactorStart << " (LDS)\n");
      LLPC_OUTS("Tess factor total size (in dwords): " << tessFactorTotalSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("HS patch count start: " << calcFactor.onChip.hsPatchCountStart << " (LDS)\n");
      LLPC_OUTS("HS wave count size (in dwords): " << 1 << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Special TF value start: " << calcFactor.onChip.specialTfValueStart << " (LDS)\n");
      LLPC_OUTS("Special TF value size (in dwords): " << calcFactor.specialTfValueSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Tess factor stride: " << tessFactorStride << " (");
      switch (m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode) {
      case PrimitiveMode::Triangles:
        LLPC_OUTS("triangles");
        break;
      case PrimitiveMode::Quads:
        LLPC_OUTS("quads");
        break;
      case PrimitiveMode::Isolines:
        LLPC_OUTS("isolines");
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
      LLPC_OUTS(")\n\n");
      LLPC_OUTS("Tess on-chip LDS total size (in dwords): " << calcFactor.tessOnChipLdsSize << "\n");
      if (calcFactor.rayQueryLdsStackSize > 0) {
        LLPC_OUTS("Ray query LDS stack size (in dwords): " << calcFactor.rayQueryLdsStackSize
                                                           << " (start = " << calcFactor.tessOnChipLdsSize << ")\n");
      }
      LLPC_OUTS("\n");
    }
  }

  if (m_shaderStage == ShaderStageCompute) {
    // In a compute shader, process lgc.reconfigure.local.invocation.id calls.
    // This does not particularly have to be done here; it could be done anywhere after BuilderImpl.
    for (Function &func : *m_module) {
      auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();

      // Different with above, this will force the threadID swizzle which will rearrange thread ID within a group into
      // blocks of 8*4, not to reconfig workgroup automatically and will support to be swizzled in 8*4 block
      // split.
      if (func.isDeclaration() && func.getName().startswith(lgcName::ReconfigureLocalInvocationId)) {
        unsigned workgroupSizeX = mode.workgroupSizeX;
        unsigned workgroupSizeY = mode.workgroupSizeY;
        unsigned workgroupSizeZ = mode.workgroupSizeZ;
        SwizzleWorkgroupLayout layout = calculateWorkgroupLayout();
        while (!func.use_empty()) {
          CallInst *reconfigCall = cast<CallInst>(*func.user_begin());
          Value *localInvocationId = reconfigCall->getArgOperand(0);
          bool isHwLocalInvocationId = dyn_cast<ConstantInt>(reconfigCall->getArgOperand(1))->getZExtValue();
          if ((layout.microLayout == WorkgroupLayout::Quads) ||
              (layout.macroLayout == WorkgroupLayout::SexagintiQuads)) {
            localInvocationId =
                reconfigWorkgroupLayout(localInvocationId, layout.macroLayout, layout.microLayout, workgroupSizeX,
                                        workgroupSizeY, workgroupSizeZ, isHwLocalInvocationId, reconfigCall);
          }
          reconfigCall->replaceAllUsesWith(localInvocationId);
          reconfigCall->eraseFromParent();
        }
      }

      if (func.isDeclaration() && func.getName().startswith(lgcName::SwizzleWorkgroupId)) {
        createSwizzleThreadGroupFunction();
      }
    }
  }
}

// =====================================================================================================================
// Visits all "call" instructions against the callee functions in current entry-point function.
//
// @param calleeFuncs : a list of candidate callee functions to check
void PatchInOutImportExport::visitCallInsts(ArrayRef<Function *> calleeFuncs) {
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
void PatchInOutImportExport::visitReturnInsts() {
  for (auto &block : *m_entryPoint)
    if (auto *retInst = dyn_cast<ReturnInst>(block.getTerminator()))
      visitReturnInst(*retInst);
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void PatchInOutImportExport::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(&callInst);

  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  auto mangledName = callee->getName();

  auto importBuiltInInput = lgcName::InputImportBuiltIn;
  auto importBuiltInOutput = lgcName::OutputImportBuiltIn;

  const bool isGenericInputImport = isa<InputImportGenericOp>(callInst);
  const bool isBuiltInInputImport = mangledName.startswith(importBuiltInInput);
  const bool isInterpolatedInputImport = isa<InputImportInterpolatedOp>(callInst);
  const bool isGenericOutputImport = isa<OutputImportGenericOp>(callInst);
  const bool isBuiltInOutputImport = mangledName.startswith(importBuiltInOutput);

  const bool isImport = (isGenericInputImport || isBuiltInInputImport || isInterpolatedInputImport ||
                         isGenericOutputImport || isBuiltInOutputImport);

  auto exportGenericOutput = lgcName::OutputExportGeneric;
  auto exportBuiltInOutput = lgcName::OutputExportBuiltIn;
  auto exportXfbOutput = lgcName::OutputExportXfb;

  const bool isGenericOutputExport = mangledName.startswith(exportGenericOutput);
  const bool isBuiltInOutputExport = mangledName.startswith(exportBuiltInOutput);
  const bool isXfbOutputExport = mangledName.startswith(exportXfbOutput);

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

      switch (m_shaderStage) {
      case ShaderStageVertex:
        // Nothing to do
        break;
      case ShaderStageTessControl: {
        // Builtin Call has different number of operands
        Value *elemIdx = nullptr;
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.arg_size() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

        input = patchTcsBuiltInInputImport(inputTy, builtInId, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStageTessEval: {
        // Builtin Call has different number of operands
        Value *elemIdx = nullptr;
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.arg_size() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);
        input = patchTesBuiltInInputImport(inputTy, builtInId, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStageGeometry: {
        // Builtin Call has different number of operands
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          vertexIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        input = patchGsBuiltInInputImport(inputTy, builtInId, vertexIdx, builder);
        break;
      }
      case ShaderStageMesh: {
        assert(callInst.arg_size() == 2);
        assert(isDontCareValue(callInst.getOperand(1)));
        input = patchMeshBuiltInInputImport(inputTy, builtInId, builder);
        break;
      }
      case ShaderStageFragment: {
        Value *generalVal = nullptr;
        if (callInst.arg_size() >= 2)
          generalVal = callInst.getArgOperand(1);
        input = patchFsBuiltInInputImport(inputTy, builtInId, generalVal, builder);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else {
      assert(m_shaderStage != ShaderStageVertex && "vertex fetch is handled by LowerVertexFetch");

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
        assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval ||
               m_shaderStage == ShaderStageFragment);
      }

      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(origLoc);
      if (m_shaderStage == ShaderStageTessEval ||
          (m_shaderStage == ShaderStageFragment &&
           (m_pipelineState->getPrevShaderStage(m_shaderStage) == ShaderStageMesh || m_pipelineState->isUnlinked()))) {
        // NOTE: For generic inputs of tessellation evaluation shader or fragment shader whose previous shader stage
        // is mesh shader or is in unlinked pipeline, they could be per-patch ones or per-primitive ones.
        const bool isPerPrimitive = genericLocationOp.getPerPrimitive();
        if (isPerPrimitive) {
          auto &checkedMap = m_shaderStage == ShaderStageTessEval ? resUsage->inOutUsage.perPatchInputLocMap
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
        if (m_pipelineState->canPackInput(m_shaderStage)) {
          // The inputLocInfoMap of {TCS, GS, FS} maps original InOutLocationInfo to tightly compact InOutLocationInfo
          const bool isTcs = m_shaderStage == ShaderStageTessControl;
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

      switch (m_shaderStage) {
      case ShaderStageTessControl: {
        auto &inputOp = cast<InputImportGenericOp>(genericLocationOp);
        auto vertexIdx = inputOp.getArrayIndex();
        assert(isDontCareValue(vertexIdx) == false);

        input = patchTcsGenericInputImport(inputTy, loc, locOffset, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStageTessEval: {
        auto &inputOp = cast<InputImportGenericOp>(genericLocationOp);

        Value *vertexIdx = nullptr;
        if (!inputOp.getPerPrimitive())
          vertexIdx = inputOp.getArrayIndex();

        input = patchTesGenericInputImport(inputTy, loc, locOffset, elemIdx, vertexIdx, builder);
        break;
      }
      case ShaderStageGeometry: {
        const unsigned compIdx = cast<ConstantInt>(elemIdx)->getZExtValue();

        auto &inputOp = cast<InputImportGenericOp>(genericLocationOp);
        Value *vertexIdx = inputOp.getArrayIndex();
        assert(isDontCareValue(vertexIdx) == false);

        input = patchGsGenericInputImport(inputTy, loc, compIdx, vertexIdx, builder);
        break;
      }
      case ShaderStageFragment: {
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

        input = patchFsGenericInputImport(inputTy, loc, locOffset, elemIdx, isPerPrimitive, interpMode, interpValue,
                                          highHalf, builder);
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
    assert(m_shaderStage == ShaderStageTessControl);

    Value *output = nullptr;
    Type *outputTy = callInst.getType();

    m_importCalls.push_back(&callInst);

    if (isBuiltInOutputImport) {
      const unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

      LLVM_DEBUG(dbgs() << "Find output import call: builtin = " << builtInId << "\n");

      assert(callInst.arg_size() == 3);
      Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
      Value *vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

      output = patchTcsBuiltInOutputImport(outputTy, builtInId, elemIdx, vertexIdx, builder);
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
        locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
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

      output = patchTcsGenericOutputImport(outputTy, loc, locOffset, elemIdx, vertexIdx, builder);
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
      switch (m_shaderStage) {
      case ShaderStageVertex: {
        // No TS/GS pipeline, VS is the last stage
        if (!m_hasGs && !m_hasTs)
          patchXfbOutputExport(output, xfbBuffer, xfbOffset, streamId, builder);
        break;
      }
      case ShaderStageTessEval: {
        // TS-only pipeline, TES is the last stage
        if (!m_hasGs)
          patchXfbOutputExport(output, xfbBuffer, xfbOffset, streamId, builder);
        break;
      }
      case ShaderStageGeometry: {
        // Do nothing, transform feedback output is done in copy shader
        break;
      }
      case ShaderStageCopyShader: {
        // TS-GS or GS-only pipeline, copy shader is the last stage
        patchXfbOutputExport(output, xfbBuffer, xfbOffset, streamId, builder);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else if (isBuiltInOutputExport) {
      const unsigned builtInId = value;

      switch (m_shaderStage) {
      case ShaderStageVertex: {
        patchVsBuiltInOutputExport(output, builtInId, &callInst);
        break;
      }
      case ShaderStageTessControl: {
        assert(callInst.arg_size() == 4);
        Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
        Value *vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

        patchTcsBuiltInOutputExport(output, builtInId, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageTessEval: {
        patchTesBuiltInOutputExport(output, builtInId, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        patchGsBuiltInOutputExport(output, builtInId, m_pipelineState->getRasterizerState().rasterStream, builder);
        break;
      }
      case ShaderStageMesh: {
        assert(callInst.arg_size() == 5);
        Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
        Value *vertexOrPrimitiveIdx = callInst.getOperand(2);
        bool isPerPrimitive = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue() != 0;

        patchMeshBuiltInOutputExport(output, builtInId, elemIdx, vertexOrPrimitiveIdx, isPerPrimitive, &callInst);
        break;
      }
      case ShaderStageFragment: {
        patchFsBuiltInOutputExport(output, builtInId, &callInst);
        break;
      }
      case ShaderStageCopyShader: {
        patchCopyShaderBuiltInOutputExport(output, builtInId, &callInst);
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
      if (m_shaderStage == ShaderStageGeometry)
        origLocInfo.setStreamId(cast<ConstantInt>(callInst.getOperand(2))->getZExtValue());

      if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageMesh) {
        locOffset = callInst.getOperand(1);

        // NOTE: For generic outputs of tessellation control shader or mesh shader, they could be per-patch ones or
        // per-primitive ones.
        if (m_shaderStage == ShaderStageMesh && cast<ConstantInt>(callInst.getOperand(4))->getZExtValue() != 0) {
          auto locMapIt = resUsage->inOutUsage.perPrimitiveOutputLocMap.find(value);
          if (locMapIt != resUsage->inOutUsage.perPrimitiveOutputLocMap.end()) {
            loc = locMapIt->second;
            exist = true;
          }
        } else if (m_shaderStage == ShaderStageTessControl && isDontCareValue(callInst.getOperand(3))) {
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
      } else if (m_shaderStage == ShaderStageCopyShader) {
        exist = true;
        loc = value;
      } else {
        // Generic output exports of FS should have been handled by the LowerFragColorExport pass
        assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageGeometry ||
               m_shaderStage == ShaderStageTessEval);

        // Check component offset and search the location info map once again
        unsigned component = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        if (output->getType()->getScalarSizeInBits() == 64)
          component *= 2; // Component in location info is dword-based
        origLocInfo.setComponent(component);
        auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);

        if (m_pipelineState->canPackOutput(m_shaderStage)) {
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

        switch (m_shaderStage) {
        case ShaderStageVertex: {
          assert(callInst.arg_size() == 3);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          patchVsGenericOutputExport(output, loc, elemIdx, builder);
          break;
        }
        case ShaderStageTessControl: {
          assert(callInst.arg_size() == 5);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

          patchTcsGenericOutputExport(output, loc, locOffset, elemIdx, vertexIdx, builder);
          break;
        }
        case ShaderStageTessEval: {
          assert(callInst.arg_size() == 3);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          patchTesGenericOutputExport(output, loc, elemIdx, builder);
          break;
        }
        case ShaderStageGeometry: {
          assert(callInst.arg_size() == 4);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          const unsigned streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
          patchGsGenericOutputExport(output, loc, elemIdx, streamId, builder);
          break;
        }
        case ShaderStageMesh: {
          assert(callInst.arg_size() == 6);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexOrPrimitiveIdx = callInst.getOperand(3);
          bool isPerPrimitive = cast<ConstantInt>(callInst.getOperand(4))->getZExtValue() != 0;
          patchMeshGenericOutputExport(output, loc, locOffset, elemIdx, vertexOrPrimitiveIdx, isPerPrimitive, builder);
          break;
        }
        case ShaderStageCopyShader: {
          patchCopyShaderGenericOutputExport(output, loc, &callInst);
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
    if (callee->isIntrinsic() && callee->getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg) {
      unsigned emitStream = InvalidValue;
      uint64_t message = cast<ConstantInt>(callInst.getArgOperand(0))->getZExtValue();
      if (message == GsEmitStreaM0 || message == GsEmitStreaM1 || message == GsEmitStreaM2 ||
          message == GsEmitStreaM3) {
        // NOTE: MSG[9:8] = STREAM_ID
        emitStream = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
      }

      if (emitStream != InvalidValue) {
        assert(m_shaderStage == ShaderStageGeometry); // Must be geometry shader

        // NOTE: Implicitly store the value of view index to GS-VS ring buffer for raster stream if multi-view is
        // enabled. Copy shader will read the value from GS-VS ring and export it to vertex position data.
        if (m_pipelineState->getInputAssemblyState().enableMultiView) {
          auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
          auto rasterStream = m_pipelineState->getRasterizerState().rasterStream;

          if (emitStream == rasterStream) {
            auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
            auto viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);

            const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
            assert(builtInOutLocMap.find(BuiltInViewIndex) != builtInOutLocMap.end());
            unsigned loc = builtInOutLocMap.find(BuiltInViewIndex)->second;

            storeValueToGsVsRing(viewIndex, loc, 0, rasterStream, builder);
          }
        }

        // Increment emit vertex counter
        auto emitCounterPair = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr();
        auto emitCounterTy = emitCounterPair.first;
        auto emitCounterPtr = emitCounterPair.second[emitStream];
        Value *emitCounter = builder.CreateLoad(emitCounterTy, emitCounterPtr);
        emitCounter = builder.CreateAdd(emitCounter, builder.getInt32(1));
        builder.CreateStore(emitCounter, emitCounterPtr);
      }
    }
  }
}

// =====================================================================================================================
// Visits "ret" instruction.
//
// @param retInst : "Ret" instruction
void PatchInOutImportExport::visitReturnInst(ReturnInst &retInst) {
  // We only handle the "ret" of shader entry point
  if (m_shaderStage == ShaderStageInvalid)
    return;

  const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);

  // Whether this shader stage has to use "exp" instructions to export outputs
  const bool useExpInst = ((m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
                            m_shaderStage == ShaderStageCopyShader) &&
                           (nextStage == ShaderStageInvalid || nextStage == ShaderStageFragment));

  auto zero = ConstantFP::get(Type::getFloatTy(*m_context), 0.0);
  auto one = ConstantFP::get(Type::getFloatTy(*m_context), 1.0);
  auto poison = PoisonValue::get(Type::getFloatTy(*m_context));

  Instruction *insertPos = &retInst;

  const bool enableXfb = m_pipelineState->enableXfb();
  if (m_shaderStage == ShaderStageCopyShader && enableXfb) {
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
              insertPos = caseBranch.getCaseSuccessor()->getTerminator();
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

    auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;

    if (m_shaderStage == ShaderStageVertex) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
      useEdgeFlag = builtInUsage.edgeFlag;
    } else if (m_shaderStage == ShaderStageTessEval) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    } else {
      assert(m_shaderStage == ShaderStageCopyShader);
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader)->builtInUsage.gs;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    }

    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
    if (enableMultiView) {
      if (m_shaderStage == ShaderStageVertex) {
        auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
        m_viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
      } else if (m_shaderStage == ShaderStageTessEval) {
        auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;
        m_viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
      } else {
        assert(m_shaderStage == ShaderStageCopyShader);
        assert(m_viewIndex); // Must have been explicitly loaded in copy shader
      }
    }

    const auto &builtInOutLocs =
        m_shaderStage == ShaderStageCopyShader ? inOutUsage.gs.builtInOutLocs : inOutUsage.builtInOutputLocMap;
    const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

    // NOTE: If gl_Position is not present in this shader stage, we have to export a dummy one.
    if (!usePosition) {
      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_0), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),              // en
          zero,                                                             // src0
          zero,                                                             // src1
          zero,                                                             // src2
          one,                                                              // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
          ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
      };
      emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    }

    // Export gl_ClipDistance[] and gl_CullDistance[] before entry-point returns
    if (clipDistanceCount > 0 || cullDistanceCount > 0) {
      assert(clipDistanceCount + cullDistanceCount <= MaxClipCullDistanceCount);

      assert(clipDistanceCount == 0 || (clipDistanceCount > 0 && m_clipDistance));
      assert(cullDistanceCount == 0 || (cullDistanceCount > 0 && m_cullDistance));

      // Extract elements of gl_ClipDistance[] and gl_CullDistance[]
      std::vector<Value *> clipDistance;
      for (unsigned i = 0; i < clipDistanceCount; ++i)
        clipDistance.push_back(ExtractValueInst::Create(m_clipDistance, {i}, "", insertPos));

      std::vector<Value *> cullDistance;
      for (unsigned i = 0; i < cullDistanceCount; ++i)
        cullDistance.push_back(ExtractValueInst::Create(m_cullDistance, {i}, "", insertPos));

      // Merge gl_ClipDistance[] and gl_CullDistance[]
      std::vector<Value *> clipCullDistance;
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
      unsigned pos = miscExport ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), pos),  // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),  // en
          clipCullDistance[0],                                  // src0
          clipCullDistance[1],                                  // src1
          clipCullDistance[2],                                  // src2
          clipCullDistance[3],                                  // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false), // done
          ConstantInt::get(Type::getInt1Ty(*m_context), false)  // vm
      };

      emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

      if (clipCullDistance.size() > 4) {
        // Do the second exporting
        Value *args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_context), pos + 1), // tgt
            ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),     // en
            clipCullDistance[4],                                     // src0
            clipCullDistance[5],                                     // src1
            clipCullDistance[6],                                     // src2
            clipCullDistance[7],                                     // src3
            ConstantInt::get(Type::getInt1Ty(*m_context), false),    // done
            ConstantInt::get(Type::getInt1Ty(*m_context), false)     // vm
        };
        emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
      }

      // NOTE: We have to export gl_ClipDistance[] or gl_CullDistancep[] via generic outputs as well.
      assert(nextStage == ShaderStageInvalid || nextStage == ShaderStageFragment);

      bool hasClipCullExport = true;
      if (nextStage == ShaderStageFragment) {
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

        recordVertexAttribExport(loc,
                                 {clipCullDistance[0], clipCullDistance[1], clipCullDistance[2], clipCullDistance[3]});

        if (clipCullDistance.size() > 4) {
          // Do the second exporting
          recordVertexAttribExport(
              loc + 1, {clipCullDistance[4], clipCullDistance[5], clipCullDistance[6], clipCullDistance[7]});
        }
      }
    }

    // Export gl_PrimitiveID before entry-point returns
    if (usePrimitiveId) {
      bool hasPrimitiveIdExport = false;
      if (nextStage == ShaderStageFragment) {
        hasPrimitiveIdExport = nextBuiltInUsage.primitiveId;
      } else if (nextStage == ShaderStageInvalid) {
        if (m_shaderStage == ShaderStageCopyShader) {
          hasPrimitiveIdExport =
              m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs.primitiveId;
        }
      }

      if (hasPrimitiveIdExport) {
        assert(builtInOutLocs.find(BuiltInPrimitiveId) != builtInOutLocs.end());
        const unsigned loc = builtInOutLocs.find(BuiltInPrimitiveId)->second;

        assert(m_primitiveId);
        Value *primitiveId = new BitCastInst(m_primitiveId, Type::getFloatTy(*m_context), "", insertPos);

        recordVertexAttribExport(loc, {primitiveId, poison, poison, poison});
      }
    }

    // Export EdgeFlag
    if (useEdgeFlag) {
      addExportInstForBuiltInOutput(m_edgeFlag, BuiltInEdgeFlag, insertPos);
    }

    if (m_gfxIp.major <= 8 && (useLayer || enableMultiView)) {
      if (enableMultiView) {
        assert(m_viewIndex);
        addExportInstForBuiltInOutput(m_viewIndex, BuiltInViewIndex, insertPos);
      }

      if (useLayer) {
        assert(m_layer);
        addExportInstForBuiltInOutput(m_layer, BuiltInLayer, insertPos);
      }
    }

    // Export gl_Layer and gl_ViewportIndex before entry-point returns
    if (m_gfxIp.major >= 9 && (useLayer || useViewportIndex || enableMultiView)) {
      Value *viewportIndexAndLayer = ConstantInt::get(Type::getInt32Ty(*m_context), 0);

      if (useViewportIndex) {
        assert(m_viewportIndex);
        viewportIndexAndLayer = BinaryOperator::CreateShl(
            m_viewportIndex, ConstantInt::get(Type::getInt32Ty(*m_context), 16), "", insertPos);
      }

      if (enableMultiView) {
        assert(m_viewIndex);
        viewportIndexAndLayer = BinaryOperator::CreateOr(viewportIndexAndLayer, m_viewIndex, "", insertPos);
      } else if (useLayer) {
        assert(m_layer);
        viewportIndexAndLayer = BinaryOperator::CreateOr(viewportIndexAndLayer, m_layer, "", insertPos);
      }

      viewportIndexAndLayer = new BitCastInst(viewportIndexAndLayer, Type::getFloatTy(*m_context), "", insertPos);

      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0x4),              // en
          poison,                                                           // src0
          poison,                                                           // src1
          viewportIndexAndLayer,                                            // src2
          poison,                                                           // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
          ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
      };

      emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

      // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
      if (useViewportIndex) {
        bool hasViewportIndexExport = true;
        if (nextStage == ShaderStageFragment) {
          hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
        } else if (nextStage == ShaderStageInvalid) {
          hasViewportIndexExport = false;
        }

        if (hasViewportIndexExport) {
          assert(builtInOutLocs.find(BuiltInViewportIndex) != builtInOutLocs.end());
          const unsigned loc = builtInOutLocs.find(BuiltInViewportIndex)->second;

          Value *viewportIndex = new BitCastInst(m_viewportIndex, Type::getFloatTy(*m_context), "", insertPos);

          recordVertexAttribExport(loc, {viewportIndex, poison, poison, poison});
        }
      }

      // NOTE: We have to export gl_Layer via generic outputs as well.
      if (useLayer) {
        bool hasLayerExport = true;
        if (nextStage == ShaderStageFragment) {
          hasLayerExport = nextBuiltInUsage.layer;
        } else if (nextStage == ShaderStageInvalid) {
          hasLayerExport = false;
        }

        if (hasLayerExport) {
          assert(builtInOutLocs.find(BuiltInLayer) != builtInOutLocs.end());
          const unsigned loc = builtInOutLocs.find(BuiltInLayer)->second;

          Value *layer = new BitCastInst(m_layer, Type::getFloatTy(*m_context), "", insertPos);

          recordVertexAttribExport(loc, {layer, poison, poison, poison});
        }
      }
    }

    // NOTE: For GFX10+, dummy generic output is no longer needed. Field NO_PC_EXPORT of SPI_VS_OUT_CONFIG
    // will control the behavior.
    if (m_gfxIp.major <= 9) {
      // NOTE: If no generic outputs is present in this shader, we have to export a dummy one
      if (inOutUsage.expCount == 0)
        recordVertexAttribExport(0, {poison, poison, poison, poison});
    }

    // Export vertex attributes that were recorded previously
    exportVertexAttribs(insertPos);

    if (m_pipelineState->isUnlinked()) {
      // If we are building unlinked relocatable shaders, it is possible there are
      // generic outputs that are not written to.  We need to count them in
      // the export count.
      auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);
      for (const auto &locInfoPair : resUsage->inOutUsage.outputLocInfoMap) {
        const unsigned newLoc = locInfoPair.second.getLocation();
        if (m_expLocs.count(newLoc) != 0)
          continue;
        inOutUsage.expCount = std::max(inOutUsage.expCount, newLoc + 1); // Update export count
      }
    }
  } else if (m_shaderStage == ShaderStageTessControl) {
    // NOTE: We will read back tessellation factors from on-chip LDS in later phases and write them to TF buffer.
    // Add fence and barrier before the return instruction to make sure they have been stored already.
    SyncScope::ID syncScope = m_context->getOrInsertSyncScopeID("workgroup");
    new FenceInst(*m_context, AtomicOrdering::Release, syncScope, insertPos);
    emitCall("llvm.amdgcn.s.barrier", Type::getVoidTy(*m_context), {}, {}, insertPos);
    new FenceInst(*m_context, AtomicOrdering::Acquire, syncScope, insertPos);
  } else if (m_shaderStage == ShaderStageGeometry) {
    if (m_gfxIp.major >= 10) {
      // NOTE: Per programming guide, we should do a "s_waitcnt 0,0,0 + s_waitcnt_vscnt 0" before issuing a "done", so
      // we use fence release to generate s_waitcnt vmcnt lgkmcnt/s_waitcnt_vscnt before s_sendmsg(MSG_GS_DONE)
      SyncScope::ID scope =
          m_pipelineState->isGsOnChip() ? m_context->getOrInsertSyncScopeID("workgroup") : SyncScope::System;
      new FenceInst(*m_context, AtomicOrdering::Release, scope, insertPos);
    }

    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
    auto gsWaveId = getFunctionArgument(m_entryPoint, entryArgIdxs.gsWaveId);
    Value *args[] = {ConstantInt::get(Type::getInt32Ty(*m_context), GsDone), gsWaveId};

    emitCall("llvm.amdgcn.s.sendmsg", Type::getVoidTy(*m_context), args, {}, insertPos);
  } else if (m_shaderStage == ShaderStageFragment) {
    // Fragment shader export are handled in LowerFragColorExport.
    return;
  }
}

// =====================================================================================================================
// Patches import calls for generic inputs of tessellation control shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchTcsGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                          Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx && vertexIdx);

  auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, location, locOffset, compIdx, vertexIdx, builder);
  return readValueFromLds(false, inputTy, ldsOffset, builder);
}

// =====================================================================================================================
// Patches import calls for generic inputs of tessellation evaluation shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchTesGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                          Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx);

  auto ldsOffset = calcLdsOffsetForTesInput(inputTy, location, locOffset, compIdx, vertexIdx, builder);
  return readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, builder);
}

// =====================================================================================================================
// Patches import calls for generic inputs of geometry shader.
//
// @param inputTy : Type of input value
// @param location : Location of the input
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchGsGenericInputImport(Type *inputTy, unsigned location, unsigned compIdx,
                                                         Value *vertexIdx, BuilderBase &builder) {
  assert(vertexIdx);

  const unsigned compCount = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  const unsigned bitWidth = inputTy->getScalarSizeInBits();

  Type *origInputTy = inputTy;

  if (bitWidth == 64) {
    compIdx *= 2; // For 64-bit data type, the component indexing must multiply by 2

    // Cast 64-bit data type to float vector
    inputTy = FixedVectorType::get(Type::getFloatTy(*m_context), compCount * 2);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  Value *input = loadValueFromEsGsRing(inputTy, location, compIdx, vertexIdx, &*builder.GetInsertPoint());

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
Value *PatchInOutImportExport::performFsFloatInterpolation(BuilderBase &builder, Value *attr, Value *channel,
                                                           Value *coordI, Value *coordJ, Value *primMask) {
  Value *result = nullptr;
  Attribute::AttrKind attribs[] = {Attribute::ReadNone};
  if (m_gfxIp.major >= 11) {
    // llvm.amdgcn.lds.param.load(attr_channel, attr, m0)
    Value *param =
        builder.CreateNamedCall("llvm.amdgcn.lds.param.load", builder.getFloatTy(), {channel, attr, primMask}, attribs);

    // tmp = llvm.amdgcn.interp.inreg.p10(p10, coordI, p0)
    result =
        builder.CreateNamedCall("llvm.amdgcn.interp.inreg.p10", builder.getFloatTy(), {param, coordI, param}, attribs);

    // llvm.amdgcn.interp.inreg.p2(p20, coordJ, tmp)
    result =
        builder.CreateNamedCall("llvm.amdgcn.interp.inreg.p2", builder.getFloatTy(), {param, coordJ, result}, attribs);
  } else {
    // llvm.amdgcn.interp.p1(coordI, attr_channel, attr, m0)
    result = builder.CreateNamedCall("llvm.amdgcn.interp.p1", builder.getFloatTy(), {coordI, channel, attr, primMask},
                                     attribs);

    // llvm.amdgcn.interp.p2(p1, coordJ, attr_channel, attr, m0)
    result = builder.CreateNamedCall("llvm.amdgcn.interp.p2", builder.getFloatTy(),
                                     {result, coordJ, channel, attr, primMask}, attribs);
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
Value *PatchInOutImportExport::performFsHalfInterpolation(BuilderBase &builder, Value *attr, Value *channel,
                                                          Value *coordI, Value *coordJ, Value *primMask,
                                                          Value *highHalf) {
  Value *result = nullptr;
  Attribute::AttrKind attribs[] = {Attribute::ReadNone};
  if (m_gfxIp.major >= 11) {
    // llvm.amdgcn.lds.param.load(attr_channel, attr, m0)
    Value *param =
        builder.CreateNamedCall("llvm.amdgcn.lds.param.load", builder.getFloatTy(), {channel, attr, primMask}, attribs);

    // tmp = llvm.amdgcn.interp.inreg.p10.f16(p10, coordI, p0, highHalf)
    result = builder.CreateNamedCall("llvm.amdgcn.interp.inreg.p10.f16", builder.getFloatTy(),
                                     {param, coordI, param, highHalf}, attribs);

    // llvm.amdgcn.interp.inreg.p2.f16(p20, coordJ, tmp, highHalf)
    result = builder.CreateNamedCall("llvm.amdgcn.interp.inreg.p2.f16", builder.getHalfTy(),
                                     {param, coordJ, result, highHalf}, attribs);
  } else {
    // llvm.amdgcn.interp.p1.f16(coordI, attr_channel, attr, highhalf, m0)
    result = builder.CreateNamedCall("llvm.amdgcn.interp.p1.f16", builder.getFloatTy(),
                                     {coordI, channel, attr, highHalf, primMask}, attribs);

    // llvm.amdgcn.interp.p2.f16(p1, coordJ, attr_channel, attr, highhalf, m0)
    result = builder.CreateNamedCall("llvm.amdgcn.interp.p2.f16", builder.getHalfTy(),
                                     {result, coordJ, channel, attr, highHalf, primMask}, attribs);
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
Value *PatchInOutImportExport::performFsParameterLoad(BuilderBase &builder, Value *attr, Value *channel,
                                                      InterpParam interpParam, Value *primMask, unsigned bitWidth,
                                                      bool highHalf) {
  Value *compValue = nullptr;

  if (m_gfxIp.major >= 11) {
    // llvm.amdgcn.lds.param.load(attr_channel, attr, m0)
    compValue = builder.CreateNamedCall("llvm.amdgcn.lds.param.load", builder.getFloatTy(), {channel, attr, primMask},
                                        {Attribute::ReadNone});
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
    // NOTE: Make mov_dpp and its source instructions run in WQM to make sure the mov_dpp could fetch
    // correct data from possible inactive lanes.
    compValue = builder.CreateIntrinsic(Intrinsic::amdgcn_wqm, builder.getInt32Ty(), compValue);
    compValue = builder.CreateBitCast(compValue, builder.getFloatTy());
  } else {
    Value *args[] = {
        builder.getInt32(interpParam), // param
        channel,                       // attr_chan
        attr,                          // attr
        primMask                       // m0
    };
    compValue = builder.CreateNamedCall("llvm.amdgcn.interp.mov", builder.getFloatTy(), args, {Attribute::ReadNone});
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
// Patches import calls for generic inputs of fragment shader.
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
Value *PatchInOutImportExport::patchFsGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                         Value *compIdx, bool isPerPrimitive, unsigned interpMode,
                                                         Value *interpValue, bool highHalf, BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);
  auto &interpInfo = resUsage->inOutUsage.fs.interpInfo;

  // NOTE: For per-primitive input, the specified location is still per-primitive based. To import the input value, we
  // have to adjust it by adding the total number of per-vertex inputs since per-vertex exports/imports are prior to
  // per-primitive ones.
  if (isPerPrimitive) {
    auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->inOutUsage;
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

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
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
    interpTy = Type::getInt8Ty(*m_context);
  } else if (bitWidth == 16)
    interpTy = Type::getHalfTy(*m_context);
  else
    interpTy = Type::getFloatTy(*m_context);
  if (numChannels > 1)
    interpTy = FixedVectorType::get(interpTy, numChannels);
  Value *interp = PoisonValue::get(interpTy);

  unsigned startChannel = 0;
  if (compIdx) {
    startChannel = cast<ConstantInt>(compIdx)->getZExtValue();
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
// Patches import calls for generic outputs of tessellation control shader.
//
// @param outputTy : Type of output value
// @param location : Base location of the output
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchTcsGenericOutputImport(Type *outputTy, unsigned location, Value *locOffset,
                                                           Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx);
  auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, location, locOffset, compIdx, vertexIdx, builder);
  return readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, builder);
}

// =====================================================================================================================
// Patches export calls for generic outputs of vertex shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param builder : The IR builder to create and insert IR instruction
void PatchInOutImportExport::patchVsGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                        BuilderBase &builder) {
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

        outputTy = FixedVectorType::get(Type::getFloatTy(*m_context), compCount);
        output = builder.CreateBitCast(output, outputTy);
      } else
        assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

      storeValueToEsGsRing(output, location, compIdx, &*builder.GetInsertPoint());
    } else
      addExportInstForGenericOutput(output, location, compIdx, &*builder.GetInsertPoint());
  }
}

// =====================================================================================================================
// Patches export calls for generic outputs of tessellation control shader.
//
// @param output : Output value
// @param location : Base location of the output
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
void PatchInOutImportExport::patchTcsGenericOutputExport(Value *output, unsigned location, Value *locOffset,
                                                         Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(compIdx);
  Type *outputTy = output->getType();
  auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, location, locOffset, compIdx, vertexIdx, builder);
  writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, builder);
}

// =====================================================================================================================
// Patches export calls for generic outputs of tessellation evaluation shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param builder : The IR builder to create and insert IR instruction
void PatchInOutImportExport::patchTesGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                         BuilderBase &builder) {
  if (m_hasGs) {
    auto outputTy = output->getType();
    assert(outputTy->isIntOrIntVectorTy() || outputTy->isFPOrFPVectorTy());

    const unsigned bitWidth = outputTy->getScalarSizeInBits();
    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx *= 2;

      unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() * 2 : 2;
      outputTy = FixedVectorType::get(Type::getFloatTy(*m_context), compCount);

      output = builder.CreateBitCast(output, outputTy);
    } else
      assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

    storeValueToEsGsRing(output, location, compIdx, &*builder.GetInsertPoint());
  } else
    addExportInstForGenericOutput(output, location, compIdx, &*builder.GetInsertPoint());
}

// =====================================================================================================================
// Patches export calls for generic outputs of geometry shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param streamId : ID of output vertex stream
// @param builder : The IR builder to create and insert IR instruction
void PatchInOutImportExport::patchGsGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                        unsigned streamId, BuilderBase &builder) {
  auto outputTy = output->getType();

  // Cast double or double vector to float vector.
  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  if (bitWidth == 64) {
    // For 64-bit data type, the component indexing must multiply by 2
    compIdx *= 2;

    if (outputTy->isVectorTy())
      outputTy =
          FixedVectorType::get(Type::getFloatTy(*m_context), cast<FixedVectorType>(outputTy)->getNumElements() * 2);
    else
      outputTy = FixedVectorType::get(Type::getFloatTy(*m_context), 2);

    output = builder.CreateBitCast(output, outputTy);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word to dword and
  // store dword to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size is based on number of dwords.

  assert(compIdx <= 4);

  storeValueToGsVsRing(output, location, compIdx, streamId, builder);
}

// =====================================================================================================================
// Patches export calls for generic outputs of mesh shader.
//
// @param output : Output value
// @param location : Base location of the output
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexOrPrimitiveIdx : Input array outermost index used for vertex or primitive indexing
// @param isPerPrimitive : Whether the output is per-primitive
// @param builder : The IR builder to create and insert IR instruction
void PatchInOutImportExport::patchMeshGenericOutputExport(Value *output, unsigned location, Value *locOffset,
                                                          Value *compIdx, Value *vertexOrPrimitiveIdx,
                                                          bool isPerPrimitive, BuilderBase &builder) {
  // outputOffset = (location + locOffset) * 4 + compIdx * (bitWidth == 64 ? 2 : 1)
  Value *outputOffset = builder.CreateAdd(builder.getInt32(location), locOffset);
  outputOffset = builder.CreateShl(outputOffset, 2);

  auto outputTy = output->getType();
  if (outputTy->getScalarSizeInBits() == 64) {
    compIdx = builder.CreateShl(compIdx, 1);
  }

  outputOffset = builder.CreateAdd(outputOffset, compIdx);

  if (isPerPrimitive)
    builder.create<WriteMeshPrimitiveOutputOp>(outputOffset, vertexOrPrimitiveIdx, output);
  else
    builder.create<WriteMeshVertexOutputOp>(outputOffset, vertexOrPrimitiveIdx, output);
}

// =====================================================================================================================
// Patches import calls for built-in inputs of tessellation control shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchTcsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *elemIdx,
                                                          Value *vertexIdx, BuilderBase &builder) {
  Value *input = PoisonValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl)->entryArgIdxs.tcs;
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
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
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTcsInput(elemTy, loc, nullptr, elemIdx, vertexIdx, builder);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, builder);
        builder.CreateInsertValue(input, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      input = readValueFromLds(false, inputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInPatchVertices: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getNumPatchControlPoints());
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
    if (m_pipelineState->getInputAssemblyState().enableMultiView)
      input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
    else
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
// Patches import calls for built-in inputs of tessellation evaluation shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchTesBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *elemIdx,
                                                          Value *vertexIdx, BuilderBase &builder) {
  Value *input = PoisonValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  const auto &inOutUsage = resUsage->inOutUsage;
  const auto &builtInInLocMap = inOutUsage.builtInInputLocMap;
  const auto &perPatchBuiltInInLocMap = inOutUsage.perPatchBuiltInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
    input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, builder);

    break;
  }
  case BuiltInPointSize:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, nullptr, vertexIdx, builder);
    input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, builder);

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
        auto elem = readValueFromLds(m_pipelineState->isTessOffChip(), elemTy, ldsOffset, builder);
        input = builder.CreateInsertValue(input, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInPatchVertices: {
    unsigned patchVertices = MaxTessPatchVertices;
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
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
        auto elem = readValueFromLds(m_pipelineState->isTessOffChip(), elemTy, ldsOffset, builder);
        input = builder.CreateInsertValue(input, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().enableMultiView)
      input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
    else
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
// Patches import calls for built-in inputs of geometry shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchGsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *vertexIdx,
                                                         BuilderBase &builder) {
  Value *input = nullptr;

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

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
    input = loadValueFromEsGsRing(inputTy, loc, 0, vertexIdx, &*builder.GetInsertPoint());
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
    if (m_pipelineState->getInputAssemblyState().enableMultiView)
      input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
    else
      input = builder.getInt32(0);
    break;
  }
  // Handle internal-use built-ins
  case BuiltInGsWaveId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.gsWaveId);
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
// Patches import calls for built-in inputs of mesh shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchMeshBuiltInInputImport(Type *inputTy, unsigned builtInId, BuilderBase &builder) {
  // Handle work group size built-in
  if (builtInId == BuiltInWorkgroupSize) {
    // WorkgroupSize is a constant vector supplied by mesh shader mode.
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    return ConstantVector::get({builder.getInt32(meshMode.workgroupSizeX), builder.getInt32(meshMode.workgroupSizeY),
                                builder.getInt32(meshMode.workgroupSizeZ)});
  }

  // Handle other built-ins
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
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
// Patches import calls for built-in inputs of fragment shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param generalVal : Sample ID, only needed for BuiltInSamplePosOffset; InterpLoc, only needed for BuiltInBaryCoord
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchFsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *generalVal,
                                                         BuilderBase &builder) {
  Value *input = PoisonValue::get(inputTy);

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->inOutUsage;

  switch (builtInId) {
  case BuiltInSampleMask: {
    assert(inputTy->isArrayTy());

    auto sampleCoverage = getFunctionArgument(m_entryPoint, entryArgIdxs.sampleCoverage);
    auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

    // gl_SampleID = Ancillary[11:8]
    auto sampleId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                            {ancillary, builder.getInt32(8), builder.getInt32(4)});

    Value *sampleMaskIn = sampleCoverage;
    if (m_pipelineState->getRasterizerState().perSampleShading || builtInUsage.runAtSampleRate) {
      unsigned baseMask = 1;
      if (!builtInUsage.sampleId) {
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
        m_pipelineState->getShaderOptions(ShaderStageFragment).adjustDepthImportVrs) {
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
    Value *interpValue =
        patchFsBuiltInInputImport(FixedVectorType::get(builder.getFloatTy(), 2), builtInId, nullptr, builder);
    input = patchFsGenericInputImport(inputTy, loc, nullptr, nullptr, false, InOutInfo::InterpModeSmooth, interpValue,
                                      false, builder);
    break;
  }
  case BuiltInHelperInvocation: {
    input = builder.CreateIntrinsic(Intrinsic::amdgcn_ps_live, {}, {});
    input = builder.CreateNot(input);
    break;
  }
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().enableMultiView)
      input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
    else
      input = builder.getInt32(0);
    break;
  }
  case BuiltInPrimitiveId:
  case BuiltInLayer:
  case BuiltInViewportIndex: {
    unsigned loc = InvalidValue;
    const auto prevStage = m_pipelineState->getPrevShaderStage(ShaderStageFragment);

    bool isPerPrimitive = false;
    if (prevStage == ShaderStageMesh) {
      assert(inOutUsage.perPrimitiveBuiltInInputLocMap.count(builtInId) > 0);
      loc = inOutUsage.perPrimitiveBuiltInInputLocMap[builtInId];
      // NOTE: If the previous shader stage is mesh shader, those built-ins are exported via primitive attributes.
      isPerPrimitive = true;
    } else {
      assert(inOutUsage.builtInInputLocMap.count(builtInId) > 0);
      loc = inOutUsage.builtInInputLocMap[builtInId];
    }

    // Emulation for "in int gl_PrimitiveID" or "in int gl_Layer" or "in int gl_ViewportIndex".
    input = patchFsGenericInputImport(inputTy, loc, nullptr, nullptr, isPerPrimitive, InOutInfo::InterpModeFlat,
                                      nullptr, false, builder);
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

    input = getShadingRate(&*builder.GetInsertPoint());
    break;
  }
  // Handle internal-use built-ins for sample position emulation
  case BuiltInNumSamples: {
    if (m_pipelineState->isUnlinked() || m_pipelineState->getRasterizerState().dynamicSampleInfo) {
      assert(entryArgIdxs.sampleInfo != 0);
      auto sampleInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.sampleInfo);
      input = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                      {sampleInfo, builder.getInt32(0), builder.getInt32(16)});
    } else {
      input = builder.getInt32(m_pipelineState->getRasterizerState().numSamples);
    }
    break;
  }
  case BuiltInSamplePatternIdx: {
    if (m_pipelineState->isUnlinked() || m_pipelineState->getRasterizerState().dynamicSampleInfo) {
      assert(entryArgIdxs.sampleInfo != 0);
      auto sampleInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.sampleInfo);
      input = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                      {sampleInfo, builder.getInt32(16), builder.getInt32(16)});
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
Value *PatchInOutImportExport::getSamplePosOffset(Type *inputTy, Value *sampleId, BuilderBase &builder) {
  // Gets the offset of sample position relative to the pixel center for the specified sample ID
  Value *numSamples = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInNumSamples, nullptr, builder);
  Value *patternIdx = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSamplePatternIdx, nullptr, builder);
  Value *validOffset = builder.CreateAdd(patternIdx, sampleId);
  // offset = (sampleCount > sampleId) ? (samplePatternOffset + sampleId) : 0
  Value *sampleValid = builder.CreateICmpUGT(numSamples, sampleId);
  Value *offset = builder.CreateSelect(sampleValid, validOffset, builder.getInt32(0));
  // Load sample position descriptor.
  Value *desc = m_pipelineSysValues.get(m_entryPoint)->loadDescFromDriverTable(SiDrvTableSamplepos, builder);
  // Load the value using the descriptor.
  offset = builder.CreateShl(offset, builder.getInt32(4));
  return builder.CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load, inputTy,
                                 {desc, offset, builder.getInt32(0), builder.getInt32(0)});
}

// =====================================================================================================================
// Generate code to read BuiltInSamplePosition
//
// @param inputTy : Type of BuiltInSamplePosition
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::getSamplePosition(Type *inputTy, BuilderBase &builder) {
  Value *sampleId = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSampleId, nullptr, builder);
  Value *input = patchFsBuiltInInputImport(inputTy, BuiltInSamplePosOffset, sampleId, builder);
  return builder.CreateFAdd(input, ConstantFP::get(inputTy, 0.5));
}

// =====================================================================================================================
// Patches import calls for built-in outputs of tessellation control shader.
//
// @param outputTy : Type of output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Output array outermost index used for vertex indexing (could be null)
// @param builder : The IR builder to create and insert IR instruction
Value *PatchInOutImportExport::patchTcsBuiltInOutputImport(Type *outputTy, unsigned builtInId, Value *elemIdx,
                                                           Value *vertexIdx, BuilderBase &builder) {
  Value *output = PoisonValue::get(outputTy);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto &builtInUsage = resUsage->builtInUsage.tcs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize: {
    assert(builtInId != BuiltInPosition || builtInUsage.position);
    assert(builtInId != BuiltInPointSize || (builtInUsage.pointSize && !elemIdx));
    (void(builtInUsage)); // unused

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
    output = readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, builder);

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
        auto elem = readValueFromLds(m_pipelineState->isTessOffChip(), elemTy, ldsOffset, builder);
        output = builder.CreateInsertValue(output, elem, {i});
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      output = readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, builder);
    }

    break;
  }
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner: {
    assert(builtInId != BuiltInTessLevelOuter || builtInUsage.tessLevelOuter);
    assert(builtInId != BuiltInTessLevelInner || builtInUsage.tessLevelInner);
    (void(builtInUsage)); // Unused

    const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;

    // tessLevelOuter (float[4]) + tessLevelInner (float[2])
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + elemIdx
    uint32_t tessFactorStart = calcFactor.onChip.tessFactorStart;
    if (builtInId == BuiltInTessLevelInner)
      tessFactorStart += 4;

    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
    Value *baseOffset = builder.CreateMul(relativeId, builder.getInt32(MaxTessFactorsPerPatch));

    if (outputTy->isArrayTy()) {
      // Import the whole tessLevel array
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        Value *ldsOffset = builder.CreateAdd(baseOffset, builder.getInt32(tessFactorStart + i));
        auto elem = readValueFromLds(false, Type::getFloatTy(*m_context), ldsOffset, builder);
        output = builder.CreateInsertValue(output, elem, {i});
      }
    } else {
      // Import a single element of tessLevel array
      Value *ldsOffset = builder.CreateAdd(baseOffset, builder.getInt32(tessFactorStart));
      ldsOffset = builder.CreateAdd(ldsOffset, elemIdx);
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
// Patches export calls for built-in outputs of vertex shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchVsBuiltInOutputExport(Value *output, unsigned builtInId, Instruction *insertPos) {
  BuilderBase builder(insertPos);

  auto outputTy = output->getType();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  auto &builtInUsage = resUsage->builtInUsage.vs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize: {
    if ((builtInId == BuiltInPosition && !builtInUsage.position) ||
        (builtInId == BuiltInPointSize && !builtInUsage.pointSize))
      return;

    if (builtInId == BuiltInPointSize && (isa<UndefValue>(output) || isa<PoisonValue>(output))) {
      // NOTE: gl_PointSize is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
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

        storeValueToEsGsRing(output, loc, 0, insertPos);
      } else
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
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
      if (builtInId == BuiltInClipDistance)
        builtInUsage.clipDistance = 0;
      else
        builtInUsage.cullDistance = 0;
      return;
    }

    if (m_hasTs) {
      assert(outputTy->isArrayTy());

      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy->getArrayElementType(), loc, 0, builder);

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(false, elem, ldsOffset, builder);

        ldsOffset =
            BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
      }
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap.find(builtInId)->second;

        storeValueToEsGsRing(output, loc, 0, insertPos);
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

      storeValueToEsGsRing(output, loc, 0, insertPos);
    }

    break;
  }
  case BuiltInViewportIndex: {
    if (!static_cast<bool>(builtInUsage.viewportIndex))
      return;

    // NOTE: Only last vertex processing shader stage has to export the value of gl_ViewportIndex.
    if (!m_hasTs && !m_hasGs) {
      if (m_gfxIp.major <= 8)
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
      else {
        // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
        m_viewportIndex = output;
      }
    } else if (m_hasTs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, builder);
      writeValueToLds(false, output, ldsOffset, builder);
    } else if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
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
      addExportInstForBuiltInOutput(output, builtInId, insertPos);
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
// Patches export calls for built-in outputs of tessellation control shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Output array outermost index used for vertex indexing (could be null)
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchTcsBuiltInOutputExport(Value *output, unsigned builtInId, Value *elemIdx,
                                                         Value *vertexIdx, Instruction *insertPos) {
  BuilderBase builder(insertPos);

  auto outputTy = output->getType();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto &builtInUsage = resUsage->builtInUsage.tcs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
  const auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;

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
    writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, builder);

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
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, elemIdx, vertexIdx, builder);
        writeValueToLds(m_pipelineState->isTessOffChip(), elem, ldsOffset, builder);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, builder);
      writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, builder);
    }

    break;
  }
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner: {
    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

    // tessLevelOuter (float[4]) + tessLevelInner (float[2])
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + elemIdx
    uint32_t tessFactorStart = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)
                                   ->inOutUsage.tcs.calcFactor.onChip.tessFactorStart;
    if (builtInId == BuiltInTessLevelInner)
      tessFactorStart += 4;

    // Write tessellation factors to on-chip LDS for later TF buffer store
    Value *baseOffset = builder.CreateMul(relativeId, builder.getInt32(MaxTessFactorsPerPatch));
    if (outputTy->isArrayTy()) {
      // Handle the whole tessLevelOuter array
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        Value *ldsOffset = builder.CreateAdd(baseOffset, builder.getInt32(tessFactorStart + i));
        auto elem = builder.CreateExtractValue(output, {i});
        writeValueToLds(false, elem, ldsOffset, builder);
      }
    } else {
      // Handle a single element of tessLevelOuter array
      Value *ldsOffset = builder.CreateAdd(baseOffset, builder.getInt32(tessFactorStart));
      ldsOffset = builder.CreateAdd(ldsOffset, elemIdx, "", insertPos);
      writeValueToLds(false, output, ldsOffset, builder);
    }

    // Write tessellation factors for TES to read if needed
    if (perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end()) {
      unsigned loc = perPatchBuiltInOutLocMap.find(builtInId)->second;

      if (outputTy->isArrayTy()) {
        // Handle the whole tessLevelOuter array
        for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
          auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, builder.getInt32(i), nullptr, builder);
          auto elem = builder.CreateExtractValue(output, {i});
          writeValueToLds(m_pipelineState->isTessOffChip(), elem, ldsOffset, builder);
        }
      } else {
        // Handle a single element of tessLevelOuter array
        auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, nullptr, builder);
        writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, builder);
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
// Patches export calls for built-in outputs of tessellation evaluation shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchTesBuiltInOutputExport(Value *output, unsigned builtInId, Instruction *insertPos) {
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  auto &builtInUsage = resUsage->builtInUsage.tes;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

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
        builtInUsage.position = false;
        return;
      case BuiltInPointSize:
        builtInUsage.pointSize = false;
        return;
      case BuiltInClipDistance:
        builtInUsage.clipDistance = 0;
        return;
      case BuiltInCullDistance:
        builtInUsage.cullDistance = 0;
        return;
      default:
        llvm_unreachable("unhandled builtInId");
      }
    }

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
    } else {
      switch (builtInId) {
      case BuiltInPosition:
      case BuiltInPointSize:
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
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

      storeValueToEsGsRing(output, loc, 0, insertPos);
    }

    break;
  }
  case BuiltInViewportIndex: {
    if (!static_cast<bool>(builtInUsage.viewportIndex))
      return;

    // NOTE: Only last vertex processing shader stage has to export the value of gl_ViewportIndex.
    if (!m_hasGs) {
      if (m_gfxIp.major <= 8)
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
      else {
        // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
        m_viewportIndex = output;
      }
    } else {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
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
// Patches export calls for built-in outputs of geometry shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param streamId : ID of output vertex stream
// @param builder : the builder to use
void PatchInOutImportExport::patchGsBuiltInOutputExport(Value *output, unsigned builtInId, unsigned streamId,
                                                        BuilderBase &builder) {
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
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
// Patches export calls for built-in outputs of mesh shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexOrPrimitiveIdx : Output array outermost index used for vertex or primitive indexing
// @param isPerPrimitive : Whether the output is per-primitive
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchMeshBuiltInOutputExport(Value *output, unsigned builtInId, Value *elemIdx,
                                                          Value *vertexOrPrimitiveIdx, bool isPerPrimitive,
                                                          Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

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
  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
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

  // outputOffset = location * 4 + elemIdx
  Value *outputOffset = builder.getInt32(4 * loc);
  if (elemIdx)
    outputOffset = builder.CreateAdd(builder.getInt32(4 * loc), elemIdx);

  if (isPerPrimitive)
    builder.create<WriteMeshPrimitiveOutputOp>(outputOffset, vertexOrPrimitiveIdx, output);
  else
    builder.create<WriteMeshVertexOutputOp>(outputOffset, vertexOrPrimitiveIdx, output);
}

// =====================================================================================================================
// Patches export calls for built-in outputs of fragment shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchFsBuiltInOutputExport(Value *output, unsigned builtInId, Instruction *insertPos) {
  switch (builtInId) {
  case BuiltInFragDepth: {
    m_fragDepth = output;
    break;
  }
  case BuiltInSampleMask: {
    assert(output->getType()->isArrayTy());

    // NOTE: Only gl_SampleMask[0] is valid for us.
    m_sampleMask = ExtractValueInst::Create(output, {0}, "", insertPos);
    m_sampleMask = new BitCastInst(m_sampleMask, Type::getFloatTy(*m_context), "", insertPos);
    break;
  }
  case BuiltInFragStencilRef: {
    m_fragStencilRef = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Patches export calls for generic outputs of copy shader.
//
// @param output : Output value
// @param location : Location of the output
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchCopyShaderGenericOutputExport(Value *output, unsigned location,
                                                                Instruction *insertPos) {
  addExportInstForGenericOutput(output, location, 0, insertPos);
}

// =====================================================================================================================
// Patches export calls for built-in outputs of copy shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchCopyShaderBuiltInOutputExport(Value *output, unsigned builtInId,
                                                                Instruction *insertPos) {
  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize: {
    addExportInstForBuiltInOutput(output, builtInId, insertPos);
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
    if (m_gfxIp.major <= 8)
      addExportInstForBuiltInOutput(output, builtInId, insertPos);
    else {
      // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
      m_viewportIndex = output;
    }

    break;
  }
  case BuiltInPrimitiveShadingRate: {
    // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
    assert(m_gfxIp >= GfxIpVersion({10, 3}));
    addExportInstForBuiltInOutput(output, builtInId, insertPos);

    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Patch export calls for transform feedback outputs of vertex shader and tessellation evaluation shader.
//
// @param output : Output value
// @param xfbBuffer : Transform feedback buffer ID
// @param xfbOffset : Transform feedback offset
// @param streamId : Output stream ID
// @param builder : The IR builder to create and insert IR instruction
void PatchInOutImportExport::patchXfbOutputExport(Value *output, unsigned xfbBuffer, unsigned xfbOffset,
                                                  unsigned streamId, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader);

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
unsigned PatchInOutImportExport::combineBufferStore(const std::vector<Value *> &storeValues, unsigned startIdx,
                                                    unsigned valueOffset, Value *bufDesc, Value *storeOffset,
                                                    Value *bufBase, CoherentFlag coherent, BuilderBase &builder) {
  Type *storeTys[4] = {
      Type::getInt32Ty(*m_context),
      FixedVectorType::get(Type::getInt32Ty(*m_context), 2),
      FixedVectorType::get(Type::getInt32Ty(*m_context), 3),
      FixedVectorType::get(Type::getInt32Ty(*m_context), 4),
  };

  std::string funcName = "llvm.amdgcn.raw.tbuffer.store.";

  // Start from 4-component combination
  unsigned compCount = 4;
  for (; compCount > 0; compCount--) {
    // GFX6 does not support 3-component combination
    if (m_gfxIp.major == 6 && compCount == 3)
      continue;

    if (startIdx + compCount <= storeValues.size()) {
      funcName += getTypeName(storeTys[compCount - 1]);
      Value *storeValue = nullptr;
      if (compCount > 1) {
        auto storeTy = FixedVectorType::get(Type::getInt32Ty(*m_context), compCount);
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
unsigned PatchInOutImportExport::combineBufferLoad(std::vector<Value *> &loadValues, unsigned startIdx, Value *bufDesc,
                                                   Value *loadOffset, Value *bufBase, CoherentFlag coherent,
                                                   BuilderBase &builder) {
  Type *loadTyps[4] = {
      Type::getInt32Ty(*m_context),
      FixedVectorType::get(Type::getInt32Ty(*m_context), 2),
      FixedVectorType::get(Type::getInt32Ty(*m_context), 3),
      FixedVectorType::get(Type::getInt32Ty(*m_context), 4),
  };

  std::string funcName = "llvm.amdgcn.raw.tbuffer.load.";
  assert(loadValues.size() > 0);

  // 4-component combination
  unsigned compCount = 4;
  for (; compCount > 0; compCount--) {
    // GFX6 does not support 3-component combination
    if (m_gfxIp.major == 6 && compCount == 3)
      continue;

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
void PatchInOutImportExport::storeValueToStreamOutBuffer(Value *storeValue, unsigned xfbBuffer, unsigned xfbOffset,
                                                         unsigned xfbStride, unsigned streamId, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader);
  assert(xfbBuffer < MaxTransformFeedbackBuffers);

  if (m_pipelineState->enableSwXfb()) {
    // NOTE: For GFX11+, exporting transform feedback outputs is represented by a call and the call is
    // replaced with real instructions when when NGG primitive shader is generated.
    std::string callName = lgcName::NggXfbExport + getTypeName(storeValue->getType());
    builder.CreateNamedCall(
        callName, builder.getVoidTy(),
        {builder.getInt32(xfbBuffer), builder.getInt32(xfbOffset), builder.getInt32(streamId), storeValue}, {});
    return;
  }

  auto storeTy = storeValue->getType();

  unsigned compCount = storeTy->isVectorTy() ? cast<FixedVectorType>(storeTy)->getNumElements() : 1;
  assert(compCount <= 4);

  const uint64_t bitWidth = storeTy->getScalarSizeInBits();
  assert(bitWidth == 16 || bitWidth == 32);

  if (storeTy->isIntOrIntVectorTy(16)) {
    Type *newStoreTy = compCount > 1 ? FixedVectorType::get(builder.getHalfTy(), compCount) : builder.getHalfTy();
    storeValue = builder.CreateBitCast(storeValue, newStoreTy);
    storeTy = newStoreTy;
  }

  // NOTE: For 16vec3, HW doesn't have a corresponding buffer store instruction. We have to split it to 16vec2 and
  // 16scalar.
  if (bitWidth == 16 && compCount == 3) {
    // 16vec3 -> 16vec2 + 16scalar
    Value *compX2 = builder.CreateShuffleVector(storeValue, {0, 1});
    storeValueToStreamOutBuffer(compX2, xfbBuffer, xfbOffset, xfbStride, streamId, builder);

    Value *comp = builder.CreateExtractElement(storeValue, 2);
    xfbOffset += 2 * (bitWidth / 8);
    storeValueToStreamOutBuffer(comp, xfbBuffer, xfbOffset, xfbStride, streamId, builder);

    return;
  }

  Value *streamInfo = nullptr;
  Value *writeIndex = nullptr;
  Value *streamOffset = nullptr;

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
  if (m_shaderStage == ShaderStageVertex) {
    streamInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.streamOutData.streamInfo);
    writeIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.streamOutData.writeIndex);
    streamOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.streamOutData.streamOffsets[xfbBuffer]);
  } else if (m_shaderStage == ShaderStageTessEval) {
    streamInfo = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.streamOutData.streamInfo);
    writeIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.streamOutData.writeIndex);
    streamOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.streamOutData.streamOffsets[xfbBuffer]);
  } else {
    assert(m_shaderStage == ShaderStageCopyShader);

    streamInfo = getFunctionArgument(m_entryPoint, CopyShaderEntryArgIdxStreamInfo);
    writeIndex = getFunctionArgument(m_entryPoint, CopyShaderEntryArgIdxWriteIndex);
    streamOffset = getFunctionArgument(m_entryPoint, CopyShaderEntryArgIdxStreamOffset + xfbBuffer);
  }

  // vertexCount = streamInfo[22:16]
  Value *vertexCount = builder.CreateAnd(builder.CreateLShr(streamInfo, 16), 0x7F);

  // writeIndex += threadIdInWave
  if (m_gfxIp.major >= 9)
    writeIndex = builder.CreateAdd(writeIndex, m_threadId);

  // The stream offset provided by GE is dword-based. Convert it to byte-based.
  streamOffset = builder.CreateShl(streamOffset, 2);

  // GPU will drop stream-out buffer store when the thread ID is invalid.
  unsigned outOfRangeWriteIndex = 0xFFFFFFFF;
  if (m_gfxIp.major == 8) {
    // Divide outofRangeValue by xfbStride only for GFX8.
    outOfRangeWriteIndex /= xfbStride;
  }
  outOfRangeWriteIndex -= (m_pipelineState->getShaderWaveSize(m_shaderStage) - 1);
  auto validVertex = builder.CreateICmpULT(m_threadId, vertexCount);
  writeIndex = builder.CreateSelect(validVertex, writeIndex, builder.getInt32(outOfRangeWriteIndex));

  unsigned format = 0;
  switch (m_gfxIp.major) {
  default: {
    CombineFormat combineFormat = {};
    combineFormat.bits.nfmt = BUF_NUM_FORMAT_FLOAT;
    static const unsigned char dfmtTable[4][2] = {
        {BUF_DATA_FORMAT_16, BUF_DATA_FORMAT_32},
        {BUF_DATA_FORMAT_16_16, BUF_DATA_FORMAT_32_32},
        {BUF_DATA_FORMAT_INVALID, BUF_DATA_FORMAT_32_32_32},
        {BUF_DATA_FORMAT_16_16_16_16, BUF_DATA_FORMAT_32_32_32_32},
    };
    combineFormat.bits.dfmt = dfmtTable[compCount - 1][bitWidth == 32];
    format = combineFormat.u32All;
    break;
  }
  case 10: {
    static unsigned char formatTable[4][2] = {
        {BUF_FORMAT_16_FLOAT, BUF_FORMAT_32_FLOAT},
        {BUF_FORMAT_16_16_FLOAT, BUF_FORMAT_32_32_FLOAT_GFX10},
        {BUF_FORMAT_INVALID, BUF_FORMAT_32_32_32_FLOAT_GFX10},
        {BUF_FORMAT_16_16_16_16_FLOAT_GFX10, BUF_FORMAT_32_32_32_32_FLOAT_GFX10},
    };
    format = formatTable[compCount - 1][bitWidth == 32];
    break;
  }
  case 11: {
    static unsigned char formatTable[4][2] = {
        {BUF_FORMAT_16_FLOAT, BUF_FORMAT_32_FLOAT},
        {BUF_FORMAT_16_16_FLOAT, BUF_FORMAT_32_32_FLOAT_GFX11},
        {},
        {BUF_FORMAT_16_16_16_16_FLOAT_GFX11, BUF_FORMAT_32_32_32_32_FLOAT_GFX11},
    };
    format = formatTable[compCount - 1][bitWidth == 32];
    break;
  }
  }

  CoherentFlag coherent = {};
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    coherent.bits.glc = true;
    coherent.bits.slc = true;
  }

  builder.CreateIntrinsic(Intrinsic::amdgcn_struct_tbuffer_store, storeTy,
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
// @param insertPos : Where to insert the store instruction
void PatchInOutImportExport::storeValueToEsGsRing(Value *storeValue, unsigned location, unsigned compIdx,
                                                  Instruction *insertPos) {
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
        storeElem = ExtractValueInst::Create(storeValue, {i}, "", insertPos);
      else {
        storeElem =
            ExtractElementInst::Create(storeValue, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
      }

      storeValueToEsGsRing(storeElem, location + (compIdx + i) / 4, (compIdx + i) % 4, insertPos);
    }
  } else {
    if (bitWidth == 8 || bitWidth == 16) {
      if (storeTy->isFloatingPointTy()) {
        assert(bitWidth == 16);
        storeValue = new BitCastInst(storeValue, Type::getInt16Ty(*m_context), "", insertPos);
      }

      storeValue = new ZExtInst(storeValue, Type::getInt32Ty(*m_context), "", insertPos);
    } else {
      assert(bitWidth == 32);
      if (storeTy->isFloatingPointTy())
        storeValue = new BitCastInst(storeValue, Type::getInt32Ty(*m_context), "", insertPos);
    }

    // Call buffer store intrinsic or LDS store
    const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
    Value *esGsOffset = nullptr;
    if (m_shaderStage == ShaderStageVertex)
      esGsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.esGsOffset);
    else {
      assert(m_shaderStage == ShaderStageTessEval);
      esGsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.tes.esGsOffset);
    }

    auto ringOffset = calcEsGsRingOffsetForOutput(location, compIdx, esGsOffset, insertPos);

    if (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9) // ES -> GS ring is always on-chip on GFX9+
    {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ringOffset};
      auto ldsType = m_lds->getValueType();
      Value *storePtr = GetElementPtrInst::Create(ldsType, m_lds, idxs, "", insertPos);
      new StoreInst(storeValue, storePtr, false, m_lds->getAlign().value(), insertPos);
    } else {
      Value *esGsRingBufDesc = m_pipelineSysValues.get(m_entryPoint)->getEsGsRingBufDesc();

      // NOTE: Here we use tbuffer_store instruction instead of buffer_store because we have to do explicit control
      // of soffset. This is required by swizzle enabled mode when address range checking should be complied with.
      CombineFormat combineFormat = {};
      combineFormat.bits.dfmt = BUF_DATA_FORMAT_32;
      combineFormat.bits.nfmt = BUF_NUM_FORMAT_UINT;
      CoherentFlag coherent = {};
      coherent.bits.glc = true;
      coherent.bits.slc = true;
      coherent.bits.swz = true;
      Value *args[] = {
          storeValue,      // vdata
          esGsRingBufDesc, // rsrc
          ringOffset,      // voffset
          esGsOffset,      // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), combineFormat.u32All),
          ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All) // glc, slc, swz
      };
      emitCall("llvm.amdgcn.raw.tbuffer.store.i32", Type::getVoidTy(*m_context), args, {}, insertPos);
    }
  }
}

// =====================================================================================================================
// Loads value from ES-GS ring (buffer or LDS).
//
// @param loadTy : Load value type
// @param location : Input location
// @param compIdx : Input component index
// @param vertexIdx : Vertex index
// @param insertPos : Where to insert the load instruction
Value *PatchInOutImportExport::loadValueFromEsGsRing(Type *loadTy, unsigned location, unsigned compIdx,
                                                     Value *vertexIdx, Instruction *insertPos) {
  Type *elemTy = loadTy;
  if (loadTy->isArrayTy())
    elemTy = cast<ArrayType>(loadTy)->getElementType();
  else if (loadTy->isVectorTy())
    elemTy = cast<VectorType>(loadTy)->getElementType();

  const uint64_t bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  Value *loadValue = PoisonValue::get(loadTy);

  if (loadTy->isArrayTy() || loadTy->isVectorTy()) {
    const unsigned elemCount = loadTy->isArrayTy() ? cast<ArrayType>(loadTy)->getNumElements()
                                                   : cast<FixedVectorType>(loadTy)->getNumElements();

    for (unsigned i = 0; i < elemCount; ++i) {
      auto loadElem =
          loadValueFromEsGsRing(elemTy, location + (compIdx + i) / 4, (compIdx + i) % 4, vertexIdx, insertPos);

      if (loadTy->isArrayTy())
        loadValue = InsertValueInst::Create(loadValue, loadElem, {i}, "", insertPos);
      else {
        loadValue = InsertElementInst::Create(loadValue, loadElem, ConstantInt::get(Type::getInt32Ty(*m_context), i),
                                              "", insertPos);
      }
    }
  } else {
    Value *ringOffset = calcEsGsRingOffsetForInput(location, compIdx, vertexIdx, insertPos);
    if (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9) // ES -> GS ring is always on-chip on GFX9
    {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ringOffset};
      auto ldsType = m_lds->getValueType();
      auto *loadPtr = GetElementPtrInst::Create(ldsType, m_lds, idxs, "", insertPos);
      auto loadInst =
          new LoadInst(loadPtr->getResultElementType(), loadPtr, "", false, m_lds->getAlign().value(), insertPos);
      loadValue = loadInst;

      if (bitWidth == 8)
        loadValue = new TruncInst(loadValue, Type::getInt8Ty(*m_context), "", insertPos);
      else if (bitWidth == 16)
        loadValue = new TruncInst(loadValue, Type::getInt16Ty(*m_context), "", insertPos);

      if (loadTy->isFloatingPointTy())
        loadValue = new BitCastInst(loadValue, loadTy, "", insertPos);
    } else {
      Value *esGsRingBufDesc = m_pipelineSysValues.get(m_entryPoint)->getEsGsRingBufDesc();
      CoherentFlag coherent = {};
      coherent.bits.glc = true;
      coherent.bits.slc = true;
      Value *args[] = {
          esGsRingBufDesc,                                                // rsrc
          ringOffset,                                                     // offset
          ConstantInt::get(Type::getInt32Ty(*m_context), 0),              // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All) // glc slc
      };
      loadValue = emitCall("llvm.amdgcn.raw.buffer.load.f32", Type::getFloatTy(*m_context), args, {}, insertPos);

      if (bitWidth == 8) {
        assert(loadTy->isIntegerTy());

        loadValue = new BitCastInst(loadValue, Type::getInt32Ty(*m_context), "", insertPos);
        loadValue = new TruncInst(loadValue, Type::getInt8Ty(*m_context), "", insertPos);
      } else if (bitWidth == 16) {
        loadValue = new BitCastInst(loadValue, Type::getInt32Ty(*m_context), "", insertPos);
        loadValue = new TruncInst(loadValue, Type::getInt16Ty(*m_context), "", insertPos);

        if (loadTy->isFloatingPointTy())
          loadValue = new BitCastInst(loadValue, loadTy, "", insertPos);
      } else {
        assert(bitWidth == 32);
        if (loadTy->isIntegerTy())
          loadValue = new BitCastInst(loadValue, loadTy, "", insertPos);
      }
    }
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
void PatchInOutImportExport::storeValueToGsVsRing(Value *storeValue, unsigned location, unsigned compIdx,
                                                  unsigned streamId, BuilderBase &builder) {
  auto storeTy = storeValue->getType();

  Type *elemTy = storeTy;
  if (storeTy->isArrayTy())
    elemTy = cast<ArrayType>(storeTy)->getElementType();
  else if (storeTy->isVectorTy())
    elemTy = cast<VectorType>(storeTy)->getElementType();

  const unsigned bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  if (m_pipelineState->getNggControl()->enableNgg) {
    // NOTE: For NGG, writing GS output to GS-VS ring is represented by a call and the call is replaced with
    // real instructions when when NGG primitive shader is generated.
    Value *args[] = {ConstantInt::get(Type::getInt32Ty(*m_context), location),
                     ConstantInt::get(Type::getInt32Ty(*m_context), compIdx),
                     ConstantInt::get(Type::getInt32Ty(*m_context), streamId), storeValue};
    std::string callName = lgcName::NggWriteGsOutput + getTypeName(storeTy);
    builder.CreateNamedCall(callName, Type::getVoidTy(*m_context), args, {});
    return;
  }

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

    const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
    Value *gsVsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.gs.gsVsOffset);

    auto emitCounterPair = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr();
    auto emitCounterTy = emitCounterPair.first;
    auto emitCounterPtr = emitCounterPair.second[streamId];
    auto emitCounter = builder.CreateLoad(emitCounterTy, emitCounterPtr);

    auto ringOffset = calcGsVsRingOffsetForOutput(location, compIdx, streamId, emitCounter, gsVsOffset, builder);

    if (m_pipelineState->isGsOnChip()) {
      Value *idxs[] = {builder.getInt32(0), ringOffset};
      auto ldsType = m_lds->getValueType();
      Value *storePtr = builder.CreateGEP(ldsType, m_lds, idxs);
      builder.CreateAlignedStore(storeValue, storePtr, m_lds->getAlign().value());
    } else {
      // NOTE: Here we use tbuffer_store instruction instead of buffer_store because we have to do explicit
      // control of soffset. This is required by swizzle enabled mode when address range checking should be
      // complied with.
      unsigned format;
      if (m_gfxIp.major <= 9) {
        CombineFormat combineFormat = {};
        combineFormat.bits.dfmt = BUF_DATA_FORMAT_32;
        combineFormat.bits.nfmt = BUF_NUM_FORMAT_UINT;
        format = combineFormat.u32All;
      } else {
        format = BUF_FORMAT_32_UINT;
      }

      CoherentFlag coherent = {};
      if (m_gfxIp.major <= 11) {
        coherent.bits.glc = true;
        coherent.bits.slc = true;
        coherent.bits.swz = true;
      }
      Value *args[] = {
          storeValue,                                                          // vdata
          m_pipelineSysValues.get(m_entryPoint)->getGsVsRingBufDesc(streamId), // rsrc
          ringOffset,                                                          // voffset
          gsVsOffset,                                                          // soffset
          builder.getInt32(format),
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
// @param insertPos : Where to insert the instruction
Value *PatchInOutImportExport::calcEsGsRingOffsetForOutput(unsigned location, unsigned compIdx, Value *esGsOffset,
                                                           Instruction *insertPos) {
  Value *ringOffset = nullptr;
  if (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9) // ES -> GS ring is always on-chip on GFX9
  {
    // ringOffset = esGsOffset + threadId * esGsRingItemSize + location * 4 + compIdx

    assert(m_pipelineState->hasShaderStage(ShaderStageGeometry));
    const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

    esGsOffset =
        BinaryOperator::CreateLShr(esGsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);

    ringOffset = BinaryOperator::CreateMul(
        m_threadId, ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.esGsRingItemSize), "", insertPos);

    ringOffset = BinaryOperator::CreateAdd(ringOffset, esGsOffset, "", insertPos);

    ringOffset = BinaryOperator::CreateAdd(
        ringOffset, ConstantInt::get(Type::getInt32Ty(*m_context), (location * 4 + compIdx)), "", insertPos);
  } else {
    // ringOffset = (location * 4 + compIdx) * 4
    ringOffset = ConstantInt::get(Type::getInt32Ty(*m_context), (location * 4 + compIdx) * 4);
  }
  return ringOffset;
}

// =====================================================================================================================
// Calculates the byte offset to load the input value from ES-GS ring based on the specified input info.
//
// @param location : Input location
// @param compIdx : Input Component index
// @param vertexIdx : Vertex index
// @param insertPos : Where to insert the instruction
Value *PatchInOutImportExport::calcEsGsRingOffsetForInput(unsigned location, unsigned compIdx, Value *vertexIdx,
                                                          Instruction *insertPos) {
  Value *ringOffset = nullptr;
  auto esGsOffsets = m_pipelineSysValues.get(m_entryPoint)->getEsGsOffsets();

  if (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9) // ES -> GS ring is always on-chip on GFX9
  {
    Value *vertexOffset = ExtractElementInst::Create(esGsOffsets, vertexIdx, "", insertPos);

    // ringOffset = vertexOffset[N] + (location * 4 + compIdx);
    ringOffset = BinaryOperator::CreateAdd(
        vertexOffset, ConstantInt::get(Type::getInt32Ty(*m_context), (location * 4 + compIdx)), "", insertPos);
  } else {
    Value *vertexOffset = ExtractElementInst::Create(esGsOffsets, vertexIdx, "", insertPos);

    // ringOffset = vertexOffset[N] * 4 + (location * 4 + compIdx) * 64 * 4;
    ringOffset =
        BinaryOperator::CreateMul(vertexOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

    ringOffset = BinaryOperator::CreateAdd(
        ringOffset, ConstantInt::get(Type::getInt32Ty(*m_context), (location * 4 + compIdx) * 64 * 4), "", insertPos);
  }

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
Value *PatchInOutImportExport::calcGsVsRingOffsetForOutput(unsigned location, unsigned compIdx, unsigned streamId,
                                                           Value *vertexIdx, Value *gsVsOffset, BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

  Value *ringOffset = nullptr;

  unsigned streamBases[MaxGsStreams];
  unsigned streamBase = 0;
  for (int i = 0; i < MaxGsStreams; ++i) {
    streamBases[i] = streamBase;
    streamBase += (resUsage->inOutUsage.gs.outLocCount[i] *
                   m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices * 4);
  }

  if (m_pipelineState->isGsOnChip()) {
    // ringOffset = esGsLdsSize +
    //              gsVsOffset +
    //              threadId * gsVsRingItemSize +
    //              (vertexIdx * vertexSizePerStream) + location * 4 + compIdx + streamBase (in dwords)

    auto esGsLdsSize = builder.getInt32(resUsage->inOutUsage.gs.calcFactor.esGsLdsSize);

    gsVsOffset = builder.CreateLShr(gsVsOffset, 2, "", /*isExact=*/true);

    auto ringItemOffset =
        builder.CreateMul(m_threadId, builder.getInt32(resUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize));

    // VertexSize is stream output vertexSize x 4 (in dwords)
    unsigned vertexSize = resUsage->inOutUsage.gs.outLocCount[streamId] * 4;
    auto vertexItemOffset = builder.CreateMul(vertexIdx, builder.getInt32(vertexSize));
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
Value *PatchInOutImportExport::readValueFromLds(bool offChip, Type *readTy, Value *ldsOffset, BuilderBase &builder) {
  assert(m_lds);
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
        m_shaderStage == ShaderStageTessEval
            ? m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tes.offChipLdsBase
            : m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs.offChipLdsBase;

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, offChipLdsBaseArgIdx);

    // Convert dword off-chip LDS offset to byte offset
    ldsOffset = builder.CreateMul(ldsOffset, builder.getInt32(4));

    CoherentFlag coherent = {};
    if (m_gfxIp.major <= 9)
      coherent.bits.glc = true;
    else if (m_gfxIp.major == 10) {
      coherent.bits.glc = true;
      coherent.bits.dlc = true;
    } else if (m_gfxIp.major == 11) {
      // NOTE: dlc depends on MALL NOALLOC which isn't used by now.
      coherent.bits.glc = true;
    } else
      llvm_unreachable("Not implemented!");

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount)
      combineCount = combineBufferLoad(loadValues, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, builder);
  } else {
    // Read from on-chip LDS
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *idxs[] = {builder.getInt32(0), ldsOffset};
      auto ldsType = m_lds->getValueType();
      auto *loadPtr = builder.CreateGEP(ldsType, m_lds, idxs);
      auto loadTy = GetElementPtrInst::getIndexedType(ldsType, idxs);
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
    auto intTy = bitWidth == 32 || bitWidth == 64
                     ? Type::getInt32Ty(*m_context)
                     : (bitWidth == 16 ? Type::getInt16Ty(*m_context) : Type::getInt8Ty(*m_context));
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
void PatchInOutImportExport::writeValueToLds(bool offChip, Value *writeValue, Value *ldsOffset, BuilderBase &builder) {
  assert(m_lds);

  auto writeTy = writeValue->getType();
  assert(writeTy->isSingleValueType());

  const unsigned compCout = writeTy->isVectorTy() ? cast<FixedVectorType>(writeTy)->getNumElements() : 1;
  const unsigned bitWidth = writeTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);
  const unsigned numChannels = compCout * (bitWidth == 64 ? 2 : 1);

  // Cast write value to <n x i32> vector
  Type *intTy = bitWidth == 32 || bitWidth == 64
                    ? Type::getInt32Ty(*m_context)
                    : (bitWidth == 16 ? Type::getInt16Ty(*m_context) : Type::getInt8Ty(*m_context));
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
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs;

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, entryArgIdxs.offChipLdsBase);
    // Convert dword off-chip LDS offset to byte offset
    ldsOffset = builder.CreateMul(ldsOffset, builder.getInt32(4));

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    CoherentFlag coherent = {};
    if (m_gfxIp.major <= 11)
      coherent.bits.glc = true;

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount) {
      combineCount =
          combineBufferStore(storeValues, i, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, builder);
    }
  } else {
    // Write to on-chip LDS
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *idxs[] = {builder.getInt32(0), ldsOffset};
      auto ldsType = m_lds->getValueType();
      Value *storePtr = builder.CreateGEP(ldsType, m_lds, idxs);
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
Value *PatchInOutImportExport::calcLdsOffsetForVsOutput(Type *outputTy, unsigned location, unsigned compIdx,
                                                        BuilderBase &builder) {
  assert(m_shaderStage == ShaderStageVertex);

  // attribOffset = location * 4 + compIdx
  Value *attribOffset = builder.getInt32(location * 4);

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  if (bitWidth == 64) {
    // For 64-bit data type, the component indexing must multiply by 2
    compIdx *= 2;
  }

  attribOffset = builder.CreateAdd(attribOffset, builder.getInt32(compIdx));

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
  auto relVertexId = getFunctionArgument(m_entryPoint, entryArgIdxs.relVertexId);

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  auto vertexStride = builder.getInt32(calcFactor.inVertexStride);

  // dwordOffset = relVertexId * vertexStride + attribOffset
  auto ldsOffset = builder.CreateMul(relVertexId, vertexStride);
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
Value *PatchInOutImportExport::calcLdsOffsetForTcsInput(Type *inputTy, unsigned location, Value *locOffset,
                                                        Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStageTessControl);

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
  const auto &calcFactor = inOutUsage.calcFactor;

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

  // dwordOffset = (relativeId * inVertexCount + vertexId) * inVertexStride + attribOffset
  auto inVertexCount = m_pipelineState->getNumPatchControlPoints();

  auto inVertexCountVal = builder.getInt32(inVertexCount);
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

  Value *ldsOffset = builder.CreateMul(relativeId, inVertexCountVal);
  ldsOffset = builder.CreateAdd(ldsOffset, vertexIdx);

  auto inVertexStride = builder.getInt32(calcFactor.inVertexStride);
  ldsOffset = builder.CreateMul(ldsOffset, inVertexStride);

  ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);

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
Value *PatchInOutImportExport::calcLdsOffsetForTcsOutput(Type *outputTy, unsigned location, Value *locOffset,
                                                         Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStageTessControl);

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
  const auto &calcFactor = inOutUsage.calcFactor;

  auto outPatchStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.outPatchStart : calcFactor.onChip.outPatchStart;

  auto patchConstStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.patchConstStart : calcFactor.onChip.patchConstStart;

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

  const bool perPatch = (!vertexIdx); // Vertex indexing is unavailable for per-patch output
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
  if (perPatch) {
    // dwordOffset = patchConstStart + relativeId * patchConstSize + attribOffset
    auto patchConstSize = builder.getInt32(calcFactor.patchConstSize);
    ldsOffset = builder.CreateMul(relativeId, patchConstSize);

    auto patchConstStartVal = builder.getInt32(patchConstStart);
    ldsOffset = builder.CreateAdd(ldsOffset, patchConstStartVal);

    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  } else {
    // dwordOffset = outPatchStart + (relativeId * outVertexCount + vertexId) * outVertexStride + attribOffset
    //             = outPatchStart + relativeId * outPatchSize + vertexId  * outVertexStride + attribOffset
    auto outPatchSize = builder.getInt32(calcFactor.outPatchSize);
    ldsOffset = builder.CreateMul(relativeId, outPatchSize);

    auto outPatchStartVal = builder.getInt32(outPatchStart);
    ldsOffset = builder.CreateAdd(ldsOffset, outPatchStartVal);

    auto outVertexStride = builder.getInt32(calcFactor.outVertexStride);
    ldsOffset = builder.CreateAdd(ldsOffset, builder.CreateMul(vertexIdx, outVertexStride));

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
Value *PatchInOutImportExport::calcLdsOffsetForTesInput(Type *inputTy, unsigned location, Value *locOffset,
                                                        Value *compIdx, Value *vertexIdx, BuilderBase &builder) {
  assert(m_shaderStage == ShaderStageTessEval);

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;

  auto outPatchStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.outPatchStart : calcFactor.onChip.outPatchStart;

  auto patchConstStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.patchConstStart : calcFactor.onChip.patchConstStart;

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tes;

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

  const bool perPatch = (!vertexIdx); // Vertex indexing is unavailable for per-patch input
  if (perPatch) {
    // dwordOffset = patchConstStart + relPatchId * patchConstSize + attribOffset
    auto patchConstSize = builder.getInt32(calcFactor.patchConstSize);
    ldsOffset = builder.CreateMul(relPatchId, patchConstSize);

    auto patchConstStartVal = builder.getInt32(patchConstStart);
    ldsOffset = builder.CreateAdd(ldsOffset, patchConstStartVal);

    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  } else {
    // dwordOffset = patchStart + (relPatchId * vertexCount + vertexId) * vertexStride + attribOffset
    //             = patchStart + relPatchId * patchSize + vertexId  * vertexStride + attribOffset
    auto patchSize = builder.getInt32(calcFactor.outPatchSize);
    ldsOffset = builder.CreateMul(relPatchId, patchSize);

    auto patchStart = builder.getInt32(outPatchStart);
    ldsOffset = builder.CreateAdd(ldsOffset, patchStart);

    auto vertexStride = builder.getInt32(calcFactor.outVertexStride);
    ldsOffset = builder.CreateAdd(ldsOffset, builder.CreateMul(vertexIdx, vertexStride));

    ldsOffset = builder.CreateAdd(ldsOffset, attribOffset);
  }

  return ldsOffset;
}

// =====================================================================================================================
// Calculates the patch count for per-thread group.
//
// @param inVertexCount : Count of vertices of input patch
// @param inVertexStride : Vertex stride of input patch in (dwords)
// @param outVertexCount : Count of vertices of output patch
// @param outVertexStride : Vertex stride of output patch in (dwords)
// @param patchConstCount : Count of output patch constants
// @param tessFactorStride : Stride of tessellation factors (dwords)
unsigned PatchInOutImportExport::calcPatchCountPerThreadGroup(unsigned inVertexCount, unsigned inVertexStride,
                                                              unsigned outVertexCount, unsigned outVertexStride,
                                                              unsigned patchConstCount,
                                                              unsigned tessFactorStride) const {
  unsigned maxThreadCountPerThreadGroup =
      m_gfxIp.major >= 9 ? Gfx9::MaxHsThreadsPerSubgroup : Gfx6::MaxHsThreadsPerSubgroup;

  // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
  // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
  // restriction by providing the capability of querying thread ID in the group rather than in wave.
  unsigned rayQueryLdsStackSize = 0;
  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  const auto tcsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  if (vsResUsage->useRayQueryLdsStack || tcsResUsage->useRayQueryLdsStack) {
    maxThreadCountPerThreadGroup = std::min(MaxRayQueryThreadsPerGroup, maxThreadCountPerThreadGroup);
    rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;
  }

  const unsigned maxThreadCountPerPatch = std::max(inVertexCount, outVertexCount);
  const unsigned patchCountLimitedByThread = maxThreadCountPerThreadGroup / maxThreadCountPerPatch;

  const unsigned inPatchSize = (inVertexCount * inVertexStride);
  const unsigned outPatchSize = (outVertexCount * outVertexStride);
  const unsigned patchConstSize = patchConstCount * 4;

  // Compute the required LDS size per patch, always include the space for input patch and tess factor
  unsigned ldsSizePerPatch = inPatchSize + MaxTessFactorsPerPatch;

  unsigned ldsSizePerThreadGroup = m_pipelineState->getTargetInfo().getGpuProperty().ldsSizePerThreadGroup;
  if (m_pipelineState->canOptimizeTessFactor()) {
    // NOTE: If we are going to optimize TF store, we need additional on-chip LDS size. The required size is
    // 2 dwords per HS wave (1 dword all-ones flag or 1 dword all-zeros flag) plus an extra dword to
    // count actual HS patches.
    assert(m_gfxIp.major >= 11);
    const unsigned maxNumHsWaves =
        Gfx9::MaxHsThreadsPerSubgroup / m_pipelineState->getMergedShaderWaveSize(ShaderStageTessControl);
    ldsSizePerThreadGroup -= 1 + maxNumHsWaves * 2;
  }
  ldsSizePerThreadGroup -= rayQueryLdsStackSize; // Exclude LDS space used as ray query stack

  unsigned patchCountLimitedByLds = ldsSizePerThreadGroup / ldsSizePerPatch;

  unsigned patchCountPerThreadGroup = std::min(patchCountLimitedByThread, patchCountLimitedByLds);

  // NOTE: Performance analysis shows that 16 patches per thread group is an optimal upper-bound. The value is only
  // an experimental number. For GFX9. 64 is an optimal number instead.
  const unsigned optimalPatchCountPerThreadGroup = m_gfxIp.major >= 9 ? 64 : 16;

  patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, optimalPatchCountPerThreadGroup);

  if (m_pipelineState->isTessOffChip()) {
    auto outPatchLdsBufferSize = (outPatchSize + patchConstSize) * 4;
    auto tessOffChipPatchCountPerThreadGroup =
        m_pipelineState->getTargetInfo().getGpuProperty().tessOffChipLdsBufferSize / outPatchLdsBufferSize;
    patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, tessOffChipPatchCountPerThreadGroup);
  }

  // TF-Buffer-based limit for Patchers per Thread Group:
  // ---------------------------------------------------------------------------------------------

  // There is one TF Buffer per shader engine. We can do the below calculation on a per-SE basis.  It is also safe to
  // assume that one thread-group could at most utilize all of the TF Buffer.
  const unsigned tfBufferSizeInBytes =
      sizeof(unsigned) * m_pipelineState->getTargetInfo().getGpuProperty().tessFactorBufferSizePerSe;
  unsigned tfBufferPatchCountLimit = tfBufferSizeInBytes / (tessFactorStride * sizeof(unsigned));

  const auto workarounds = &m_pipelineState->getTargetInfo().getGpuWorkarounds();
  if (workarounds->gfx10.waTessFactorBufferSizeLimitGeUtcl1Underflow) {
    tfBufferPatchCountLimit /= 2;
  }

  patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, tfBufferPatchCountLimit);

  if (m_pipelineState->isTessOffChip()) {
    // For all-offchip tessellation, we need to write an additional 4-byte TCS control word to the TF buffer whenever
    // the patch-ID is zero.
    const unsigned offChipTfBufferPatchCountLimit =
        (tfBufferSizeInBytes - (patchCountPerThreadGroup * sizeof(unsigned))) / (tessFactorStride * sizeof(unsigned));
    patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, offChipTfBufferPatchCountLimit);
  }

  // Adjust the patches-per-thread-group based on hardware workarounds.
  if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx6.miscLoadBalancePerWatt != 0) {
    const unsigned waveSize = m_pipelineState->getTargetInfo().getGpuProperty().waveSize;
    // Load balance per watt is a mechanism which monitors HW utilization (num waves active, instructions issued
    // per cycle, etc.) to determine if the HW can handle the workload with fewer CUs enabled.  The SPI_LB_CU_MASK
    // register directs the SPI to stop launching waves to a CU so it will be clock-gated.  There is a bug in the
    // SPI which where that register setting is applied immediately, which causes any pending LS/HS/CS waves on
    // that CU to never be launched.
    //
    // The workaround is to limit each LS/HS threadgroup to a single wavefront: if there's only one wave, then the
    // CU can safely be turned off afterwards.  A microcode fix exists for CS but for GFX it was decided that the
    // cost in power efficiency wasn't worthwhile.
    //
    // Clamping to threads-per-wavefront / max(input control points, threads-per-patch) will make the hardware
    // launch a single LS/HS wave per thread-group.
    // For vulkan, threads-per-patch is always equal with outVertexCount.
    const unsigned maxThreadCountPerPatch = std::max(inVertexCount, outVertexCount);
    const unsigned maxPatchCount = waveSize / maxThreadCountPerPatch;

    patchCountPerThreadGroup = std::min(patchCountPerThreadGroup, maxPatchCount);
  }

  return patchCountPerThreadGroup;
}

// =====================================================================================================================
// Inserts "exp" instruction to export generic output.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert the "exp" instruction
void PatchInOutImportExport::addExportInstForGenericOutput(Value *output, unsigned location, unsigned compIdx,
                                                           Instruction *insertPos) {
  // Check if the shader stage is valid to use "exp" instruction to export output
  const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);
  const bool useExpInst = ((m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
                            m_shaderStage == ShaderStageCopyShader) &&
                           (nextStage == ShaderStageInvalid || nextStage == ShaderStageFragment));
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
  Type *exportTy =
      numChannels > 1 ? FixedVectorType::get(Type::getFloatTy(*m_context), numChannels) : Type::getFloatTy(*m_context);

  if (outputTy != exportTy) {
    if (bitWidth == 8) {
      // NOTE: For 16-bit output export, we have to cast the 8-bit value to 32-bit floating-point value.
      assert(outputTy->isIntOrIntVectorTy());
      Type *zExtTy = Type::getInt32Ty(*m_context);
      zExtTy = outputTy->isVectorTy() ? cast<Type>(FixedVectorType::get(zExtTy, compCount)) : zExtTy;
      exportInst = new ZExtInst(output, zExtTy, "", insertPos);
      exportInst = new BitCastInst(exportInst, exportTy, "", insertPos);
    } else if (bitWidth == 16) {
      // NOTE: For 16-bit output export, we have to cast the 16-bit value to 32-bit floating-point value.
      if (outputTy->isFPOrFPVectorTy()) {
        Type *bitCastTy = Type::getInt16Ty(*m_context);
        bitCastTy = outputTy->isVectorTy() ? cast<Type>(FixedVectorType::get(bitCastTy, compCount)) : bitCastTy;
        exportInst = new BitCastInst(output, bitCastTy, "", insertPos);
      } else {
        assert(outputTy->isIntOrIntVectorTy());
        exportInst = output;
      }

      Type *zExtTy = Type::getInt32Ty(*m_context);
      zExtTy = outputTy->isVectorTy() ? cast<Type>(FixedVectorType::get(zExtTy, compCount)) : zExtTy;
      exportInst = new ZExtInst(exportInst, zExtTy, "", insertPos);
      exportInst = new BitCastInst(exportInst, exportTy, "", insertPos);
    } else {
      assert(canBitCast(outputTy, exportTy));
      exportInst = new BitCastInst(output, exportTy, "", insertPos);
    }
  } else
    exportInst = output;

  assert(numChannels <= 8);
  Value *exportValues[8] = {nullptr};

  if (numChannels == 1)
    exportValues[0] = exportInst;
  else {
    for (unsigned i = 0; i < numChannels; ++i) {
      exportValues[i] =
          ExtractElementInst::Create(exportInst, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }
  }

  auto poison = PoisonValue::get(Type::getFloatTy(*m_context));
  if (numChannels <= 4) {
    assert(startChannel + numChannels <= 4);

    Value *attribValues[4] = {poison, poison, poison, poison};
    for (unsigned i = startChannel; i < startChannel + numChannels; ++i)
      attribValues[i] = exportValues[i - startChannel];

    m_expLocs.insert(location);
    recordVertexAttribExport(location, {attribValues[0], attribValues[1], attribValues[2], attribValues[3]});
  } else {
    // We have to do exporting twice for this output
    assert(startChannel == 0); // Other values are disallowed according to GLSL spec
    assert(numChannels == 6 || numChannels == 8);

    Value *attribValues[8] = {poison, poison, poison, poison, poison, poison, poison, poison};
    for (unsigned i = 0; i < numChannels; ++i)
      attribValues[i] = exportValues[i];

    m_expLocs.insert(location); // First export
    recordVertexAttribExport(location, {attribValues[0], attribValues[1], attribValues[2], attribValues[3]});

    m_expLocs.insert(location + 1); // Second export
    recordVertexAttribExport(location + 1, {attribValues[4], attribValues[5], attribValues[6], attribValues[7]});
  }
}

// =====================================================================================================================
// Inserts "exp" instruction to export built-in output.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the "exp" instruction
void PatchInOutImportExport::addExportInstForBuiltInOutput(Value *output, unsigned builtInId, Instruction *insertPos) {
  // Check if the shader stage is valid to use "exp" instruction to export output
  const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);
  const bool useExpInst = ((m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
                            m_shaderStage == ShaderStageCopyShader) &&
                           (nextStage == ShaderStageFragment || nextStage == ShaderStageInvalid));
  assert(useExpInst);
  (void(useExpInst)); // unused

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  const auto &builtInOutLocs =
      m_shaderStage == ShaderStageCopyShader ? inOutUsage.gs.builtInOutLocs : inOutUsage.builtInOutputLocMap;

  const auto poison = PoisonValue::get(Type::getFloatTy(*m_context));

  switch (builtInId) {
  case BuiltInPosition: {
    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_0), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),              // en
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        ConstantInt::get(Type::getInt1Ty(*m_context), false), // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)  // vm
    };

    // src0 ~ src3
    for (unsigned i = 0; i < 4; ++i) {
      auto compValue =
          ExtractElementInst::Create(output, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
      args[2 + i] = compValue;
    }

    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    break;
  }
  case BuiltInPointSize: {
    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0x1),              // en
        output,                                                           // src0
        poison,                                                           // src1
        poison,                                                           // src2
        poison,                                                           // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
    };
    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    break;
  }
  case BuiltInLayer: {
    assert(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_Layer are packed

    Value *layer = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);

    // NOTE: Only export gl_Layer when multi-view is disabled. Otherwise, we will export gl_ViewIndex to vertex position
    // data.
    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
    if (!enableMultiView) {
      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0x4),              // en
          poison,                                                           // src0
          poison,                                                           // src1
          layer,                                                            // src2
          poison,                                                           // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
          ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
      };
      emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    }

    // NOTE: We have to export gl_Layer via generic outputs as well.
    bool hasLayerExport = true;
    if (nextStage == ShaderStageFragment) {
      const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
      hasLayerExport = nextBuiltInUsage.layer;
    } else if (nextStage == ShaderStageInvalid) {
      hasLayerExport = false;
    }

    if (hasLayerExport) {
      assert(builtInOutLocs.find(BuiltInLayer) != builtInOutLocs.end());
      const unsigned loc = builtInOutLocs.find(BuiltInLayer)->second;

      recordVertexAttribExport(loc, {layer, poison, poison, poison});
    }

    break;
  }
  case BuiltInViewportIndex: {
    assert(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_Layer are packed
    Value *viewportIndex = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);

    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0x8),              // en
        poison,                                                           // src0
        poison,                                                           // src1
        poison,                                                           // src2
        viewportIndex,                                                    // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
    };
    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

    // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
    bool hasViewportIndexExport = true;
    if (nextStage == ShaderStageFragment) {
      const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
      hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
    } else if (nextStage == ShaderStageInvalid) {
      hasViewportIndexExport = false;
    }

    if (hasViewportIndexExport) {
      assert(builtInOutLocs.find(BuiltInViewportIndex) != builtInOutLocs.end());
      const unsigned loc = builtInOutLocs.find(BuiltInViewportIndex)->second;

      recordVertexAttribExport(loc, {viewportIndex, poison, poison, poison});
    }

    break;
  }
  case BuiltInViewIndex: {
    assert(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_ViewIndex are packed

    Value *viewIndex = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);

    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0x4),              // en
        poison,                                                           // src0
        poison,                                                           // src1
        viewIndex,                                                        // src2
        poison,                                                           // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
    };
    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

    break;
  }
  case BuiltInPrimitiveShadingRate: {
    // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
    assert(m_gfxIp >= GfxIpVersion({10, 3}));

    exportShadingRate(output, insertPos);
    break;
  }
  case BuiltInEdgeFlag: {
    Value *edgeflag = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);

    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0x2),              // en
        PoisonValue::get(Type::getFloatTy(*m_context)),                   // src1
        edgeflag,                                                         // src0
        PoisonValue::get(Type::getFloatTy(*m_context)),                   // src2
        PoisonValue::get(Type::getFloatTy(*m_context)),                   // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
    };
    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
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
Value *PatchInOutImportExport::adjustCentroidIj(Value *centroidIj, Value *centerIj, BuilderBase &builder) {
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
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
Value *PatchInOutImportExport::getSubgroupLocalInvocationId(BuilderBase &builder) {
  Value *subgroupLocalInvocationId =
      builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});

  unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  if (waveSize == 64) {
    subgroupLocalInvocationId =
        builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), subgroupLocalInvocationId});
  }

  return subgroupLocalInvocationId;
}

// =====================================================================================================================
// Do automatic workgroup size reconfiguration in a compute shader, to allow ReconfigWorkgroupLayout
// to apply optimizations.
SwizzleWorkgroupLayout PatchInOutImportExport::calculateWorkgroupLayout() {
  auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  SwizzleWorkgroupLayout resultLayout = {WorkgroupLayout::Unknown, WorkgroupLayout::Unknown};

  if (m_shaderStage == ShaderStageCompute) {
    auto &resUsage = *m_pipelineState->getShaderResourceUsage(ShaderStageCompute);
    if (resUsage.builtInUsage.cs.foldWorkgroupXY) {
      llvm_unreachable("Should never be called!");
    }

    if (mode.derivatives == DerivativeMode::Quads) {
      resultLayout.microLayout = WorkgroupLayout::Quads;
    } else if (mode.derivatives == DerivativeMode::Linear) {
      resultLayout.microLayout = WorkgroupLayout::Linear;
    }

    if (m_pipelineState->getOptions().forceCsThreadIdSwizzling) {
      if ((mode.workgroupSizeX >= 16) && (mode.workgroupSizeX % 8 == 0) && (mode.workgroupSizeY % 4 == 0)) {
        resultLayout.macroLayout = WorkgroupLayout::SexagintiQuads;
      }
    }

    // If no configuration has been specified, apply a reconfigure if the compute shader uses images and the
    // pipeline option was enabled.
    if (m_pipelineState->getOptions().reconfigWorkgroupLayout) {
      if ((mode.workgroupSizeX % 2) == 0 && (mode.workgroupSizeY % 2) == 0) {
        if (mode.workgroupSizeX % 8 == 0) {
          // It can be reconfigured into 8 X N
          if (resultLayout.macroLayout == WorkgroupLayout::Unknown) {
            resultLayout.macroLayout = WorkgroupLayout::SexagintiQuads;
          }
        } else {
          // If our local size in the X & Y dimensions are multiples of 2, we can reconfigure.
          if (resultLayout.microLayout == WorkgroupLayout::Unknown) {
            resultLayout.microLayout = WorkgroupLayout::Quads;
          }
        }
      }
    }
  }
  return resultLayout;
}

// =====================================================================================================================
// Reconfigure the workgroup for optimization purposes.
// @param localInvocationId : This is a v3i32 shader input (three VGPRs set up in hardware).
// @param macroLayout : Swizzle the thread id into macroLayout from macro level
// @param microLayout : Swizzle the thread id into microLayout from micro level
// @param workgroupSizeX : WorkgroupSize X for thread Id numbers
// @param workgroupSizeY : WorkgroupSize Y for thread Id numbers
// @param workgroupSizeZ : WorkgroupSize Z for thread Id numbers
// @param isHwLocalInvocationId : identify whether the localInvocationId is builtInLocalInvcocationId or
// BuiltInUnswizzledLocalInvocationId
// @param insertPos : Where to insert instructions.
Value *PatchInOutImportExport::reconfigWorkgroupLayout(Value *localInvocationId, WorkgroupLayout macroLayout,
                                                       WorkgroupLayout microLayout, unsigned workgroupSizeX,
                                                       unsigned workgroupSizeY, unsigned workgroupSizeZ,
                                                       bool isHwLocalInvocationId, llvm::Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *apiX = builder.getInt32(0);
  Value *apiY = builder.getInt32(0);
  Value *newLocalInvocationId = PoisonValue::get(localInvocationId->getType());
  unsigned bitsX = 0;
  unsigned bitsY = 0;
  auto &resUsage = *m_pipelineState->getShaderResourceUsage(ShaderStageCompute);
  resUsage.builtInUsage.cs.foldWorkgroupXY = true;

  Value *tidXY = builder.CreateExtractElement(localInvocationId, builder.getInt32(0), "tidXY");
  Value *apiZ = builder.getInt32(0);
  if (workgroupSizeZ > 1) {
    apiZ = builder.CreateExtractElement(localInvocationId, builder.getInt32(1), "tidZ");
  }
  // For BuiltInUnswizzledLocalInvocationId, it shouldn't swizzle and return the localInvocation<apiX,apiY,apiZ> without
  // foldXY.
  if (isHwLocalInvocationId) {
    apiX = builder.CreateURem(tidXY, builder.getInt32(workgroupSizeX));
    apiY = builder.CreateUDiv(tidXY, builder.getInt32(workgroupSizeX));
  } else {
    // Micro-tiling with quad:2x2, the thread-id will be marked as {<0,0>,<1,0>,<0,1>,<1,1>}
    // for each quad. Each 4 threads will be wrapped in the same tid.
    if (microLayout == WorkgroupLayout::Quads) {
      apiX = builder.CreateAnd(tidXY, builder.getInt32(1));
      apiY = builder.CreateAnd(builder.CreateLShr(tidXY, builder.getInt32(1)), builder.getInt32(1));
      tidXY = builder.CreateLShr(tidXY, builder.getInt32(2));
      bitsX = 1;
      bitsY = 1;
    }

    // Macro-tiling with 8xN block
    if (macroLayout == WorkgroupLayout::SexagintiQuads) {
      unsigned bits = 3 - bitsX;
      Value *subTileApiX = builder.CreateAnd(tidXY, builder.getInt32((1 << bits) - 1));
      subTileApiX = builder.CreateShl(subTileApiX, builder.getInt32(bitsX));
      apiX = builder.CreateOr(apiX, subTileApiX);

      // 1. Folding 4 threads as one tid if micro-tiling with quad before.
      //    After the folding, each 4 hwThreadIdX share the same tid after tid>>=bits.
      //    For example: hwThreadId.X = 0~3, the tid will be 0; <apiX,apiY> will be {<0,0>,<1,0>,<0,1>,<1,1>}
      //                 hwThreadId.X = 4~7, the tid will be 1; <apiX,apiY> will be {<0,0>,<1,0>,<0,1>,<1,1>}
      // 2. Folding 8 threads as one tid without any micro-tiling before.
      //    After the folding, each 8 hwThreadIdX share the same tid after tid>>=bits and only apiX are calculated.
      //    For example: hwThreadId.X = 0~7, tid = hwThreadId.X/8 = 0; <apiX> will be {0,1,...,7}
      //                 hwThreadId.X = 8~15, tid = hwThreadId.X/8 = 1; <apiX> will be {0,1,...,7}
      tidXY = builder.CreateLShr(tidXY, builder.getInt32(bits));
      bitsX = 3;

      // 1. Unfolding 4 threads, it needs to set walkY = workgroupSizeY/2 as these threads are wrapped in 2X2 size.
      // 2. Unfolding 8 threads, it needs to set walkY = workgroupSizeY/2 as these threads are wrapped in 1x8 size.
      // After unfolding these threads, it needs '| apiX and | apiY' to calculated each thread's coordinate
      // in the unfolded wrap threads.
      unsigned walkY = workgroupSizeY >> bitsY;
      Value *tileApiY = builder.CreateShl(builder.CreateURem(tidXY, builder.getInt32(walkY)), builder.getInt32(bitsY));
      apiY = builder.CreateOr(apiY, tileApiY);
      Value *tileApiX = builder.CreateShl(builder.CreateUDiv(tidXY, builder.getInt32(walkY)), builder.getInt32(bitsX));
      apiX = builder.CreateOr(apiX, tileApiX);
    } else {
      // Update the coordinates for each 4 wrap-threads then unfold each thread to calculate the coordinate by '| apiX
      // and | apiY'
      unsigned walkX = workgroupSizeX >> bitsX;
      Value *tileApiX = builder.CreateShl(builder.CreateURem(tidXY, builder.getInt32(walkX)), builder.getInt32(bitsX));
      apiX = builder.CreateOr(apiX, tileApiX);
      Value *tileApiY = builder.CreateShl(builder.CreateUDiv(tidXY, builder.getInt32(walkX)), builder.getInt32(bitsY));
      apiY = builder.CreateOr(apiY, tileApiY);
    }
  }

  newLocalInvocationId = builder.CreateInsertElement(newLocalInvocationId, apiX, uint64_t(0));
  newLocalInvocationId = builder.CreateInsertElement(newLocalInvocationId, apiY, uint64_t(1));
  newLocalInvocationId = builder.CreateInsertElement(newLocalInvocationId, apiZ, uint64_t(2));
  return newLocalInvocationId;
}

// =====================================================================================================================
// Creates the LGC intrinsic "lgc.swizzle.thread.group" to swizzle thread group for optimization purposes.
//
void PatchInOutImportExport::createSwizzleThreadGroupFunction() {

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

  Type *ivec3Ty = FixedVectorType::get(Type::getInt32Ty(*m_context), 3);

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

  static constexpr unsigned tileDims[] = {InvalidValue, 4, 8, 16};
  static constexpr unsigned tileBits[] = {InvalidValue, 2, 3, 4};
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
          auto result = builder.CreateLShr(src, ConstantInt::get(Type::getInt32Ty(*m_context), shift));
          result = builder.CreateOr(result, src);
          result = builder.CreateAnd(result, ConstantInt::get(Type::getInt32Ty(*m_context), mask));
          return result;
        };

        // x &= 0x55555555;                   // x = -f-e -d-c -b-a -9-8 -7-6 -5-4 -3-2 -1-0
        auto result = builder.CreateAnd(src, ConstantInt::get(Type::getInt32Ty(*m_context), 0x55555555));

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
// @param insertPos : Where to insert instructions.
void PatchInOutImportExport::exportShadingRate(Value *shadingRate, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

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
  // "Done" flag is valid for exporting position 0 ~ 3
  builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(),
                          {builder.getInt32(EXP_TARGET_POS_1), // tgt
                           builder.getInt32(0x2),              // en
                           poison,                             // src0
                           hwShadingRate,                      // src1
                           poison,                             // src2
                           poison,                             // src3
                           builder.getFalse(),                 // done
                           builder.getFalse()});               // src0
}

// =====================================================================================================================
// Gets HW shading rate and converts them to LGC definitions.
//
// @param insertPos : Where to insert instructions.
Value *PatchInOutImportExport::getShadingRate(Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  assert(m_shaderStage == ShaderStageFragment);
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

  // Y rate = Ancillary[5:4], X rate = Ancillary[3:2]
  Value *xRate = builder.CreateAnd(ancillary, 0xC);
  xRate = builder.CreateLShr(xRate, 2);
  Value *yRate = builder.CreateAnd(ancillary, 0x30);
  yRate = builder.CreateLShr(yRate, 4);

  if (m_gfxIp.major >= 11) {
    // NOTE: In GFX11, the graphics pipeline is to support VRS rates till 4x4 which includes 2x4 and 4x2
    // along with the legacy rates.
    //
    // xRate = xRate == 0x1 ? Horizontal2Pixels : (xRate == 0x2 ? Horizontal4Pixels : None)
    auto xRate2Pixels = builder.CreateICmpEQ(xRate, builder.getInt32(1));
    auto xRate4Pixels = builder.CreateICmpEQ(xRate, builder.getInt32(2));
    xRate = builder.CreateSelect(xRate2Pixels, builder.getInt32(ShadingRateHorizontal2Pixels),
                                 builder.CreateSelect(xRate4Pixels, builder.getInt32(ShadingRateHorizontal4Pixels),
                                                      builder.getInt32(ShadingRateNone)));

    // yRate = yRate == 0x1 ? Vertical2Pixels : (yRate == 0x2 ? Vertical2Pixels : None)
    auto yRate2Pixels = builder.CreateICmpEQ(yRate, builder.getInt32(1));
    auto yRate4Pixels = builder.CreateICmpEQ(yRate, builder.getInt32(2));
    yRate = builder.CreateSelect(yRate2Pixels, builder.getInt32(ShadingRateVertical2Pixels),
                                 builder.CreateSelect(yRate4Pixels, builder.getInt32(ShadingRateVertical4Pixels),
                                                      builder.getInt32(ShadingRateNone)));
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
  }

  return builder.CreateOr(xRate, yRate);
}

// =====================================================================================================================
// Records export info of vertex attributes
//
// @param location : Vertex attribute location
// @param attribValues : Values of this vertex attribute to export
void PatchInOutImportExport::recordVertexAttribExport(unsigned location, ArrayRef<Value *> attribValues) {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader); // Valid shader stages
  assert(location <= MaxInOutLocCount);           // 32 attributes at most
  assert(attribValues.size() == 4);               // Must have 4 elements, corresponds to <4 x float>

  auto poison = PoisonValue::get(Type::getFloatTy(*m_context));

  // Vertex attribute not existing, insert a new one and initialize it
  if (m_attribExports.count(location) == 0) {
    for (unsigned i = 0; i < 4; ++i)
      m_attribExports[location][i] = poison;
  }

  for (unsigned i = 0; i < 4; ++i) {
    assert(attribValues[i]);
    if (isa<UndefValue>(attribValues[i]) || isa<PoisonValue>(attribValues[i]))
      continue; // Here, we only record new attribute values that are valid (not unspecified ones)

    // NOTE: The existing values must have been initialized to unspecified ones already. Overlapping is disallowed (see
    // such cases):
    //   - Valid:
    //       Existing: attrib0, <1.0, 2.0, undef/poison, undef/poison>
    //       New:      attrib0, <undef/poison, undef/poison, 3.0, 4.0>
    //   - Invalid:
    //       Existing: attrib0, <1.0, 2.0, 3.0, undef/poison>
    //       New:      attrib0, <undef/poison, undef/poison, 4.0, 5.0>
    assert(isa<UndefValue>(m_attribExports[location][i]) || isa<PoisonValue>(m_attribExports[location][i]));
    m_attribExports[location][i] = attribValues[i]; // Update values that are valid
  }

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  inOutUsage.expCount = std::max(inOutUsage.expCount, location + 1); // Update export count
}

// =====================================================================================================================
// Exports vertex attributes that were recorded previously
//
// @param insertPos : Where to insert instructions.
void PatchInOutImportExport::exportVertexAttribs(Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader); // Valid shader stages
  if (m_attribExports.empty()) {
    assert(m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.expCount == 0);
    return;
  }

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  for (auto &attribExport : m_attribExports) {
    if (m_gfxIp.major <= 10) {
      unsigned channelMask = 0;
      for (unsigned i = 0; i < 4; ++i) {
        assert(attribExport.second[i]);
        if (!isa<UndefValue>(attribExport.second[i]) && !isa<PoisonValue>(attribExport.second[i]))
          channelMask |= (1u << i); // Update channel mask if the value is valid (not unspecified)
      }

      builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(),
                              {builder.getInt32(EXP_TARGET_PARAM_0 + attribExport.first), // tgt
                               builder.getInt32(channelMask),                             // en
                               attribExport.second[0],                                    // src0
                               attribExport.second[1],                                    // src1
                               attribExport.second[2],                                    // src2
                               attribExport.second[3],                                    // src3
                               builder.getFalse(),                                        // done
                               builder.getFalse()});                                      // src0
    } else {
      Value *attribValue = PoisonValue::get(FixedVectorType::get(builder.getFloatTy(), 4)); // Always be <4 x float>
      for (unsigned i = 0; i < 4; ++i)
        attribValue = builder.CreateInsertElement(attribValue, attribExport.second[i], i);
      // NOTE: For GFX11+, vertex attributes are exported through memory. This call will be expanded when NGG primitive
      // shader is generated. The arguments are: buffer descriptor of attribute ring, attribute location, and attribute
      // export value.
      emitCall(lgcName::NggAttribExport, builder.getVoidTy(),
               {m_pipelineSysValues.get(m_entryPoint)->getAttribRingBufDesc(), builder.getInt32(attribExport.first),
                attribValue},
               {}, insertPos);
    }
  }
}

} // namespace lgc
