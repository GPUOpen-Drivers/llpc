/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-patch-in-out-import-export"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char LegacyPatchInOutImportExport::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for input import and output export
ModulePass *createLegacyPatchInOutImportExport() {
  return new LegacyPatchInOutImportExport();
}

// =====================================================================================================================
PatchInOutImportExport::PatchInOutImportExport() : m_lds(nullptr) {
  memset(&m_gfxIp, 0, sizeof(m_gfxIp));
  initPerShader();
}

// =====================================================================================================================
LegacyPatchInOutImportExport::LegacyPatchInOutImportExport() : ModulePass(ID) {
}

// =====================================================================================================================
LegacyPatchInOutImportExport::~LegacyPatchInOutImportExport() {
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
  m_threadId = nullptr;

  m_attribExports.clear();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchInOutImportExport::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  PipelineShadersResult &pipelineShaders = getAnalysis<LegacyPipelineShaders>().getResult();
  auto getPDT = [&](Function &f) -> PostDominatorTree & {
    return getAnalysis<PostDominatorTreeWrapperPass>(f).getPostDomTree();
  };
  return m_impl.runImpl(module, pipelineShaders, pipelineState, getPDT);
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

  switch (m_gfxIp.major) {
  default:
    m_buffFormats = &BufferFormatsGfx9;
    break;
  case 10:
    m_buffFormats = &BufferFormatsGfx10;
    break;
  }

  // Process each shader in turn, in reverse order (because for example VS uses inOutUsage.tcs.calcFactor
  // set by TCS).
  for (int shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage) {
    auto entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (entryPoint) {
      processFunction(*entryPoint, static_cast<ShaderStage>(shaderStage), inputCallees, otherCallees,
                      getPostDominatorTree);
      if (shaderStage == ShaderStageTessControl)
        storeTessFactors();
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
  const bool enableXfb = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.enableXfb;
  useThreadId = useThreadId || enableXfb;

  if (useThreadId) {
    // Calculate and store thread ID
    BuilderBase builder(*m_context);
    builder.setInsertPointPastAllocas(*m_entryPoint);
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
      //          +-------------+-------------+
      // Off-chip | Input Patch | Tess Factor | (LDS)
      //          +-------------+-------------+
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

      const unsigned inVertexCount = m_pipelineState->getInputAssemblyState().patchControlPoints;
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
      LLPC_OUTS("\n");
    }
  }

  if (m_shaderStage == ShaderStageCompute) {
    // In a compute shader, process lgc.reconfigure.local.invocation.id calls.
    // This does not particularly have to be done here; it could be done anywhere after BuilderImpl.
    for (Function &func : *m_module) {
      if (func.isDeclaration() && func.getName().startswith(lgcName::ReconfigureLocalInvocationId)) {
        WorkgroupLayout workgroupLayout = calculateWorkgroupLayout();
        while (!func.use_empty()) {
          CallInst *reconfigCall = cast<CallInst>(*func.user_begin());
          Value *localInvocationId = reconfigCall->getArgOperand(0);

          // If we do not need to configure our workgroup in linear layout and the layout info is not specified, we
          // do the reconfiguration for this workgroup.
          if (workgroupLayout != WorkgroupLayout::Unknown && workgroupLayout != WorkgroupLayout::Linear)
            localInvocationId = reconfigWorkgroup(localInvocationId, reconfigCall);
          reconfigCall->replaceAllUsesWith(localInvocationId);
          reconfigCall->eraseFromParent();
        }
      }

      if (func.isDeclaration() && func.getName().startswith(lgcName::SwizzleWorkgroupId)) {
        createSwizzleThreadGroupFunction();
      }
      // Different with above, this will force the threadID swizzle which will rearrange thread ID within a group into
      // blocks of 8*4, not to reconfig workgroup automatically and will support to be swizzled in 8*4 block
      // split.
      if (func.isDeclaration() && func.getName().startswith(lgcName::SwizzleLocalInvocationId)) {
        while (!func.use_empty()) {
          CallInst *swizzleCall = cast<CallInst>(*func.user_begin());
          Value *localInvocationId = swizzleCall->getArgOperand(0);

          localInvocationId = swizzleLocalInvocationIdIn8x4(localInvocationId, swizzleCall);
          swizzleCall->replaceAllUsesWith(localInvocationId);
          swizzleCall->eraseFromParent();
        }
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

  IRBuilder<> builder(*m_context);
  auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  auto mangledName = callee->getName();

  auto importGenericInput = lgcName::InputImportGeneric;
  auto importBuiltInInput = lgcName::InputImportBuiltIn;
  auto importInterpolantInput = lgcName::InputImportInterpolant;
  auto importGenericOutput = lgcName::OutputImportGeneric;
  auto importBuiltInOutput = lgcName::OutputImportBuiltIn;

  const bool isGenericInputImport = mangledName.startswith(importGenericInput);
  const bool isBuiltInInputImport = mangledName.startswith(importBuiltInInput);
  const bool isInterpolantInputImport = mangledName.startswith(importInterpolantInput);
  const bool isGenericOutputImport = mangledName.startswith(importGenericOutput);
  const bool isBuiltInOutputImport = mangledName.startswith(importBuiltInOutput);

  const bool isImport = (isGenericInputImport || isBuiltInInputImport || isInterpolantInputImport ||
                         isGenericOutputImport || isBuiltInOutputImport);

  auto exportGenericOutput = lgcName::OutputExportGeneric;
  auto exportBuiltInOutput = lgcName::OutputExportBuiltIn;
  auto exportXfbOutput = lgcName::OutputExportXfb;

  const bool isGenericOutputExport = mangledName.startswith(exportGenericOutput);
  const bool isBuiltInOutputExport = mangledName.startswith(exportBuiltInOutput);
  const bool isXfbOutputExport = mangledName.startswith(exportXfbOutput);

  const bool isExport = (isGenericOutputExport || isBuiltInOutputExport || isXfbOutputExport);

  const bool isInput = (isGenericInputImport || isBuiltInInputImport || isInterpolantInputImport);
  const bool isOutput = (isGenericOutputImport || isBuiltInOutputImport || isGenericOutputExport ||
                         isBuiltInOutputExport || isXfbOutputExport);

  if (isImport && isInput) {
    // Input imports
    Value *input = nullptr;
    Type *inputTy = callInst.getType();

    // Generic value (location or SPIR-V built-in ID)
    unsigned value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

    LLVM_DEBUG(dbgs() << "Find input import call: builtin = " << isBuiltInInputImport << " value = " << value << "\n");

    m_importCalls.push_back(&callInst);

    if (isBuiltInInputImport) {
      const unsigned builtInId = value;

      switch (m_shaderStage) {
      case ShaderStageVertex: {
        input = patchVsBuiltInInputImport(inputTy, builtInId, &callInst);
        break;
      }
      case ShaderStageTessControl: {
        // Builtin Call has different number of operands
        Value *elemIdx = nullptr;
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.arg_size() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

        input = patchTcsBuiltInInputImport(inputTy, builtInId, elemIdx, vertexIdx, &callInst);
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
        input = patchTesBuiltInInputImport(inputTy, builtInId, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        // Builtin Call has different number of operands
        Value *vertexIdx = nullptr;
        if (callInst.arg_size() > 1)
          vertexIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        input = patchGsBuiltInInputImport(inputTy, builtInId, vertexIdx, &callInst);
        break;
      }
      case ShaderStageMesh: {
        assert(callInst.arg_size() == 2);
        Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
        input = patchMeshBuiltInInputImport(inputTy, builtInId, elemIdx, &callInst);
        break;
      }
      case ShaderStageFragment: {
        Value *generalVal = nullptr;
        if (callInst.arg_size() >= 2)
          generalVal = callInst.getArgOperand(1);
        input = patchFsBuiltInInputImport(inputTy, builtInId, generalVal, &callInst);
        break;
      }
      default: {
        llvm_unreachable("Should never be called!");
        break;
      }
      }
    } else {
      assert(isGenericInputImport || isInterpolantInputImport);

      unsigned loc = InvalidValue;
      Value *locOffset = nullptr;
      Value *elemIdx = nullptr;
      bool highHalf = false;

      if (m_shaderStage == ShaderStageVertex) {
        // NOTE: For vertex shader, generic inputs are not mapped.
        loc = value;
      } else {
        if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval ||
            (m_shaderStage == ShaderStageFragment && isInterpolantInputImport)) {
          // NOTE: If location offset is present and is a constant, we have to add it to the unmapped
          // location before querying the mapped location. Meanwhile, we have to adjust the location
          // offset to 0 (rebase it).
          locOffset = callInst.getOperand(1);
          if (isa<ConstantInt>(locOffset)) {
            value += cast<ConstantInt>(locOffset)->getZExtValue();
            locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
          }
        }

        InOutLocationInfo origLocInfo;
        origLocInfo.setLocation(value);
        auto locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
        if (m_shaderStage == ShaderStageTessEval ||
            (m_shaderStage == ShaderStageFragment &&
             m_pipelineState->getPrevShaderStage(m_shaderStage) == ShaderStageMesh)) {
          // NOTE: For generic inputs of tessellation evaluation shader or fragment shader whose previous shader stage
          // is mesh shader, they could be per-patch ones or per-primitive ones.
          if (locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end()) {
            loc = locInfoMapIt->second.getLocation();
          } else if (resUsage->inOutUsage.perPatchInputLocMap.find(value) !=
                     resUsage->inOutUsage.perPatchInputLocMap.end()) {
            loc = resUsage->inOutUsage.perPatchInputLocMap[value];
          } else {
            assert(resUsage->inOutUsage.perPrimitiveInputLocMap.find(value) !=
                   resUsage->inOutUsage.perPrimitiveInputLocMap.end());
            loc = resUsage->inOutUsage.perPrimitiveInputLocMap[value];
          }
        } else {
          if (m_pipelineState->canPackInput(m_shaderStage)) {
            // The inputLocInfoMap of {TCS, GS, FS} maps original InOutLocationInfo to tightly compact InOutLocationInfo
            const bool isTcs = m_shaderStage == ShaderStageTessControl;
            const uint32_t elemIdxArgIdx = (isInterpolantInputImport || isTcs) ? 2 : 1;
            bool hasDynIndex = false;
            if (isTcs) {
              hasDynIndex = !isa<ConstantInt>(callInst.getOperand(1)) || !isa<ConstantInt>(callInst.getOperand(2));
              if (!hasDynIndex) {
                // TCS input calls at the same location may have dynamic indexing or not
                // Try the key as combination of location and component at first
                origLocInfo.setComponent(cast<ConstantInt>(callInst.getOperand(elemIdxArgIdx))->getZExtValue());
                locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
                if (locInfoMapIt == resUsage->inOutUsage.inputLocInfoMap.end()) {
                  // Try the key as the plain location
                  origLocInfo.setComponent(0);
                  locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
                  hasDynIndex = true;
                }
              } else {
                locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
              }
            } else {
              origLocInfo.setComponent(cast<ConstantInt>(callInst.getOperand(elemIdxArgIdx))->getZExtValue());
              locInfoMapIt = resUsage->inOutUsage.inputLocInfoMap.find(origLocInfo);
            }
            assert(locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end());

            loc = locInfoMapIt->second.getLocation();
            if (!hasDynIndex)
              elemIdx = builder.getInt32(locInfoMapIt->second.getComponent());
            highHalf = locInfoMapIt->second.isHighHalf();
          } else {
            assert(locInfoMapIt != resUsage->inOutUsage.inputLocInfoMap.end());
            loc = locInfoMapIt->second.getLocation();
          }
        }
      }
      assert(loc != InvalidValue);

      switch (m_shaderStage) {
      case ShaderStageTessControl: {
        assert(callInst.arg_size() == 4);

        if (!elemIdx) {
          elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);
        }

        auto vertexIdx = callInst.getOperand(3);
        assert(isDontCareValue(vertexIdx) == false);

        input = patchTcsGenericInputImport(inputTy, loc, locOffset, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageTessEval: {
        assert(callInst.arg_size() == 4);

        auto elemIdx = callInst.getOperand(2);
        assert(isDontCareValue(elemIdx) == false);

        auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

        input = patchTesGenericInputImport(inputTy, loc, locOffset, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        assert(callInst.arg_size() == 3);
        if (!elemIdx)
          elemIdx = cast<ConstantInt>(callInst.getOperand(1));

        const unsigned compIdx = cast<ConstantInt>(elemIdx)->getZExtValue();
        assert(isDontCareValue(elemIdx) == false);

        Value *vertexIdx = callInst.getOperand(2);
        assert(isDontCareValue(vertexIdx) == false);

        input = patchGsGenericInputImport(inputTy, loc, compIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageFragment: {
        unsigned interpMode = InOutInfo::InterpModeSmooth;
        unsigned interpLoc = InOutInfo::InterpLocCenter;
        bool isPerPrimitive = false;

        if (!elemIdx)
          elemIdx = callInst.getOperand(isInterpolantInputImport ? 2 : 1);
        assert(isDontCareValue(elemIdx) == false);

        Value *auxInterpValue = nullptr;

        if (isGenericInputImport) {
          assert(callInst.arg_size() == 5);

          isPerPrimitive = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue() != 0;
          interpMode = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
          interpLoc = cast<ConstantInt>(callInst.getOperand(4))->getZExtValue();
        } else {
          assert(isInterpolantInputImport);
          assert(callInst.arg_size() == 5);

          interpMode = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
          interpLoc = InOutInfo::InterpLocUnknown;

          auxInterpValue = callInst.getOperand(4);
        }

        input = patchFsGenericInputImport(inputTy, loc, locOffset, elemIdx, isPerPrimitive, auxInterpValue, interpMode,
                                          interpLoc, highHalf, &callInst);
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

    // Generic value (location or SPIR-V built-in ID)
    unsigned value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

    LLVM_DEBUG(dbgs() << "Find output import call: builtin = " << isBuiltInOutputImport << " value = " << value
                      << "\n");

    m_importCalls.push_back(&callInst);

    if (isBuiltInOutputImport) {
      const unsigned builtInId = value;

      assert(callInst.arg_size() == 3);
      Value *elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);
      Value *vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

      output = patchTcsBuiltInOutputImport(outputTy, builtInId, elemIdx, vertexIdx, &callInst);
    } else {
      assert(isGenericOutputImport);

      unsigned loc = InvalidValue;

      // NOTE: If location offset is a constant, we have to add it to the unmapped location before querying
      // the mapped location. Meanwhile, we have to adjust the location offset to 0 (rebase it).
      Value *locOffset = callInst.getOperand(1);
      if (isa<ConstantInt>(locOffset)) {
        value += cast<ConstantInt>(locOffset)->getZExtValue();
        locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
      }

      // NOTE: For generic outputs of tessellation control shader, they could be per-patch ones.
      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(value);
      auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
      if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
        loc = locInfoMapIt->second.getLocation();
      } else {
        assert(resUsage->inOutUsage.perPatchOutputLocMap.find(value) !=
               resUsage->inOutUsage.perPatchOutputLocMap.end());
        loc = resUsage->inOutUsage.perPatchOutputLocMap[value];
      }
      assert(loc != InvalidValue);

      assert(callInst.arg_size() == 4);
      auto elemIdx = callInst.getOperand(2);
      assert(isDontCareValue(elemIdx) == false);
      auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

      output = patchTcsGenericOutputImport(outputTy, loc, locOffset, elemIdx, vertexIdx, &callInst);
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
          patchXfbOutputExport(output, xfbBuffer, xfbOffset, streamId, &callInst);
        break;
      }
      case ShaderStageTessEval: {
        // TS-only pipeline, TES is the last stage
        if (!m_hasGs)
          patchXfbOutputExport(output, xfbBuffer, xfbOffset, streamId, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        // Do nothing, transform feedback output is done in copy shader
        break;
      }
      case ShaderStageCopyShader: {
        // TS-GS or GS-only pipeline, copy shader is the last stage
        patchXfbOutputExport(output, xfbBuffer, xfbOffset, streamId, &callInst);
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
        patchGsBuiltInOutputExport(output, builtInId, resUsage->inOutUsage.gs.rasterStream, &callInst);
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
      auto locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);

      if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageMesh) {
        locOffset = callInst.getOperand(1);

        // NOTE: For generic outputs of tessellation control shader or mesh shader, they could be per-patch ones or
        // per-primitive ones.
        if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
          exist = true;
          loc = locInfoMapIt->second.getLocation();
        } else if (resUsage->inOutUsage.perPatchOutputLocMap.find(value) !=
                   resUsage->inOutUsage.perPatchOutputLocMap.end()) {
          exist = true;
          loc = resUsage->inOutUsage.perPatchOutputLocMap[value];
        } else if (resUsage->inOutUsage.perPrimitiveOutputLocMap.find(value) !=
                   resUsage->inOutUsage.perPrimitiveOutputLocMap.end()) {
          exist = true;
          loc = resUsage->inOutUsage.perPrimitiveOutputLocMap[value];
        }
      } else if (m_shaderStage == ShaderStageCopyShader) {
        exist = true;
        loc = value;
      } else {
        if (m_pipelineState->canPackOutput(m_shaderStage)) {
          assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageGeometry ||
                 m_shaderStage == ShaderStageTessEval);
          origLocInfo.setComponent(cast<ConstantInt>(callInst.getOperand(1))->getZExtValue());
          locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
          bool relateDynIndex = false;
          const bool checkDynIndex =
              (m_shaderStage == ShaderStageVertex && m_pipelineState->hasShaderStage(ShaderStageTessControl));
          if (checkDynIndex && locInfoMapIt == resUsage->inOutUsage.outputLocInfoMap.end()) {
            // The location in TCS may be used with dynamic indexing, try location as the key for a search
            origLocInfo.setComponent(0);
            locInfoMapIt = resUsage->inOutUsage.outputLocInfoMap.find(origLocInfo);
            relateDynIndex = true;
          }

          if (locInfoMapIt != resUsage->inOutUsage.outputLocInfoMap.end()) {
            loc = locInfoMapIt->second.getLocation();
            // Dynamic indexing related locations just use the location for mapping
            if (!relateDynIndex)
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
          patchVsGenericOutputExport(output, loc, elemIdx, &callInst);
          break;
        }
        case ShaderStageTessControl: {
          assert(callInst.arg_size() == 5);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

          patchTcsGenericOutputExport(output, loc, locOffset, elemIdx, vertexIdx, &callInst);
          break;
        }
        case ShaderStageTessEval: {
          assert(callInst.arg_size() == 3);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          patchTesGenericOutputExport(output, loc, elemIdx, &callInst);
          break;
        }
        case ShaderStageGeometry: {
          assert(callInst.arg_size() == 4);
          if (elemIdx == InvalidValue)
            elemIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          const unsigned streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
          patchGsGenericOutputExport(output, loc, elemIdx, streamId, &callInst);
          break;
        }
        case ShaderStageMesh: {
          assert(callInst.arg_size() == 6);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexOrPrimitiveIdx = callInst.getOperand(3);
          bool isPerPrimitive = cast<ConstantInt>(callInst.getOperand(4))->getZExtValue() != 0;
          patchMeshGenericOutputExport(output, loc, locOffset, elemIdx, vertexOrPrimitiveIdx, isPerPrimitive,
                                       &callInst);
          break;
        }
        case ShaderStageFragment: {
          assert(callInst.arg_size() == 3);
          llvm_unreachable("Fragment shader export should have been handled by the LowerFragColorExport pass");
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
      // NOTE: Implicitly store the value of gl_ViewIndex to GS-VS ring buffer before emit calls.
      if (m_pipelineState->getInputAssemblyState().enableMultiView) {
        assert(m_shaderStage == ShaderStageGeometry); // Must be geometry shader

        auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
        auto viewIndex = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);

        auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
        const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

        assert(builtInOutLocMap.find(BuiltInViewIndex) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap.find(BuiltInViewIndex)->second;

        auto rasterStream = resUsage->inOutUsage.gs.rasterStream;
        storeValueToGsVsRing(viewIndex, loc, 0, rasterStream, &callInst);
      }

      unsigned emitStream = InvalidValue;

      uint64_t message = cast<ConstantInt>(callInst.getArgOperand(0))->getZExtValue();
      if (message == GsEmitStreaM0 || message == GsEmitStreaM1 || message == GsEmitStreaM2 ||
          message == GsEmitStreaM3) {
        // NOTE: MSG[9:8] = STREAM_ID
        emitStream = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
      }

      if (emitStream != InvalidValue) {
        // Increment emit vertex counter
        auto emitCounterPair = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr();
        auto emitCounterTy = emitCounterPair.first;
        auto emitCounterPtr = emitCounterPair.second[emitStream];
        Value *emitCounter = new LoadInst(emitCounterTy, emitCounterPtr, "", &callInst);
        emitCounter =
            BinaryOperator::CreateAdd(emitCounter, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", &callInst);
        new StoreInst(emitCounter, emitCounterPtr, &callInst);
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
  auto undef = UndefValue::get(Type::getFloatTy(*m_context));

  Instruction *insertPos = &retInst;

  const bool enableXfb = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.enableXfb;
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
      auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);

      bool updated = false;
      for (auto &block : *m_entryPoint) {
        // Search blocks to find the switch-case instruction
        auto switchInst = dyn_cast<SwitchInst>(block.getTerminator());
        if (switchInst) {
          for (auto &caseBranch : switchInst->cases()) {
            if (caseBranch.getCaseValue()->getZExtValue() == resUsage->inOutUsage.gs.rasterStream) {
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
    unsigned clipDistanceCount = 0;
    unsigned cullDistanceCount = 0;

    auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;

    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

    if (m_shaderStage == ShaderStageVertex) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;

      if (enableMultiView) {
        // NOTE: If multi-view is enabled, the exported value of gl_Layer is from gl_ViewIndex.
        m_layer = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
      }
    } else if (m_shaderStage == ShaderStageTessEval) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
      auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;

      usePosition = builtInUsage.position;
      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;

      if (enableMultiView) {
        // NOTE: If multi-view is enabled, the exported value of gl_Layer is from gl_ViewIndex.
        m_layer = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
      }
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

    useLayer = enableMultiView || useLayer;

    const auto &builtInOutLocs =
        m_shaderStage == ShaderStageCopyShader ? inOutUsage.gs.builtInOutLocs : inOutUsage.builtInOutputLocMap;
    const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
    if (nextStage == ShaderStageFragment) {
      useLayer |= nextBuiltInUsage.layer || nextBuiltInUsage.viewIndex;
      if (useLayer && !m_layer) {
        // Multi-view is disabled and vertex process stages don't use gl_ViewIndex/gl_Layer,
        // but FS uses them, the value will be zero.
        m_layer = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
      }
    }

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
          clipCullDistance.push_back(undef);
      } else {
        while (clipCullDistance.size() < 8) // [8 x float]
          clipCullDistance.push_back(undef);
      }

      bool miscExport = usePointSize || useLayer || useViewportIndex;
      miscExport |= useShadingRate;
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
            clipCullDistance.push_back(undef);

          for (unsigned i = 0; i < cullDistanceCount; ++i)
            clipCullDistance.push_back(cullDistance[i]);

          // Do array padding
          if (clipCullDistance.size() <= 4) {
            while (clipCullDistance.size() < 4) // [4 x float]
              clipCullDistance.push_back(undef);
          } else {
            while (clipCullDistance.size() < 8) // [8 x float]
              clipCullDistance.push_back(undef);
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

        recordVertexAttribExport(loc, {primitiveId, undef, undef, undef});
      }
    }

    if (m_gfxIp.major <= 8 && useLayer) {
      assert(m_layer);
      addExportInstForBuiltInOutput(m_layer, BuiltInLayer, insertPos);
    }

    // Export gl_Layer and gl_ViewportIndex before entry-point returns
    if (m_gfxIp.major >= 9 && (useLayer || useViewportIndex)) {
      Value *viewportIndexAndLayer = ConstantInt::get(Type::getInt32Ty(*m_context), 0);

      if (useViewportIndex) {
        assert(m_viewportIndex);
        viewportIndexAndLayer = BinaryOperator::CreateShl(
            m_viewportIndex, ConstantInt::get(Type::getInt32Ty(*m_context), 16), "", insertPos);
      }

      if (useLayer) {
        assert(m_layer);
        viewportIndexAndLayer = BinaryOperator::CreateOr(viewportIndexAndLayer, m_layer, "", insertPos);
      }

      viewportIndexAndLayer = new BitCastInst(viewportIndexAndLayer, Type::getFloatTy(*m_context), "", insertPos);

      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0x4),              // en
          undef,                                                            // src0
          undef,                                                            // src1
          viewportIndexAndLayer,                                            // src2
          undef,                                                            // src3
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

          recordVertexAttribExport(loc, {viewportIndex, undef, undef, undef});
        }
      }

      // NOTE: We have to export gl_Layer via generic outputs as well.
      if (useLayer) {
        if (nextStage == ShaderStageFragment) {
          unsigned loc = InvalidValue;
          Value *layer = nullptr;

          if (nextBuiltInUsage.layer) {
            assert(builtInOutLocs.find(BuiltInLayer) != builtInOutLocs.end());
            loc = builtInOutLocs.find(BuiltInLayer)->second;

            layer = new BitCastInst(m_layer, Type::getFloatTy(*m_context), "", insertPos);
          }

          if (nextBuiltInUsage.viewIndex) {
            assert(builtInOutLocs.find(BuiltInViewIndex) != builtInOutLocs.end());
            loc = builtInOutLocs.find(BuiltInViewIndex)->second;

            if (enableMultiView)
              layer = new BitCastInst(m_layer, Type::getFloatTy(*m_context), "", insertPos);
            else
              layer = ConstantFP::get(Type::getFloatTy(*m_context), 0.0f);
          }
          if (layer)
            recordVertexAttribExport(loc, {layer, undef, undef, undef});
        }
      }
    }

    // NOTE: For GFX10+, dummy generic output is no longer needed. Field NO_PC_EXPORT of SPI_VS_OUT_CONFIG
    // will control the behavior.
    if (m_gfxIp.major <= 9) {
      // NOTE: If no generic outputs is present in this shader, we have to export a dummy one
      if (inOutUsage.expCount == 0)
        recordVertexAttribExport(0, {undef, undef, undef, undef});
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
  } else if (m_shaderStage == ShaderStageGeometry) {
    if (m_gfxIp.major >= 10) {
      // NOTE: Per programming guide, we should do a “s_waitcnt 0,0,0 + s_waitcnt_vscnt 0” before issuing a "done", so
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
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTcsGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                          Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(compIdx && vertexIdx);

  auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, location, locOffset, compIdx, vertexIdx, insertPos);
  return readValueFromLds(false, inputTy, ldsOffset, insertPos);
}

// =====================================================================================================================
// Patches import calls for generic inputs of tessellation evaluation shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTesGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                          Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(compIdx);

  auto ldsOffset = calcLdsOffsetForTesInput(inputTy, location, locOffset, compIdx, vertexIdx, insertPos);
  return readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, insertPos);
}

// =====================================================================================================================
// Patches import calls for generic inputs of geometry shader.
//
// @param inputTy : Type of input value
// @param location : Location of the input
// @param compIdx : Index used for vector element indexing
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchGsGenericInputImport(Type *inputTy, unsigned location, unsigned compIdx,
                                                         Value *vertexIdx, Instruction *insertPos) {
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

  Value *input = loadValueFromEsGsRing(inputTy, location, compIdx, vertexIdx, insertPos);

  if (inputTy != origInputTy) {
    // Cast back to original input type
    assert(canBitCast(inputTy, origInputTy));
    assert(inputTy->isVectorTy());

    input = new BitCastInst(input, origInputTy, "", insertPos);
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
  {
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
  {
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

  {
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
// @param auxInterpValue : Auxiliary value of interpolation: for non "custom" interpolation, it is the explicitly
// calculated I/J; for "custom" interpolation, it is vertex no. (could be null)
// @param interpMode : Interpolation mode
// @param interpLoc : Interpolation location
// @param highHalf : Whether it is a high half in a 16-bit attribute
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchFsGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                         Value *compIdx, bool isPerPrimitive, Value *auxInterpValue,
                                                         unsigned interpMode, unsigned interpLoc, bool highHalf,
                                                         Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

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
  if (interpMode != InOutInfo::InterpModeFlat && interpMode != InOutInfo::InterpModeCustom) {
    auto ij = auxInterpValue;
    if (!ij) {
      if (interpMode == InOutInfo::InterpModeSmooth) {
        if (interpLoc == InOutInfo::InterpLocCentroid) {
          ij = adjustCentroidIj(getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.centroid),
                                getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.center), insertPos);
        } else if (interpLoc == InOutInfo::InterpLocSample)
          ij = getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.sample);
        else {
          assert(interpLoc == InOutInfo::InterpLocCenter);
          ij = getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.center);
        }
      } else {
        assert(interpMode == InOutInfo::InterpModeNoPersp);
        if (interpLoc == InOutInfo::InterpLocCentroid) {
          ij = adjustCentroidIj(getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.centroid),
                                getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center), insertPos);
        } else if (interpLoc == InOutInfo::InterpLocSample)
          ij = getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.sample);
        else {
          assert(interpLoc == InOutInfo::InterpLocCenter);
          ij = getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center);
        }
      }
    }
    coordI = ExtractElementInst::Create(ij, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);
    coordJ = ExtractElementInst::Create(ij, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
  }

  Type *basicTy = inputTy->isVectorTy() ? cast<VectorType>(inputTy)->getElementType() : inputTy;

  const unsigned compCout = inputTy->isVectorTy() ? cast<FixedVectorType>(inputTy)->getNumElements() : 1;
  const unsigned bitWidth = inputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  const unsigned numChannels = (bitWidth == 64 ? 2 : 1) * compCout;

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
  Value *interp = UndefValue::get(interpTy);

  unsigned startChannel = 0;
  if (compIdx)
    startChannel = cast<ConstantInt>(compIdx)->getZExtValue();

  Value *loc = nullptr;
  if (locOffset) {
    loc = ConstantInt::get(Type::getInt32Ty(*m_context), location + cast<ConstantInt>(locOffset)->getZExtValue());
    assert((startChannel + numChannels) <= 4);
  } else {
    loc = ConstantInt::get(Type::getInt32Ty(*m_context), location);
  }

  for (unsigned i = startChannel; i < startChannel + numChannels; ++i) {
    Value *compValue = nullptr;

    if (interpMode != InOutInfo::InterpModeFlat && interpMode != InOutInfo::InterpModeCustom) {
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
        assert(isa<ConstantInt>(auxInterpValue));
        unsigned vertexNo = cast<ConstantInt>(auxInterpValue)->getZExtValue();

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

      Value *attr = locOffset ? loc : builder.getInt32(location + i / 4);
      compValue =
          performFsParameterLoad(builder, attr, builder.getInt32(i % 4), interpParam, primMask, bitWidth, highHalf);
    }

    if (numChannels == 1)
      interp = compValue;
    else {
      interp = InsertElementInst::Create(
          interp, compValue, ConstantInt::get(Type::getInt32Ty(*m_context), i - startChannel), "", insertPos);
    }
  }

  // Store interpolation results to inputs
  Value *input;
  if (interpTy == inputTy) {
    input = interp;
  } else {
    assert(canBitCast(interpTy, inputTy));
    input = new BitCastInst(interp, inputTy, "", insertPos);
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
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTcsGenericOutputImport(Type *outputTy, unsigned location, Value *locOffset,
                                                           Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(compIdx);

  auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, location, locOffset, compIdx, vertexIdx, insertPos);
  return readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, insertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of vertex shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchVsGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                        Instruction *insertPos) {
  auto outputTy = output->getType();

  if (m_hasTs) {
    auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, location, compIdx, insertPos);
    writeValueToLds(false, output, ldsOffset, insertPos);
  } else {
    if (m_hasGs) {
      assert(outputTy->isIntOrIntVectorTy() || outputTy->isFPOrFPVectorTy());

      const unsigned bitWidth = outputTy->getScalarSizeInBits();
      if (bitWidth == 64) {
        // For 64-bit data type, the component indexing must multiply by 2
        compIdx *= 2;

        unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() * 2 : 2;

        outputTy = FixedVectorType::get(Type::getFloatTy(*m_context), compCount);
        output = new BitCastInst(output, outputTy, "", insertPos);
      } else
        assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

      storeValueToEsGsRing(output, location, compIdx, insertPos);
    } else
      addExportInstForGenericOutput(output, location, compIdx, insertPos);
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
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchTcsGenericOutputExport(Value *output, unsigned location, Value *locOffset,
                                                         Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(compIdx);

  Type *outputTy = output->getType();
  auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, location, locOffset, compIdx, vertexIdx, insertPos);
  writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, insertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of tessellation evaluation shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchTesGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                         Instruction *insertPos) {
  if (m_hasGs) {
    auto outputTy = output->getType();
    assert(outputTy->isIntOrIntVectorTy() || outputTy->isFPOrFPVectorTy());

    const unsigned bitWidth = outputTy->getScalarSizeInBits();
    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx *= 2;

      unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() * 2 : 2;
      outputTy = FixedVectorType::get(Type::getFloatTy(*m_context), compCount);

      output = new BitCastInst(output, outputTy, "", insertPos);
    } else
      assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

    storeValueToEsGsRing(output, location, compIdx, insertPos);
  } else
    addExportInstForGenericOutput(output, location, compIdx, insertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of geometry shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param streamId : ID of output vertex stream
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchGsGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                        unsigned streamId, Instruction *insertPos) {
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

    output = new BitCastInst(output, outputTy, "", insertPos);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word to dword and
  // store dword to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size is based on number of dwords.

  assert(compIdx <= 4);

  storeValueToGsVsRing(output, location, compIdx, streamId, insertPos);
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
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchMeshGenericOutputExport(Value *output, unsigned location, Value *locOffset,
                                                          Value *compIdx, Value *vertexOrPrimitiveIdx,
                                                          bool isPerPrimitive, Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  // outputOffset = (location + locOffset) * 4 + compIdx * (bitWidth == 64 ? 2 : 1)
  Value *outputOffset = builder.CreateAdd(builder.getInt32(location), locOffset);
  outputOffset = builder.CreateShl(outputOffset, 2);

  auto outputTy = output->getType();
  if (outputTy->getScalarSizeInBits() == 64) {
    compIdx = builder.CreateShl(compIdx, 1);
  }

  outputOffset = builder.CreateAdd(outputOffset, compIdx);

  std::string callName(isPerPrimitive ? lgcName::MeshTaskWritePrimitiveOutput : lgcName::MeshTaskWriteVertexOutput);
  callName += getTypeName(outputTy);
  builder.CreateNamedCall(callName, builder.getVoidTy(), {outputOffset, vertexOrPrimitiveIdx, output}, {});
}

// =====================================================================================================================
// Patches import calls for built-in inputs of vertex shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchVsBuiltInInputImport(Type *inputTy, unsigned builtInId, Instruction *insertPos) {
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;

  switch (builtInId) {
  // BuiltInVertexIndex, BuiltInInstanceIndex, BuiltInBaseVertex, BuiltInBaseInstance, BuiltInDrawIndex
  // now handled in InOutBuilder.
  case BuiltInViewIndex: {
    if (m_pipelineState->getInputAssemblyState().enableMultiView) {
      return getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
    }
    return ConstantInt::get(Type::getInt32Ty(*m_context), 0);
  }
  default:
    llvm_unreachable("Should never be called!");
    return UndefValue::get(inputTy);
  }
}

// =====================================================================================================================
// Patches import calls for built-in inputs of tessellation control shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTcsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *elemIdx,
                                                          Value *vertexIdx, Instruction *insertPos) {
  Value *input = UndefValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl)->entryArgIdxs.tcs;
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto &inoutUsage = resUsage->inOutUsage;
  const auto &builtInInLocMap = inoutUsage.builtInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    input = readValueFromLds(false, inputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    input = readValueFromLds(false, inputTy, ldsOffset, insertPos);

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
        auto ldsOffset = calcLdsOffsetForTcsInput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, insertPos);
        input = InsertValueInst::Create(input, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      input = readValueFromLds(false, inputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInPatchVertices: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getInputAssemblyState().patchControlPoints);
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
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
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
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTesBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *elemIdx,
                                                          Value *vertexIdx, Instruction *insertPos) {
  Value *input = UndefValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  const auto &inOutUsage = resUsage->inOutUsage;
  const auto &builtInInLocMap = inOutUsage.builtInInputLocMap;
  const auto &perPatchBuiltInInLocMap = inOutUsage.perPatchBuiltInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, insertPos);

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
        auto ldsOffset = calcLdsOffsetForTesInput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(m_pipelineState->isTessOffChip(), elemTy, ldsOffset, insertPos);
        input = InsertValueInst::Create(input, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInPatchVertices: {
    unsigned patchVertices = MaxTessPatchVertices;
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
    if (hasTcs)
      patchVertices = m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;

    input = ConstantInt::get(Type::getInt32Ty(*m_context), patchVertices);

    break;
  }
  case BuiltInPrimitiveId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.patchId);
    break;
  }
  case BuiltInTessCoord: {
    auto tessCoord = m_pipelineSysValues.get(m_entryPoint)->getTessCoord();

    if (elemIdx)
      input = ExtractElementInst::Create(tessCoord, elemIdx, "", insertPos);
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
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTesInput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(m_pipelineState->isTessOffChip(), elemTy, ldsOffset, insertPos);
        input = InsertValueInst::Create(input, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      input = readValueFromLds(m_pipelineState->isTessOffChip(), inputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInViewIndex: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
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
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchGsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *vertexIdx,
                                                         Instruction *insertPos) {
  Value *input = nullptr;

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize:
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    assert(inOutUsage.builtInInputLocMap.find(builtInId) != inOutUsage.builtInInputLocMap.end());
    const unsigned loc = inOutUsage.builtInInputLocMap.find(builtInId)->second;
    assert(loc != InvalidValue);
    input = loadValueFromEsGsRing(inputTy, loc, 0, vertexIdx, insertPos);
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
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
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
// @param elemIdx : Index used for vector element indexing (could be null)
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchMeshBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *elemIdx,
                                                           Instruction *insertPos) {
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  // Handle work group size built-in
  if (builtInId == BuiltInWorkgroupSize) {
    // WorkgroupSize is a constant vector supplied by mesh shader mode.
    const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    Value *input =
        ConstantVector::get({builder.getInt32(meshMode.workgroupSizeX), builder.getInt32(meshMode.workgroupSizeY),
                             builder.getInt32(meshMode.workgroupSizeZ)});
    if (elemIdx)
      input = builder.CreateExtractElement(input, elemIdx);
    return input;
  }

  // Handle other built-ins
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->builtInUsage.mesh;
  (void(builtInUsage)); // Unused

  switch (builtInId) {
  case BuiltInDrawIndex:
    assert(builtInUsage.drawIndex);
    assert(!elemIdx); // No vector element indexing
    break;
  case BuiltInViewIndex:
    assert(builtInUsage.viewIndex);
    assert(!elemIdx); // No vector element indexing
    break;
  case BuiltInNumWorkgroups:
    assert(builtInUsage.numWorkgroups);
    inputTy = elemIdx ? FixedVectorType::get(builder.getInt32Ty(), 3) : inputTy;
    break;
  case BuiltInWorkgroupId:
    assert(builtInUsage.workgroupId);
    inputTy = elemIdx ? FixedVectorType::get(builder.getInt32Ty(), 3) : inputTy;
    break;
  case BuiltInLocalInvocationId:
    assert(builtInUsage.localInvocationId);
    inputTy = elemIdx ? FixedVectorType::get(builder.getInt32Ty(), 3) : inputTy;
    break;
  case BuiltInGlobalInvocationId:
    assert(builtInUsage.globalInvocationId);
    inputTy = elemIdx ? FixedVectorType::get(builder.getInt32Ty(), 3) : inputTy;
    break;
  case BuiltInLocalInvocationIndex:
    assert(builtInUsage.localInvocationIndex);
    assert(!elemIdx); // No vector element indexing
    break;
  case BuiltInSubgroupId:
    assert(builtInUsage.subgroupId);
    assert(!elemIdx); // No vector element indexing
    break;
  case BuiltInNumSubgroups:
    assert(builtInUsage.numSubgroups);
    assert(!elemIdx); // No vector element indexing
    break;
  default:
    llvm_unreachable("Unknown mesh shader built-in!");
    break;
  }

  std::string callName(lgcName::MeshTaskGetMeshInput);
  callName += getTypeName(inputTy);
  Value *input = builder.CreateNamedCall(callName, inputTy, builder.getInt32(builtInId), {});
  if (elemIdx)
    input = builder.CreateExtractElement(input, elemIdx);
  return input;
}

// =====================================================================================================================
// Patches import calls for built-in inputs of fragment shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param generalVal : Sample ID, only needed for BuiltInSamplePosOffset; InterpLoc, only needed for BuiltInBaryCoord
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchFsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *generalVal,
                                                         Instruction *insertPos) {
  Value *input = UndefValue::get(inputTy);

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->inOutUsage;

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);

  switch (builtInId) {
  case BuiltInSampleMask: {
    assert(inputTy->isArrayTy());

    auto sampleCoverage = getFunctionArgument(m_entryPoint, entryArgIdxs.sampleCoverage);
    auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

    // gl_SampleID = Ancillary[11:8]
    auto sampleId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                            {ancillary, builder.getInt32(8), builder.getInt32(4)});

    Value *sampleMaskIn = sampleCoverage;
    if (m_pipelineState->getRasterizerState().perSampleShading) {
      // gl_SampleMaskIn[0] = (SampleCoverage & (1 << gl_SampleID))
      sampleMaskIn = builder.CreateShl(builder.getInt32(1), sampleId);
      sampleMaskIn = builder.CreateAnd(sampleCoverage, sampleMaskIn);
    }

    // NOTE: Only gl_SampleMaskIn[0] is valid for us.
    input = builder.CreateInsertValue(input, sampleMaskIn, 0);

    break;
  }
  case BuiltInFragCoord: {
    // TODO: Support layout qualifiers "pixel_center_integer" and "origin_upper_left".
    Value *fragCoord[4] = {
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.x),
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.y),
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.z),
        getFunctionArgument(m_entryPoint, entryArgIdxs.fragCoord.w),
    };
    // Adjust gl_FragCoord.z value for the shading rate X,
    //
    // adjustedFragCoordZ = gl_FragCood.z + dFdxFine(gl_FragCood.z) * 1/16
    // adjustedFragCoordZ = gl_ShadingRate.x == 1? adjustedFragCoordZ : gl_FragCood.z
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waAdjustDepthImportVrs &&
        m_pipelineState->getShaderOptions(ShaderStageFragment).adjustDepthImportVrs) {
      const unsigned firstDppCtrl = 0xF5; // FineX:   [0,1,2,3]->[1,1,3,3]
      const unsigned secondDppCtrl = 0xA0; // FineX:  [0,1,2,3]->[0,0,2,2]
      Value *fragCoordZAsInt = builder.CreateBitCast(fragCoord[2], builder.getInt32Ty());
      Value *firstDppValue = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
                                                {fragCoordZAsInt, builder.getInt32(firstDppCtrl), builder.getInt32(15),
                                                 builder.getInt32(15), builder.getTrue()});
      firstDppValue = builder.CreateBitCast(firstDppValue, builder.getFloatTy());
      Value *secondDppValue = builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
                                                {fragCoordZAsInt, builder.getInt32(secondDppCtrl), builder.getInt32(15),
                                                 builder.getInt32(15), builder.getTrue()});
      secondDppValue = builder.CreateBitCast(secondDppValue, builder.getFloatTy());
      Value *adjustedFragCoordZ = builder.CreateFSub(firstDppValue, secondDppValue);
      adjustedFragCoordZ = builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, adjustedFragCoordZ, nullptr);
      Value *sixteenth = ConstantFP::get(builder.getFloatTy(), 1.0 / 16.0f);
      adjustedFragCoordZ = builder.CreateIntrinsic(Intrinsic::fma, builder.getFloatTy(), {adjustedFragCoordZ, sixteenth, fragCoord[2]});
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
    const bool perSampleShading = m_pipelineState->getRasterizerState().perSampleShading;
    input = patchFsGenericInputImport(inputTy, loc, nullptr, nullptr, false, nullptr, InOutInfo::InterpModeSmooth,
                                      perSampleShading ? InOutInfo::InterpLocSample : InOutInfo::InterpLocCenter, false,
                                      insertPos);
    break;
  }
  case BuiltInHelperInvocation: {
    input = builder.CreateIntrinsic(Intrinsic::amdgcn_ps_live, {}, {});
    input = builder.CreateNot(input);
    break;
  }
  case BuiltInPrimitiveId:
  case BuiltInLayer:
  case BuiltInViewportIndex:
  case BuiltInViewIndex: {
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

    // Emulation for "in int gl_PrimitiveID" or "in int gl_Layer" or "in int gl_ViewportIndex"
    // or "in int gl_ViewIndex".
    input = patchFsGenericInputImport(inputTy, loc, nullptr, nullptr, isPerPrimitive, nullptr,
                                      InOutInfo::InterpModeFlat, InOutInfo::InterpLocCenter, false, insertPos);
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
      locCount = builtInUsage.clipDistance + builtInUsage.cullDistance > 4 ? 2 : 1;
      startChannel = builtInUsage.clipDistance % 4;
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

    input = getShadingRate(insertPos);
    break;
  }
  // Handle internal-use built-ins for sample position emulation
  case BuiltInNumSamples: {
    if (m_pipelineState->isUnlinked()) {
      input = builder.CreateRelocationConstant(reloc::NumSamples);
    } else {
      input = builder.getInt32(m_pipelineState->getRasterizerState().numSamples);
    }
    break;
  }
  case BuiltInSamplePatternIdx: {
    if (m_pipelineState->isUnlinked()) {
      input = builder.CreateRelocationConstant(reloc::SamplePatternIdx);
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
                             getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.center), insertPos);
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
                             getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center), insertPos);
    break;
  }
  case BuiltInSamplePosOffset: {
    input = getSamplePosOffset(inputTy, generalVal, insertPos);
    break;
  }
  case BuiltInSamplePosition: {
    input = getSamplePosition(inputTy, insertPos);
    break;
  }
  case BuiltInBaryCoord:
  case BuiltInBaryCoordNoPerspKHR: {
    assert(isa<ConstantInt>(generalVal));
    unsigned idx = 0;
    unsigned interpLoc = cast<ConstantInt>(generalVal)->getZExtValue();
    Value *iJCoord = nullptr;
    if (interpLoc == InOutInfo::InterpLocCentroid) {
      if (BuiltInBaryCoord == builtInId)
        iJCoord = adjustCentroidIj(getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.centroid),
                                   getFunctionArgument(m_entryPoint, entryArgIdxs.perspInterp.center), insertPos);
      else
        iJCoord = adjustCentroidIj(getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.centroid),
                                   getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center), insertPos);
    } else if (interpLoc == InOutInfo::InterpLocSample) {
      idx = (BuiltInBaryCoord == builtInId) ? entryArgIdxs.perspInterp.sample : entryArgIdxs.linearInterp.sample;
      iJCoord = getFunctionArgument(m_entryPoint, idx);
    } else {
      idx = (BuiltInBaryCoord == builtInId) ? entryArgIdxs.perspInterp.center : entryArgIdxs.linearInterp.center;
      iJCoord = getFunctionArgument(m_entryPoint, idx);
    }

    auto iCoord = builder.CreateExtractElement(iJCoord, uint64_t(0));
    auto jCoord = builder.CreateExtractElement(iJCoord, 1);
    auto primType = m_pipelineState->getPrimitiveType();
    auto provokingVertexMode = m_pipelineState->getRasterizerState().provokingVertexMode;
    switch (primType) {
    case PrimitiveType::Point: {
      // Points
      input = builder.CreateInsertElement(input, ConstantFP::get(builder.getFloatTy(), 1.0), uint64_t(0));
      input = builder.CreateInsertElement(input, ConstantFP::get(builder.getFloatTy(), 0.0), 1);
      input = builder.CreateInsertElement(input, ConstantFP::get(builder.getFloatTy(), 0.0), 2);
      break;
    }
    case PrimitiveType::LineList:
    case PrimitiveType::LineStrip: {
      // Lines
      // The weight of vertex0 is (1 - i - j), the weight of vertex1 is (i + j).
      auto kCoord = builder.CreateFSub(ConstantFP::get(builder.getFloatTy(), 1.0), iCoord);
      kCoord = builder.CreateFSub(kCoord, jCoord);
      jCoord = builder.CreateFAdd(iCoord, jCoord);
      input = builder.CreateInsertElement(input, kCoord, uint64_t(0));
      input = builder.CreateInsertElement(input, jCoord, 1);
      input = builder.CreateInsertElement(input, ConstantFP::get(builder.getFloatTy(), 0.0), 2);
      break;
    }
    case PrimitiveType::TriangleList: {
      // Triangles
      // V0 ==> Attr_indx2
      // V1 ==> Attr_indx0
      // V2 ==> Attr_indx1
      auto kCoord = builder.CreateFSub(ConstantFP::get(builder.getFloatTy(), 1.0), iCoord);
      kCoord = builder.CreateFSub(kCoord, jCoord);
      input = builder.CreateInsertElement(input, iCoord, 2);
      input = builder.CreateInsertElement(input, jCoord, uint64_t(0));
      input = builder.CreateInsertElement(input, kCoord, 1);
      break;
    }
    case PrimitiveType::TriangleFan: {
      Value *odd = UndefValue::get(inputTy);
      Value *even = UndefValue::get(inputTy);
      auto kCoord = builder.CreateFSub(ConstantFP::get(builder.getFloatTy(), 1.0), iCoord);
      kCoord = builder.CreateFSub(kCoord, jCoord);
      if (provokingVertexMode == ProvokingVertexLast) {
        odd = builder.CreateInsertElement(odd, iCoord, uint64_t(0));
        odd = builder.CreateInsertElement(odd, jCoord, 1);
        odd = builder.CreateInsertElement(odd, kCoord, 2);

        even = builder.CreateInsertElement(even, iCoord, 2);
        even = builder.CreateInsertElement(even, jCoord, uint64_t(0));
        even = builder.CreateInsertElement(even, kCoord, 1);
      } else {
        odd = builder.CreateInsertElement(odd, iCoord, 2);
        odd = builder.CreateInsertElement(odd, jCoord, uint64_t(0));
        odd = builder.CreateInsertElement(odd, kCoord, 1);

        even = builder.CreateInsertElement(even, iCoord, 1);
        even = builder.CreateInsertElement(even, jCoord, 2);
        even = builder.CreateInsertElement(even, kCoord, uint64_t(0));
      }
      auto primitiveId = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInPrimitiveId, nullptr, insertPos);
      auto evenV = builder.CreateSRem(primitiveId, builder.getInt32(2));
      Value *con = builder.CreateICmpEQ(evenV, builder.getInt32(0));
      input = builder.CreateSelect(con, even, odd);
      break;
    }
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TriangleStripAdjacency: {
      Value *odd = UndefValue::get(inputTy);
      Value *even = UndefValue::get(inputTy);
      auto kCoord = builder.CreateFSub(ConstantFP::get(builder.getFloatTy(), 1.0), iCoord);
      kCoord = builder.CreateFSub(kCoord, jCoord);
      if (provokingVertexMode == ProvokingVertexLast) {
        odd = builder.CreateInsertElement(odd, iCoord, uint64_t(0));
        odd = builder.CreateInsertElement(odd, jCoord, 1);
        odd = builder.CreateInsertElement(odd, kCoord, 2);

        even = builder.CreateInsertElement(even, iCoord, 1);
        even = builder.CreateInsertElement(even, jCoord, 2);
        even = builder.CreateInsertElement(even, kCoord, uint64_t(0));
      } else {
        odd = builder.CreateInsertElement(odd, iCoord, 2);
        odd = builder.CreateInsertElement(odd, jCoord, uint64_t(0));
        odd = builder.CreateInsertElement(odd, kCoord, 1);

        even = builder.CreateInsertElement(even, iCoord, 1);
        even = builder.CreateInsertElement(even, jCoord, 2);
        even = builder.CreateInsertElement(even, kCoord, uint64_t(0));
      }
      auto primitiveId = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInPrimitiveId, nullptr, insertPos);
      auto evenV = builder.CreateSRem(primitiveId, builder.getInt32(2));
      Value *con = builder.CreateICmpEQ(evenV, builder.getInt32(0));
      input = builder.CreateSelect(con, even, odd);
      break;
    }
    case PrimitiveType::TriangleListAdjacency: {
      auto kCoord = builder.CreateFSub(ConstantFP::get(builder.getFloatTy(), 1.0), iCoord);
      kCoord = builder.CreateFSub(kCoord, jCoord);
      Value *odd = UndefValue::get(inputTy);
      odd = builder.CreateInsertElement(odd, iCoord, uint64_t(0));
      odd = builder.CreateInsertElement(odd, jCoord, 1);
      odd = builder.CreateInsertElement(odd, kCoord, 2);

      Value *even = UndefValue::get(inputTy);
      even = builder.CreateInsertElement(even, iCoord, 1);
      even = builder.CreateInsertElement(even, jCoord, 2);
      even = builder.CreateInsertElement(even, kCoord, uint64_t(0));

      auto primitiveId = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInPrimitiveId, nullptr, insertPos);
      auto evenV = builder.CreateSRem(primitiveId, builder.getInt32(2));
      Value *con = builder.CreateICmpEQ(evenV, builder.getInt32(0));
      input = builder.CreateSelect(con, even, odd);
      break;
    }
    default: {
      llvm_unreachable("Should never be called!");
      break;
    }
    }
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
// @param insertPos : Insert position
Value *PatchInOutImportExport::getSamplePosOffset(Type *inputTy, Value *sampleId, Instruction *insertPos) {
  // Gets the offset of sample position relative to the pixel center for the specified sample ID
  BuilderBase builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *numSamples = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInNumSamples, nullptr, insertPos);
  Value *patternIdx = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSamplePatternIdx, nullptr, insertPos);
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
// @param insertPos : Insert position
Value *PatchInOutImportExport::getSamplePosition(Type *inputTy, Instruction *insertPos) {
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *sampleId = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSampleId, nullptr, insertPos);
  Value *input = patchFsBuiltInInputImport(inputTy, BuiltInSamplePosOffset, sampleId, insertPos);
  return builder.CreateFAdd(input, ConstantFP::get(inputTy, 0.5));
}

// =====================================================================================================================
// Patches import calls for built-in outputs of tessellation control shader.
//
// @param outputTy : Type of output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Output array outermost index used for vertex indexing (could be null)
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTcsBuiltInOutputImport(Type *outputTy, unsigned builtInId, Value *elemIdx,
                                                           Value *vertexIdx, Instruction *insertPos) {
  Value *output = UndefValue::get(outputTy);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto &builtInUsage = resUsage->builtInUsage.tcs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInUsage.position);
    (void(builtInUsage)); // unused

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    output = readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    assert(builtInUsage.pointSize);
    (void(builtInUsage)); // unused

    assert(!elemIdx);
    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    output = readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, insertPos);

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
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(m_pipelineState->isTessOffChip(), elemTy, ldsOffset, insertPos);
        output = InsertValueInst::Create(output, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      output = readValueFromLds(m_pipelineState->isTessOffChip(), outputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInTessLevelOuter: {
    assert(builtInUsage.tessLevelOuter);
    (void(builtInUsage)); // Unused

    const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

    // tessLevelOuter (float[4]) + tessLevelInner (float[2])
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + elemIdx
    if (outputTy->isArrayTy()) {
      // Import the whole tessLevelOuter array
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        Value *ldsOffset = BinaryOperator::CreateMul(
            relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
        ldsOffset = BinaryOperator::CreateAdd(
            ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart + i), ldsOffset, "",
            insertPos);
        auto elem = readValueFromLds(false, Type::getFloatTy(*m_context), ldsOffset, insertPos);
        output = InsertValueInst::Create(output, elem, {i}, "", insertPos);
      }
    } else {
      // Import a single element of tessLevelOuter array
      Value *ldsOffset = BinaryOperator::CreateMul(
          relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
      ldsOffset = BinaryOperator::CreateAdd(
          ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart), "", insertPos);
      ldsOffset = BinaryOperator::CreateAdd(ldsOffset, elemIdx, "", insertPos);
      output = readValueFromLds(false, outputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInTessLevelInner: {
    assert(builtInUsage.tessLevelInner);
    (void(builtInUsage)); // Unused

    const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

    // tessLevelOuter (float[4]) + tessLevelInner (float[2])
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + 4 + elemIdx
    if (outputTy->isArrayTy()) {
      // Import the whole tessLevelInner array
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        Value *ldsOffset = BinaryOperator::CreateMul(
            relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
        ldsOffset = BinaryOperator::CreateAdd(
            ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart + 4 + i), ldsOffset, "",
            insertPos);
        auto elem = readValueFromLds(false, Type::getFloatTy(*m_context), ldsOffset, insertPos);
        output = InsertValueInst::Create(output, elem, {i}, "", insertPos);
      }
    } else {
      // Import a single element of tessLevelInner array
      Value *ldsOffset = BinaryOperator::CreateMul(
          relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
      ldsOffset = BinaryOperator::CreateAdd(
          ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart + 4), "",
          insertPos);
      ldsOffset = BinaryOperator::CreateAdd(ldsOffset, elemIdx, "", insertPos);
      output = readValueFromLds(false, outputTy, ldsOffset, insertPos);
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
  auto outputTy = output->getType();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  auto &builtInUsage = resUsage->builtInUsage.vs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    if (!static_cast<bool>(builtInUsage.position))
      return;

    if (m_hasTs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, insertPos);
      writeValueToLds(false, output, ldsOffset, insertPos);
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
  case BuiltInPointSize: {
    if (!static_cast<bool>(builtInUsage.pointSize))
      return;

    if (isa<UndefValue>(output)) {
      // NOTE: gl_PointSize is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInUsage.pointSize = false;
      return;
    }

    if (m_hasTs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, insertPos);
      writeValueToLds(false, output, ldsOffset, insertPos);
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
  case BuiltInClipDistance: {
    if (builtInUsage.clipDistance == 0)
      return;

    if (isa<UndefValue>(output)) {
      // NOTE: gl_ClipDistance[] is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInUsage.clipDistance = 0;
      return;
    }

    if (m_hasTs) {
      assert(outputTy->isArrayTy());

      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy->getArrayElementType(), loc, 0, insertPos);

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(false, elem, ldsOffset, insertPos);

        ldsOffset =
            BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
      }
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap.find(builtInId)->second;

        storeValueToEsGsRing(output, loc, 0, insertPos);
      } else {
        // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
        m_clipDistance = output;
      }
    }

    break;
  }
  case BuiltInCullDistance: {
    if (builtInUsage.cullDistance == 0)
      return;

    if (isa<UndefValue>(output)) {
      // NOTE: gl_CullDistance[] is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInUsage.cullDistance = 0;
      return;
    }

    if (m_hasTs) {
      assert(outputTy->isArrayTy());

      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy->getArrayElementType(), loc, 0, insertPos);

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(false, elem, ldsOffset, insertPos);

        ldsOffset =
            BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
      }
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap.find(builtInId)->second;

        storeValueToEsGsRing(output, loc, 0, insertPos);
      } else {
        // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
        m_cullDistance = output;
      }
    }

    break;
  }
  case BuiltInLayer: {
    if (!static_cast<bool>(builtInUsage.layer))
      return;
    m_layer = output;

    break;
  }
  case BuiltInViewportIndex: {
    if (!static_cast<bool>(builtInUsage.viewportIndex))
      return;

    // NOTE: Only last non-fragment shader stage has to export the value of gl_ViewportIndex.
    if (!m_hasTs && !m_hasGs) {
      if (m_gfxIp.major <= 8)
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
      else {
        // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
        m_viewportIndex = output;
      }
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
  auto outputTy = output->getType();

  const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto &builtInUsage = resUsage->builtInUsage.tcs;
  const auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    if (!static_cast<bool>(builtInUsage.position))
      return;

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    if (!static_cast<bool>(builtInUsage.pointSize))
      return;

    assert(!elemIdx);
    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap.find(builtInId)->second;

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, insertPos);

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
        auto ldsOffset = calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, elemIdx, vertexIdx, insertPos);
        writeValueToLds(m_pipelineState->isTessOffChip(), elem, ldsOffset, insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      writeValueToLds(m_pipelineState->isTessOffChip(), output, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInTessLevelOuter: {
    const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

    // tessLevelOuter (float[4]) + tessLevelInner (float[2])
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + elemIdx
    if (outputTy->isArrayTy()) {
      // Export the whole tessLevelOuter array
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        Value *ldsOffset = BinaryOperator::CreateMul(
            relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
        ldsOffset = BinaryOperator::CreateAdd(
            ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart + i), ldsOffset, "",
            insertPos);
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(false, elem, ldsOffset, insertPos);
      }
    } else {
      // Export a single element of tessLevelOuter array
      Value *ldsOffset = BinaryOperator::CreateMul(
          relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
      ldsOffset = BinaryOperator::CreateAdd(
          ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart), "", insertPos);
      ldsOffset = BinaryOperator::CreateAdd(ldsOffset, elemIdx, "", insertPos);
      writeValueToLds(false, output, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInTessLevelInner: {
    const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

    // tessLevelOuter (float[4]) + tessLevelInner (float[2])
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + 4 + elemIdx
    if (outputTy->isArrayTy()) {
      // Export the whole tessLevelInner array
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        Value *ldsOffset = BinaryOperator::CreateMul(
            relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
        ldsOffset = BinaryOperator::CreateAdd(
            ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart + 4 + i), ldsOffset, "",
            insertPos);
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(false, elem, ldsOffset, insertPos);
      }
    } else {
      // Export a single element of tessLevelInner array
      Value *ldsOffset = BinaryOperator::CreateMul(
          relativeId, ConstantInt::get(Type::getInt32Ty(*m_context), MaxTessFactorsPerPatch), "", insertPos);
      ldsOffset = BinaryOperator::CreateAdd(
          ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.onChip.tessFactorStart + 4), "",
          insertPos);
      ldsOffset = BinaryOperator::CreateAdd(ldsOffset, elemIdx, "", insertPos);
      writeValueToLds(false, output, ldsOffset, insertPos);
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
  case BuiltInPosition: {
    if (!static_cast<bool>(builtInUsage.position))
      return;

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
    } else
      addExportInstForBuiltInOutput(output, builtInId, insertPos);

    break;
  }
  case BuiltInPointSize: {
    if (!static_cast<bool>(builtInUsage.pointSize))
      return;

    if (isa<UndefValue>(output)) {
      // NOTE: gl_PointSize is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInUsage.pointSize = false;
      return;
    }

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
    } else
      addExportInstForBuiltInOutput(output, builtInId, insertPos);

    break;
  }
  case BuiltInClipDistance: {
    if (builtInUsage.clipDistance == 0)
      return;

    if (isa<UndefValue>(output)) {
      // NOTE: gl_ClipDistance[] is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInUsage.clipDistance = 0;
      return;
    }

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
    } else {
      // NOTE: The export of gl_ClipDistance[] is delayed and is done before entry-point returns.
      m_clipDistance = output;
    }

    break;
  }
  case BuiltInCullDistance: {
    if (builtInUsage.cullDistance == 0)
      return;

    if (isa<UndefValue>(output)) {
      // NOTE: gl_CullDistance[] is always declared as a field of gl_PerVertex. We have to check the output
      // value to determine if it is actually referenced in shader.
      builtInUsage.cullDistance = 0;
      return;
    }

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap.find(builtInId)->second;

      storeValueToEsGsRing(output, loc, 0, insertPos);
    } else {
      // NOTE: The export of gl_CullDistance[] is delayed and is done before entry-point returns.
      m_cullDistance = output;
    }

    break;
  }
  case BuiltInLayer: {
    if (!static_cast<bool>(builtInUsage.layer))
      return;

    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

    // NOTE: Only last non-fragment shader stage has to export the value of gl_Layer.
    if (!m_hasGs && !static_cast<bool>(enableMultiView)) {
      // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
      m_layer = output;
    }

    break;
  }
  case BuiltInViewportIndex: {
    if (!static_cast<bool>(builtInUsage.viewportIndex))
      return;

    // NOTE: Only last non-fragment shader stage has to export the value of gl_ViewportIndex.
    if (!m_hasGs) {
      if (m_gfxIp.major <= 8)
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
      else {
        // NOTE: The export of gl_ViewportIndex is delayed and is done before entry-point returns.
        m_viewportIndex = output;
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
// Patches export calls for built-in outputs of geometry shader.
//
// @param output : Output value
// @param builtInId : ID of the built-in variable
// @param streamId : ID of output vertex stream
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchGsBuiltInOutputExport(Value *output, unsigned builtInId, unsigned streamId,
                                                        Instruction *insertPos) {
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
  storeValueToGsVsRing(output, loc, 0, streamId, insertPos);
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

  auto outputTy = output->getType();

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

    builder.CreateNamedCall(lgcName::MeshTaskSetPrimitiveIndices + getTypeName(outputTy), builder.getVoidTy(),
                            {vertexOrPrimitiveIdx, output}, {});
    return;
  }

  // Handle cull primitive built-in
  if (builtInId == BuiltInCullPrimitive) {
    assert(isPerPrimitive);
    assert(outputTy->isIntegerTy(1)); // Must be boolean
    builder.CreateNamedCall(lgcName::MeshTaskSetPrimitiveCulled, builder.getVoidTy(), {vertexOrPrimitiveIdx, output},
                            {});
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

  std::string callName(isPerPrimitive ? lgcName::MeshTaskWritePrimitiveOutput : lgcName::MeshTaskWriteVertexOutput);
  callName += getTypeName(outputTy);
  builder.CreateNamedCall(callName, builder.getVoidTy(), {outputOffset, vertexOrPrimitiveIdx, output}, {});
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
// @param insertPos : Where to insert the store instruction
void PatchInOutImportExport::patchXfbOutputExport(Value *output, unsigned xfbBuffer, unsigned xfbOffset,
                                                  unsigned streamId, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader);

  Value *streamOutBufDesc = m_pipelineSysValues.get(m_entryPoint)->getStreamOutBufDesc(xfbBuffer);

  const auto &xfbStrides = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.xfbStrides;
  unsigned xfbStride = xfbStrides[xfbBuffer];

  auto outputTy = output->getType();
  unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;
  unsigned bitWidth = outputTy->getScalarSizeInBits();

  if (bitWidth == 64) {
    // Cast 64-bit output to 32-bit
    compCount *= 2;
    bitWidth = 32;
    outputTy = FixedVectorType::get(Type::getFloatTy(*m_context), compCount);
    output = new BitCastInst(output, outputTy, "", insertPos);
  }
  assert(bitWidth == 16 || bitWidth == 32);

  if (compCount == 8) {
    // vec8 -> vec4 + vec4
    assert(bitWidth == 32);

    Constant *shuffleMask0123[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), 0), ConstantInt::get(Type::getInt32Ty(*m_context), 1),
        ConstantInt::get(Type::getInt32Ty(*m_context), 2), ConstantInt::get(Type::getInt32Ty(*m_context), 3)};
    Value *compX4 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask0123), "", insertPos);

    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);

    Constant *shuffleMask4567[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), 4), ConstantInt::get(Type::getInt32Ty(*m_context), 5),
        ConstantInt::get(Type::getInt32Ty(*m_context), 6), ConstantInt::get(Type::getInt32Ty(*m_context), 7)};
    compX4 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask4567), "", insertPos);

    xfbOffset += 4 * (bitWidth / 8);
    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);
  } else if (compCount == 6) {
    // vec6 -> vec4 + vec2
    assert(bitWidth == 32);

    // NOTE: This case is generated by copy shader, which casts 64-bit outputs to float.
    Constant *shuffleMask0123[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), 0), ConstantInt::get(Type::getInt32Ty(*m_context), 1),
        ConstantInt::get(Type::getInt32Ty(*m_context), 2), ConstantInt::get(Type::getInt32Ty(*m_context), 3)};
    Value *compX4 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask0123), "", insertPos);

    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);

    Constant *shuffleMask45[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 4),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 5)};
    Value *compX2 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask45), "", insertPos);

    xfbOffset += 4 * (bitWidth / 8);
    storeValueToStreamOutBuffer(compX2, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);
  } else {
    // 16vec4, 16vec3, 16vec2, 16scalar
    // vec4, vec3, vec2, scalar
    if (outputTy->isVectorTy() && compCount == 1) {
      // NOTE: We translate vec1 to scalar. SPIR-V translated from DX has such usage.
      output = ExtractElementInst::Create(output, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);
    }

    storeValueToStreamOutBuffer(output, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);
  }
}

// =====================================================================================================================
// Creates the LLPC intrinsic "lgc.streamoutbuffer.store.f32" to store value to stream-out buffer.
//
// @param storeValue : Value to store
// @param xfbStride : Transform feedback stride
// @param [out] funcName : Function name to add mangling to
void PatchInOutImportExport::createStreamOutBufferStoreFunction(Value *storeValue, unsigned xfbStride,
                                                                std::string &funcName) {
  addTypeMangling(nullptr, {storeValue}, funcName);

  // define void @lgc.streamoutbuffer.store.f32(
  //      float %storeValue, <4 x i32> %streamOutBufDesc, i32 %writeIndex, i32 %threadId,
  //      i32 %vertexCount, i32 %xfbOffset, i32 %streamOffset)
  // {
  // .entry
  //     %1 = icmp ult i32 %threadId, %vtxCount
  //     br i1 %1, label %.store, label %.end
  //
  // .store:
  //     call void llvm.amdgcn.struct.tbuffer.store.f32(
  //         float %storeValue, <4 x i32> %streamOutBufDesc, i32 %writeIndex,
  //         i32 %xfbOffset, i32 %streamOffset, i32 %format, i32 %coherent)
  //     br label %.end
  //
  // .end:
  //     ret void
  // }

  Type *argTys[] = {
      storeValue->getType(),                                 // %storeValue
      FixedVectorType::get(Type::getInt32Ty(*m_context), 4), // %streamOutBufDesc
      Type::getInt32Ty(*m_context),                          // %writeIndex
      Type::getInt32Ty(*m_context),                          // %threadId
      Type::getInt32Ty(*m_context),                          // %vertexCount
      Type::getInt32Ty(*m_context),                          // %xfbOffset
      Type::getInt32Ty(*m_context)                           // %streamOffset
  };
  auto funcTy = FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, funcName, m_module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::NoUnwind);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *storedValue = argIt++;
  Value *streamOutBufDesc = argIt++;
  Value *writeIndex = argIt++;
  Value *threadId = argIt++;
  Value *vertexCount = argIt++;
  Value *xfbOffset = argIt++;
  Value *streamOffset = argIt;

  // Create ".end" block
  BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);
  ReturnInst::Create(*m_context, endBlock);

  // Create ".store" block
  BasicBlock *storeBlock = BasicBlock::Create(*m_context, ".store", func, endBlock);

  // Create entry block
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func, storeBlock);
  auto threadValid = new ICmpInst(*entryBlock, ICmpInst::ICMP_ULT, threadId, vertexCount);

  if (m_shaderStage != ShaderStageCopyShader) {
    // Setup out-of-range value. GPU will drop stream-out buffer writing when the thread is invalid.
    unsigned outofRangeValue = 0xFFFFFFFF;
    // Divide outofRangeValue by xfbStride only for GFX8.
    if (m_gfxIp.major == 8)
      outofRangeValue /= xfbStride;
    outofRangeValue -= (m_pipelineState->getShaderWaveSize(m_shaderStage) - 1);
    writeIndex = SelectInst::Create(threadValid, writeIndex,
                                    ConstantInt::get(Type::getInt32Ty(*m_context), outofRangeValue), "", entryBlock);
    BranchInst::Create(storeBlock, entryBlock);
  } else
    BranchInst::Create(storeBlock, endBlock, threadValid, entryBlock);

  auto storeTy = storeValue->getType();

  unsigned compCount = storeTy->isVectorTy() ? cast<FixedVectorType>(storeTy)->getNumElements() : 1;
  assert(compCount <= 4);

  const uint64_t bitWidth = storeTy->getScalarSizeInBits();
  assert(bitWidth == 16 || bitWidth == 32);

  static const char *const callNames[4][2] = {
      {"llvm.amdgcn.struct.tbuffer.store.f16", "llvm.amdgcn.struct.tbuffer.store.f32"},
      {"llvm.amdgcn.struct.tbuffer.store.v2f16", "llvm.amdgcn.struct.tbuffer.store.v2f32"},
      {nullptr, "llvm.amdgcn.struct.tbuffer.store.v3f32"},
      {"llvm.amdgcn.struct.tbuffer.store.v4f16", "llvm.amdgcn.struct.tbuffer.store.v4f32"},
  };
  StringRef callName(callNames[compCount - 1][bitWidth == 32]);

  unsigned format = 0;
  switch (m_gfxIp.major) {
  default: {
    CombineFormat formatOprd = {};
    formatOprd.bits.nfmt = BUF_NUM_FORMAT_FLOAT;
    static const unsigned char dfmtTable[4][2] = {
        {BUF_DATA_FORMAT_16, BUF_DATA_FORMAT_32},
        {BUF_DATA_FORMAT_16_16, BUF_DATA_FORMAT_32_32},
        {BUF_DATA_FORMAT_INVALID, BUF_DATA_FORMAT_32_32_32},
        {BUF_DATA_FORMAT_16_16_16_16, BUF_DATA_FORMAT_32_32_32_32},
    };
    formatOprd.bits.dfmt = dfmtTable[compCount - 1][bitWidth == 32];
    format = formatOprd.u32All;
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
  }

  // byteOffset = streamOffsets[xfbBuffer] * 4 +
  //              (writeIndex + threadId) * bufferStride[bufferId] +
  //              xfbOffset
  CoherentFlag coherent = {};
  coherent.bits.glc = true;
  coherent.bits.slc = true;
  Value *args[] = {
      storedValue,                                                    // value
      streamOutBufDesc,                                               // desc
      writeIndex,                                                     // vindex
      xfbOffset,                                                      // offset
      streamOffset,                                                   // soffset
      ConstantInt::get(Type::getInt32Ty(*m_context), format),         // format
      ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All) // glc, slc
  };
  emitCall(callName, Type::getVoidTy(*m_context), args, {}, storeBlock);
  BranchInst::Create(endBlock, storeBlock);
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
// @param insertPos : Where to insert write instructions
unsigned PatchInOutImportExport::combineBufferStore(const std::vector<Value *> &storeValues, unsigned startIdx,
                                                    unsigned valueOffset, Value *bufDesc, Value *storeOffset,
                                                    Value *bufBase, CoherentFlag coherent, Instruction *insertPos) {
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
        storeValue = UndefValue::get(storeTy);

        for (unsigned i = 0; i < compCount; ++i) {
          storeValue = InsertElementInst::Create(storeValue, storeValues[startIdx + i],
                                                 ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
        }
      } else
        storeValue = storeValues[startIdx];

      auto writeOffset = BinaryOperator::CreateAdd(
          storeOffset, ConstantInt::get(Type::getInt32Ty(*m_context), valueOffset * 4), "", insertPos);
      Value *args[] = {
          storeValue,                                                                      // vdata
          bufDesc,                                                                         // rsrc
          writeOffset,                                                                     // voffset
          bufBase,                                                                         // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), (*m_buffFormats)[compCount - 1]), // format
          ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All)                  // glc
      };
      emitCall(funcName, Type::getVoidTy(*m_context), args, {}, insertPos);

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
// @param insertPos : Where to insert write instructions
unsigned PatchInOutImportExport::combineBufferLoad(std::vector<Value *> &loadValues, unsigned startIdx, Value *bufDesc,
                                                   Value *loadOffset, Value *bufBase, CoherentFlag coherent,
                                                   Instruction *insertPos) {
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
      auto writeOffset = BinaryOperator::CreateAdd(
          loadOffset, ConstantInt::get(Type::getInt32Ty(*m_context), startIdx * 4), "", insertPos);
      Value *args[] = {
          bufDesc,                                                                         // rsrc
          writeOffset,                                                                     // voffset
          bufBase,                                                                         // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), (*m_buffFormats)[compCount - 1]), // format
          ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All)                  // glc
      };
      loadValue = emitCall(funcName, loadTyps[compCount - 1], args, {}, insertPos);
      assert(loadValue);
      if (compCount > 1) {
        for (unsigned i = 0; i < compCount; i++) {
          loadValues[startIdx + i] =
              ExtractElementInst::Create(loadValue, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
        }
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
// @param streamOutBufDesc : Transform feedback buffer descriptor
// @param insertPos : Where to insert the store instruction
void PatchInOutImportExport::storeValueToStreamOutBuffer(Value *storeValue, unsigned xfbBuffer, unsigned xfbOffset,
                                                         unsigned xfbStride, unsigned streamId, Value *streamOutBufDesc,
                                                         Instruction *insertPos) {

  auto storeTy = storeValue->getType();

  unsigned compCount = storeTy->isVectorTy() ? cast<FixedVectorType>(storeTy)->getNumElements() : 1;
  assert(compCount <= 4);

  const uint64_t bitWidth = storeTy->getScalarSizeInBits();
  assert(bitWidth == 16 || bitWidth == 32);

  if (storeTy->isIntOrIntVectorTy()) {
    Type *bitCastTy = bitWidth == 32 ? Type::getFloatTy(*m_context) : Type::getHalfTy(*m_context);
    if (compCount > 1)
      bitCastTy = FixedVectorType::get(bitCastTy, compCount);
    storeValue = new BitCastInst(storeValue, bitCastTy, "", insertPos);
  }

  // NOTE: For 16vec3, HW doesn't have a corresponding buffer store instruction. We have to split it to 16vec2 and
  // 16scalar.
  if (bitWidth == 16 && compCount == 3) {
    // 16vec3 -> 16vec2 + 16scalar
    Constant *shuffleMask01[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 1)};
    Value *compX2 = new ShuffleVectorInst(storeValue, storeValue, ConstantVector::get(shuffleMask01), "", insertPos);

    storeValueToStreamOutBuffer(compX2, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);

    Value *comp =
        ExtractElementInst::Create(storeValue, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);

    xfbOffset += 2 * (bitWidth / 8);
    storeValueToStreamOutBuffer(comp, xfbBuffer, xfbOffset, xfbStride, streamId, streamOutBufDesc, insertPos);

    return;
  }

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;

  unsigned streamOffsets[MaxTransformFeedbackBuffers] = {};
  unsigned writeIndex = 0;
  unsigned streamInfo = 0;

  if (m_shaderStage == ShaderStageVertex) {
    memcpy(streamOffsets, entryArgIdxs.vs.streamOutData.streamOffsets, sizeof(streamOffsets));
    writeIndex = entryArgIdxs.vs.streamOutData.writeIndex;
    streamInfo = entryArgIdxs.vs.streamOutData.streamInfo;
  } else if (m_shaderStage == ShaderStageTessEval) {
    memcpy(streamOffsets, entryArgIdxs.tes.streamOutData.streamOffsets, sizeof(streamOffsets));
    writeIndex = entryArgIdxs.tes.streamOutData.writeIndex;
    streamInfo = entryArgIdxs.tes.streamOutData.streamInfo;
  } else {
    assert(m_shaderStage == ShaderStageCopyShader);

    writeIndex = CopyShaderUserSgprIdxWriteIndex;
    streamInfo = CopyShaderUserSgprIdxStreamInfo;

    const auto &inoutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
    unsigned streamOffset = CopyShaderUserSgprIdxStreamOffset;

    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (inoutUsage.xfbStrides[i] > 0)
        streamOffsets[i] = streamOffset++;
    }
  }

  assert(xfbBuffer < MaxTransformFeedbackBuffers);
  assert(streamOffsets[xfbBuffer] != 0);

  Value *streamOffset = getFunctionArgument(m_entryPoint, streamOffsets[xfbBuffer]);

  streamOffset =
      BinaryOperator::CreateMul(streamOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  // vertexCount = streamInfo[22:16]
  Value *ubfeArgs[] = {getFunctionArgument(m_entryPoint, streamInfo),
                       ConstantInt::get(Type::getInt32Ty(*m_context), 16),
                       ConstantInt::get(Type::getInt32Ty(*m_context), 7)};
  Value *vertexCount = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), ubfeArgs, {}, &*insertPos);

  // Setup write index for stream-out
  Value *writeIndexVal = getFunctionArgument(m_entryPoint, writeIndex);

  if (m_gfxIp.major >= 9)
    writeIndexVal = BinaryOperator::CreateAdd(writeIndexVal, m_threadId, "", insertPos);

  std::string funcName = lgcName::StreamOutBufferStore;
  createStreamOutBufferStoreFunction(storeValue, xfbStride, funcName);

  Value *args[] = {storeValue,  streamOutBufDesc, writeIndexVal,
                   m_threadId,  vertexCount,      ConstantInt::get(Type::getInt32Ty(*m_context), xfbOffset),
                   streamOffset};
  emitCall(funcName, Type::getVoidTy(*m_context), args, {}, insertPos);
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
      new StoreInst(storeValue, storePtr, false, m_lds->getAlign().getValue(), insertPos);
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

  Value *loadValue = UndefValue::get(loadTy);

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
          new LoadInst(loadPtr->getResultElementType(), loadPtr, "", false, m_lds->getAlign().getValue(), insertPos);
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
// @param insertPos : Where to insert the store instruction
void PatchInOutImportExport::storeValueToGsVsRing(Value *storeValue, unsigned location, unsigned compIdx,
                                                  unsigned streamId, Instruction *insertPos) {
  auto storeTy = storeValue->getType();

  Type *elemTy = storeTy;
  if (storeTy->isArrayTy())
    elemTy = cast<ArrayType>(storeTy)->getElementType();
  else if (storeTy->isVectorTy())
    elemTy = cast<VectorType>(storeTy)->getElementType();

  const unsigned bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  if (m_pipelineState->getNggControl()->enableNgg) {
    // NOTE: For NGG, exporting GS output to GS-VS ring is represented by a call and the call is replaced with
    // real instructions when when NGG primitive shader is generated.
    Value *args[] = {ConstantInt::get(Type::getInt32Ty(*m_context), location),
                     ConstantInt::get(Type::getInt32Ty(*m_context), compIdx),
                     ConstantInt::get(Type::getInt32Ty(*m_context), streamId), storeValue};
    std::string callName = lgcName::NggGsOutputExport + getTypeName(storeTy);
    emitCall(callName, Type::getVoidTy(*m_context), args, {}, insertPos);
    return;
  }

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

      storeValueToGsVsRing(storeElem, location + (compIdx + i) / 4, (compIdx + i) % 4, streamId, insertPos);
    }
  } else {
    if (bitWidth == 8 || bitWidth == 16) {
      // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word
      // to dword. This is because copy shader does not know the actual data type. It only generates output
      // export calls based on number of dwords.
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

    const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
    Value *gsVsOffset = getFunctionArgument(m_entryPoint, entryArgIdxs.gs.gsVsOffset);

    auto emitCounterPair = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr();
    auto emitCounterTy = emitCounterPair.first;
    auto emitCounterPtr = emitCounterPair.second[streamId];
    auto emitCounter = new LoadInst(emitCounterTy, emitCounterPtr, "", insertPos);

    auto ringOffset = calcGsVsRingOffsetForOutput(location, compIdx, streamId, emitCounter, gsVsOffset, insertPos);

    if (m_pipelineState->isGsOnChip()) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ringOffset};
      auto ldsType = m_lds->getValueType();
      Value *storePtr = GetElementPtrInst::Create(ldsType, m_lds, idxs, "", insertPos);
      new StoreInst(storeValue, storePtr, false, m_lds->getAlign().getValue(), insertPos);
    } else {
      // NOTE: Here we use tbuffer_store instruction instead of buffer_store because we have to do explicit
      // control of soffset. This is required by swizzle enabled mode when address range checking should be
      // complied with.
      if (m_gfxIp.major <= 9) {
        CombineFormat combineFormat = {};
        combineFormat.bits.dfmt = BUF_DATA_FORMAT_32;
        combineFormat.bits.nfmt = BUF_NUM_FORMAT_UINT;
        CoherentFlag coherent = {};
        coherent.bits.glc = true;
        coherent.bits.slc = true;
        coherent.bits.swz = true;
        Value *args[] = {
            storeValue,                                                          // vdata
            m_pipelineSysValues.get(m_entryPoint)->getGsVsRingBufDesc(streamId), // rsrc
            ringOffset,                                                          // voffset
            gsVsOffset,                                                          // soffset
            ConstantInt::get(Type::getInt32Ty(*m_context), combineFormat.u32All),
            ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All) // glc, slc, swz
        };
        emitCall("llvm.amdgcn.raw.tbuffer.store.i32", Type::getVoidTy(*m_context), args, {}, insertPos);
      } else {
        CoherentFlag coherent = {};
        coherent.bits.glc = true;
        coherent.bits.slc = true;
        coherent.bits.swz = true;
        Value *args[] = {
            storeValue,                                                          // vdata
            m_pipelineSysValues.get(m_entryPoint)->getGsVsRingBufDesc(streamId), // rsrc
            ringOffset,                                                          // voffset
            gsVsOffset,                                                          // soffset
            ConstantInt::get(Type::getInt32Ty(*m_context), BUF_FORMAT_32_UINT),  // format
            ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All)      // glc, slc, swz
        };
        emitCall("llvm.amdgcn.raw.tbuffer.store.i32", Type::getVoidTy(*m_context), args, {}, insertPos);
      }
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
// @param insertPos : Where to insert the instruction
Value *PatchInOutImportExport::calcGsVsRingOffsetForOutput(unsigned location, unsigned compIdx, unsigned streamId,
                                                           Value *vertexIdx, Value *gsVsOffset,
                                                           Instruction *insertPos) {
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

    auto esGsLdsSize = ConstantInt::get(Type::getInt32Ty(*m_context), resUsage->inOutUsage.gs.calcFactor.esGsLdsSize);

    gsVsOffset = BinaryOperator::CreateExact(Instruction::LShr, gsVsOffset,
                                             ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);

    auto ringItemOffset = BinaryOperator::CreateMul(
        m_threadId, ConstantInt::get(Type::getInt32Ty(*m_context), resUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize),
        "", insertPos);

    // VertexSize is stream output vertexSize x 4 (in dwords)
    unsigned vertexSize = resUsage->inOutUsage.gs.outLocCount[streamId] * 4;
    auto vertexItemOffset =
        BinaryOperator::CreateMul(vertexIdx, ConstantInt::get(Type::getInt32Ty(*m_context), vertexSize), "", insertPos);

    ringOffset = BinaryOperator::CreateAdd(esGsLdsSize, gsVsOffset, "", insertPos);

    ringOffset = BinaryOperator::CreateAdd(ringOffset, ringItemOffset, "", insertPos);

    ringOffset = BinaryOperator::CreateAdd(ringOffset, vertexItemOffset, "", insertPos);

    unsigned attribOffset = (location * 4) + compIdx + streamBases[streamId];
    ringOffset = BinaryOperator::CreateAdd(ringOffset, ConstantInt::get(Type::getInt32Ty(*m_context), attribOffset), "",
                                           insertPos);
  } else {
    // ringOffset = ((location * 4 + compIdx) * maxVertices + vertexIdx) * 4 (in bytes);

    unsigned outputVertices = m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices;

    ringOffset = BinaryOperator::CreateAdd(
        ConstantInt::get(Type::getInt32Ty(*m_context), (location * 4 + compIdx) * outputVertices), vertexIdx, "",
        insertPos);

    ringOffset =
        BinaryOperator::CreateMul(ringOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);
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
// @param insertPos : Where to insert read instructions
Value *PatchInOutImportExport::readValueFromLds(bool offChip, Type *readTy, Value *ldsOffset, Instruction *insertPos) {
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
    ldsOffset = BinaryOperator::CreateMul(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

    CoherentFlag coherent = {};
    if (m_gfxIp.major <= 9)
      coherent.bits.glc = true;
    else if (m_gfxIp.major == 10) {
      coherent.bits.glc = true;
      coherent.bits.dlc = true;
    }
    else
      llvm_unreachable("Not implemented!");

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount) {
      combineCount = combineBufferLoad(loadValues, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, insertPos);

      for (unsigned j = i; j < i + combineCount; ++j) {
        if (bitWidth == 8)
          loadValues[j] = new TruncInst(loadValues[j], Type::getInt8Ty(*m_context), "", insertPos);
        else if (bitWidth == 16)
          loadValues[j] = new TruncInst(loadValues[j], Type::getInt16Ty(*m_context), "", insertPos);
      }
    }
  } else {
    // Read from on-chip LDS
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ldsOffset};
      auto ldsType = m_lds->getValueType();
      auto *loadPtr = GetElementPtrInst::Create(ldsType, m_lds, idxs, "", insertPos);
      auto loadTy = loadPtr->getResultElementType();
      auto loadInst = new LoadInst(loadTy, loadPtr, "", false, m_lds->getAlign().getValue(), insertPos);
      loadValues[i] = loadInst;

      if (bitWidth == 8)
        loadValues[i] = new TruncInst(loadValues[i], Type::getInt8Ty(*m_context), "", insertPos);
      else if (bitWidth == 16)
        loadValues[i] = new TruncInst(loadValues[i], Type::getInt16Ty(*m_context), "", insertPos);

      ldsOffset =
          BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
    }
  }

  // Construct <n x i8>, <n x i16>, or <n x i32> vector from load values (dwords)
  Value *castValue = nullptr;
  if (numChannels > 1) {
    auto intTy = bitWidth == 32 || bitWidth == 64
                     ? Type::getInt32Ty(*m_context)
                     : (bitWidth == 16 ? Type::getInt16Ty(*m_context) : Type::getInt8Ty(*m_context));
    auto castTy = FixedVectorType::get(intTy, numChannels);
    castValue = UndefValue::get(castTy);

    for (unsigned i = 0; i < numChannels; ++i) {
      castValue = InsertElementInst::Create(castValue, loadValues[i], ConstantInt::get(Type::getInt32Ty(*m_context), i),
                                            "", insertPos);
    }
  } else
    castValue = loadValues[0];

  // Cast <n x i8>, <n x i16> or <n x i32> vector to read value
  return new BitCastInst(castValue, readTy, "", insertPos);
}

// =====================================================================================================================
// Writes value to LDS.
//
// @param offChip : Whether to use off-chip LDS or not
// @param writeValue : Value written to LDS
// @param ldsOffset : Start offset to do LDS write operations
// @param insertPos : Where to insert write instructions
void PatchInOutImportExport::writeValueToLds(bool offChip, Value *writeValue, Value *ldsOffset,
                                             Instruction *insertPos) {
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
  Value *castValue = new BitCastInst(writeValue, castTy, "", insertPos);

  // Extract store values (dwords) from <n x i8>, <n x i16> or <n x i32> vector
  std::vector<Value *> storeValues(numChannels);
  if (numChannels > 1) {
    for (unsigned i = 0; i < numChannels; ++i) {
      storeValues[i] =
          ExtractElementInst::Create(castValue, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);

      if (bitWidth == 8 || bitWidth == 16)
        storeValues[i] = new ZExtInst(storeValues[i], Type::getInt32Ty(*m_context), "", insertPos);
    }
  } else {
    storeValues[0] = castValue;

    if (bitWidth == 8 || bitWidth == 16)
      storeValues[0] = new ZExtInst(storeValues[0], Type::getInt32Ty(*m_context), "", insertPos);
  }

  if (offChip) {
    // Write to off-chip LDS buffer
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs;

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, entryArgIdxs.offChipLdsBase);
    // Convert dword off-chip LDS offset to byte offset
    ldsOffset = BinaryOperator::CreateMul(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    CoherentFlag coherent = {};
    coherent.bits.glc = true;

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount) {
      combineCount =
          combineBufferStore(storeValues, i, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, insertPos);
    }
  } else {
    // Write to on-chip LDS
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ldsOffset};
      auto ldsType = m_lds->getValueType();
      Value *storePtr = GetElementPtrInst::Create(ldsType, m_lds, idxs, "", insertPos);
      new StoreInst(storeValues[i], storePtr, false, m_lds->getAlign().getValue(), insertPos);

      ldsOffset =
          BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
    }
  }
}

// =====================================================================================================================
// Calculates the dword offset to write value to LDS based on the specified VS output info.
//
// @param outputTy : Type of the output
// @param location : Base location of the output
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert calculation instructions
Value *PatchInOutImportExport::calcLdsOffsetForVsOutput(Type *outputTy, unsigned location, unsigned compIdx,
                                                        Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageVertex);

  // attribOffset = location * 4 + compIdx
  Value *attribOffset = ConstantInt::get(Type::getInt32Ty(*m_context), location * 4);

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  if (bitWidth == 64) {
    // For 64-bit data type, the component indexing must multiply by 2
    compIdx *= 2;
  }

  attribOffset =
      BinaryOperator::CreateAdd(attribOffset, ConstantInt::get(Type::getInt32Ty(*m_context), compIdx), "", insertPos);

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
  auto relVertexId = getFunctionArgument(m_entryPoint, entryArgIdxs.relVertexId);

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  auto vertexStride = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.inVertexStride);

  // dwordOffset = relVertexId * vertexStride + attribOffset
  auto ldsOffset = BinaryOperator::CreateMul(relVertexId, vertexStride, "", insertPos);
  ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attribOffset, "", insertPos);

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
// @param insertPos : Where to insert calculation instructions
Value *PatchInOutImportExport::calcLdsOffsetForTcsInput(Type *inputTy, unsigned location, Value *locOffset,
                                                        Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessControl);

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
  const auto &calcFactor = inOutUsage.calcFactor;

  // attribOffset = (location + locOffset) * 4 + compIdx
  Value *attribOffset = ConstantInt::get(Type::getInt32Ty(*m_context), location);

  if (locOffset)
    attribOffset = BinaryOperator::CreateAdd(attribOffset, locOffset, "", insertPos);

  attribOffset =
      BinaryOperator::CreateMul(attribOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  if (compIdx) {
    const unsigned bitWidth = inputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = BinaryOperator::CreateMul(compIdx, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    }

    attribOffset = BinaryOperator::CreateAdd(attribOffset, compIdx, "", insertPos);
  }

  // dwordOffset = (relativeId * inVertexCount + vertexId) * inVertexStride + attribOffset
  auto inVertexCount = m_pipelineState->getInputAssemblyState().patchControlPoints;
  auto inVertexCountVal = ConstantInt::get(Type::getInt32Ty(*m_context), inVertexCount);
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

  Value *ldsOffset = BinaryOperator::CreateMul(relativeId, inVertexCountVal, "", insertPos);
  ldsOffset = BinaryOperator::CreateAdd(ldsOffset, vertexIdx, "", insertPos);

  auto inVertexStride = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.inVertexStride);
  ldsOffset = BinaryOperator::CreateMul(ldsOffset, inVertexStride, "", insertPos);

  ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attribOffset, "", insertPos);

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
// @param insertPos : Where to insert calculation instructions
Value *PatchInOutImportExport::calcLdsOffsetForTcsOutput(Type *outputTy, unsigned location, Value *locOffset,
                                                         Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessControl);

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
  const auto &calcFactor = inOutUsage.calcFactor;

  auto outPatchStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.outPatchStart : calcFactor.onChip.outPatchStart;

  auto patchConstStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.patchConstStart : calcFactor.onChip.patchConstStart;

  // attribOffset = (location + locOffset) * 4 + compIdx * bitWidth / 32
  Value *attribOffset = ConstantInt::get(Type::getInt32Ty(*m_context), location);

  if (locOffset)
    attribOffset = BinaryOperator::CreateAdd(attribOffset, locOffset, "", insertPos);

  attribOffset =
      BinaryOperator::CreateMul(attribOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  if (compIdx) {
    const unsigned bitWidth = outputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = BinaryOperator::CreateMul(compIdx, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    }

    attribOffset = BinaryOperator::CreateAdd(attribOffset, compIdx, "", insertPos);
  }

  Value *ldsOffset = nullptr;

  const bool perPatch = (!vertexIdx); // Vertex indexing is unavailable for per-patch output
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
  if (perPatch) {
    // dwordOffset = patchConstStart + relativeId * patchConstSize + attribOffset
    auto patchConstSize = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.patchConstSize);
    ldsOffset = BinaryOperator::CreateMul(relativeId, patchConstSize, "", insertPos);

    auto patchConstStartVal = ConstantInt::get(Type::getInt32Ty(*m_context), patchConstStart);
    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, patchConstStartVal, "", insertPos);

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attribOffset, "", insertPos);
  } else {
    // dwordOffset = outPatchStart + (relativeId * outVertexCount + vertexId) * outVertexStride + attribOffset
    //             = outPatchStart + relativeId * outPatchSize + vertexId  * outVertexStride + attribOffset
    auto outPatchSize = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.outPatchSize);
    ldsOffset = BinaryOperator::CreateMul(relativeId, outPatchSize, "", insertPos);

    auto outPatchStartVal = ConstantInt::get(Type::getInt32Ty(*m_context), outPatchStart);
    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, outPatchStartVal, "", insertPos);

    auto outVertexStride = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.outVertexStride);
    ldsOffset = BinaryOperator::CreateAdd(
        ldsOffset, BinaryOperator::CreateMul(vertexIdx, outVertexStride, "", insertPos), "", insertPos);

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attribOffset, "", insertPos);
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
// @param insertPos : Where to insert calculation instructions
Value *PatchInOutImportExport::calcLdsOffsetForTesInput(Type *inputTy, unsigned location, Value *locOffset,
                                                        Value *compIdx, Value *vertexIdx, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessEval);

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;

  auto outPatchStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.outPatchStart : calcFactor.onChip.outPatchStart;

  auto patchConstStart =
      m_pipelineState->isTessOffChip() ? calcFactor.offChip.patchConstStart : calcFactor.onChip.patchConstStart;

  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tes;

  auto relPatchId = getFunctionArgument(m_entryPoint, entryArgIdxs.relPatchId);

  // attribOffset = (location + locOffset) * 4 + compIdx
  Value *attribOffset = ConstantInt::get(Type::getInt32Ty(*m_context), location);

  if (locOffset)
    attribOffset = BinaryOperator::CreateAdd(attribOffset, locOffset, "", insertPos);

  attribOffset =
      BinaryOperator::CreateMul(attribOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  if (compIdx) {
    const unsigned bitWidth = inputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = BinaryOperator::CreateMul(compIdx, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    }

    attribOffset = BinaryOperator::CreateAdd(attribOffset, compIdx, "", insertPos);
  }

  Value *ldsOffset = nullptr;

  const bool perPatch = (!vertexIdx); // Vertex indexing is unavailable for per-patch input
  if (perPatch) {
    // dwordOffset = patchConstStart + relPatchId * patchConstSize + attribOffset
    auto patchConstSize = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.patchConstSize);
    ldsOffset = BinaryOperator::CreateMul(relPatchId, patchConstSize, "", insertPos);

    auto patchConstStartVal = ConstantInt::get(Type::getInt32Ty(*m_context), patchConstStart);
    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, patchConstStartVal, "", insertPos);

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attribOffset, "", insertPos);
  } else {
    // dwordOffset = patchStart + (relPatchId * vertexCount + vertexId) * vertexStride + attribOffset
    //             = patchStart + relPatchId * patchSize + vertexId  * vertexStride + attribOffset
    auto patchSize = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.outPatchSize);
    ldsOffset = BinaryOperator::CreateMul(relPatchId, patchSize, "", insertPos);

    auto patchStart = ConstantInt::get(Type::getInt32Ty(*m_context), outPatchStart);
    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, patchStart, "", insertPos);

    auto vertexStride = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.outVertexStride);
    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, BinaryOperator::CreateMul(vertexIdx, vertexStride, "", insertPos),
                                          "", insertPos);

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attribOffset, "", insertPos);
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

  const unsigned maxThreadCountPerPatch = std::max(inVertexCount, outVertexCount);
  const unsigned patchCountLimitedByThread = maxThreadCountPerThreadGroup / maxThreadCountPerPatch;

  const unsigned inPatchSize = (inVertexCount * inVertexStride);
  const unsigned outPatchSize = (outVertexCount * outVertexStride);
  const unsigned patchConstSize = patchConstCount * 4;

  // Compute the required LDS size per patch, always include the space for input patch and tess factor
  unsigned ldsSizePerPatch = inPatchSize + MaxTessFactorsPerPatch;

  unsigned ldsSizePerThreadGroup = m_pipelineState->getTargetInfo().getGpuProperty().ldsSizePerThreadGroup;
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

  auto undef = UndefValue::get(Type::getFloatTy(*m_context));
  if (numChannels <= 4) {
    assert(startChannel + numChannels <= 4);

    Value *attribValues[4] = {undef, undef, undef, undef};
    for (unsigned i = startChannel; i < startChannel + numChannels; ++i)
      attribValues[i] = exportValues[i - startChannel];

    m_expLocs.insert(location);
    recordVertexAttribExport(location, {attribValues[0], attribValues[1], attribValues[2], attribValues[3]});
  } else {
    // We have to do exporting twice for this output
    assert(startChannel == 0); // Other values are disallowed according to GLSL spec
    assert(numChannels == 6 || numChannels == 8);

    Value *attribValues[8] = {undef, undef, undef, undef, undef, undef, undef, undef};
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

  const auto undef = UndefValue::get(Type::getFloatTy(*m_context));

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
        undef,                                                            // src1
        undef,                                                            // src2
        undef,                                                            // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
    };
    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    break;
  }
  case BuiltInLayer: {
    assert(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_Layer are packed

    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

    Value *layer = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);

    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0x4),              // en
        undef,                                                            // src0
        undef,                                                            // src1
        layer,                                                            // src2
        undef,                                                            // src3
        ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
        ConstantInt::get(Type::getInt1Ty(*m_context), false)              // vm
    };
    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

    // NOTE: We have to export gl_Layer via generic outputs as well.
    if (nextStage == ShaderStageFragment) {
      const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
      unsigned loc = InvalidValue;
      Value *layer = nullptr;
      if (nextBuiltInUsage.layer) {
        assert(builtInOutLocs.find(BuiltInLayer) != builtInOutLocs.end());
        loc = builtInOutLocs.find(BuiltInLayer)->second;
        layer = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);
      }

      if (nextBuiltInUsage.viewIndex) {
        assert(builtInOutLocs.find(BuiltInViewIndex) != builtInOutLocs.end());
        loc = builtInOutLocs.find(BuiltInViewIndex)->second;

        if (enableMultiView)
          layer = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);
        else
          layer = ConstantFP::get(Type::getFloatTy(*m_context), 0.0f);
      }
      if (layer)
        recordVertexAttribExport(loc, {layer, undef, undef, undef});
    }

    break;
  }
  case BuiltInViewportIndex: {
    assert(m_gfxIp.major <= 8); // For GFX9, gl_ViewportIndex and gl_Layer are packed
    Value *viewportIndex = new BitCastInst(output, Type::getFloatTy(*m_context), "", insertPos);

    Value *args[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_POS_1), // tgt
        ConstantInt::get(Type::getInt32Ty(*m_context), 0x8),              // en
        undef,                                                            // src0
        undef,                                                            // src1
        undef,                                                            // src2
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

      recordVertexAttribExport(loc, {viewportIndex, undef, undef, undef});
    }

    break;
  }
  case BuiltInPrimitiveShadingRate: {
    // gl_PrimitiveShadingRate is not supported on pre-GFX10.3
    assert(m_gfxIp >= GfxIpVersion({10, 3}));

    exportShadingRate(output, insertPos);
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
// @param insertPos : Where to insert this call
Value *PatchInOutImportExport::adjustCentroidIj(Value *centroidIj, Value *centerIj, Instruction *insertPos) {
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
  Value *ij = nullptr;

  if (builtInUsage.centroid && builtInUsage.center) {
    // NOTE: If both centroid and center are enabled, centroid I/J provided by hardware natively may be invalid. We have
    // to adjust it with center I/J on condition of bc_optimize flag. bc_optimize = pPrimMask[31], when bc_optimize is
    // on, pPrimMask is less than zero
    auto cond =
        new ICmpInst(insertPos, ICmpInst::ICMP_SLT, primMask, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "");
    ij = SelectInst::Create(cond, centerIj, centroidIj, "", insertPos);
  } else
    ij = centroidIj;

  return ij;
}

// =====================================================================================================================
// Get Subgroup local invocation Id
//
// @param builder : The builder to use
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
// Do automatic workgroup size reconfiguration in a compute shader, to allow ReconfigWorkgroup
// to apply optimizations.
WorkgroupLayout PatchInOutImportExport::calculateWorkgroupLayout() {
  auto &resUsage = *m_pipelineState->getShaderResourceUsage(ShaderStageCompute);
  if (m_shaderStage == ShaderStageCompute) {
    bool reconfig = false;

    switch (static_cast<WorkgroupLayout>(resUsage.builtInUsage.cs.workgroupLayout)) {
    case WorkgroupLayout::Unknown:
      // If no configuration has been specified, apply a reconfigure if the compute shader uses images and the
      // pipeline option was enabled.
      if (resUsage.useImages)
        reconfig = m_pipelineState->getOptions().reconfigWorkgroupLayout;
      break;
    case WorkgroupLayout::Linear:
      // The hardware by default applies the linear rules, so just ban reconfigure and we're done.
      reconfig = false;
      break;
    case WorkgroupLayout::Quads:
      // 2x2 requested.
      reconfig = true;
      break;
    case WorkgroupLayout::SexagintiQuads:
      // 8x8 requested.
      reconfig = true;
      break;
    }

    if (reconfig) {
      auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
      if ((mode.workgroupSizeX % 2) == 0 && (mode.workgroupSizeY % 2) == 0) {
        if ((mode.workgroupSizeX > 8 && mode.workgroupSizeY >= 8) ||
            (mode.workgroupSizeX >= 8 && mode.workgroupSizeY > 8)) {
          // If our local size in the X & Y dimensions are greater than 8, we can reconfigure.
          resUsage.builtInUsage.cs.workgroupLayout = static_cast<unsigned>(WorkgroupLayout::SexagintiQuads);
        } else {
          // If our local size in the X & Y dimensions are multiples of 2, we can reconfigure.
          resUsage.builtInUsage.cs.workgroupLayout = static_cast<unsigned>(WorkgroupLayout::Quads);
        }
      }
    }
  }
  return static_cast<WorkgroupLayout>(resUsage.builtInUsage.cs.workgroupLayout);
}

// =====================================================================================================================
// Reconfigure the workgroup for optimization purposes.
//
// @param localInvocationId : The original workgroup ID.
// @param insertPos : Where to insert instructions.
Value *PatchInOutImportExport::reconfigWorkgroup(Value *localInvocationId, Instruction *insertPos) {
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCompute)->builtInUsage.cs;
  auto workgroupLayout = static_cast<WorkgroupLayout>(builtInUsage.workgroupLayout);
  auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();

  // NOTE: Here, we implement "GDC 2018 Engine Optimization Hot Lap Workgroup Optimization " (slides 40-45, by
  // Timothy Lottes).
  // uvec2 Remap(uint a) {
  //   uint y = bitfieldExtract(a,3,4); // v_bfe_u32 ---> {...0,y3,y2,y1,x2}
  //   y = bitfieldInsert(y,a,0,1);     // v_bfi_b32 ---> {...0,y3,y2,y1,y0}
  //   uint x = bitfieldExtract(a,1,3); // v_bfe_u32 ---> {...0,x2,x1,x0}
  //   a = bitfieldExtract(a,4,5);      // v_bfe_u32 ---> {...0,x4,x3,y3,y2,y1}
  //   x = bitfieldInsert(a,x,0,3);     // v_bfi_b32 ---> {...0,x4,x3,x2,x1,x0}
  //   return uvec2(x, y);
  // }
  // usage in shader
  //   uvec2 xy = Remap(gl_LocalInvocationID.x);
  //   xy.x += gl_WorkGroupID.x << 5; // v_lshl_add_u32
  //   xy.y += gl_WorkGroupID.y << 4; // v_lshl_add_u32

  Type *const int16Ty = Type::getInt16Ty(*m_context);
  Type *const int32Ty = Type::getInt32Ty(*m_context);

  Value *remappedId = localInvocationId;

  // For a reconfigured workgroup, we map Y -> Z
  if (mode.workgroupSizeZ > 1) {
    Constant *shuffleMask[] = {ConstantInt::get(int32Ty, 0), UndefValue::get(int32Ty), ConstantInt::get(int32Ty, 1)};

    remappedId = new ShuffleVectorInst(remappedId, UndefValue::get(remappedId->getType()),
                                       ConstantVector::get(shuffleMask), "", insertPos);
  } else {
    remappedId = InsertElementInst::Create(remappedId, ConstantInt::get(int32Ty, 0), ConstantInt::get(int32Ty, 2), "",
                                           insertPos);
  }

  Instruction *const x = ExtractElementInst::Create(remappedId, ConstantInt::get(int32Ty, 0), "", insertPos);

  Instruction *const bit0 = BinaryOperator::CreateAnd(x, ConstantInt::get(int32Ty, 0x1), "", insertPos);

  Instruction *bit1 = BinaryOperator::CreateAnd(x, ConstantInt::get(int32Ty, 0x2), "", insertPos);
  bit1 = BinaryOperator::CreateLShr(bit1, ConstantInt::get(int32Ty, 1), "", insertPos);

  Instruction *offset = nullptr;
  Instruction *maskedX = x;

  // Check if we are doing 8x8, as we need to calculate an offset and mask out the top bits of X if so.
  if (workgroupLayout == WorkgroupLayout::SexagintiQuads) {
    const unsigned workgroupSizeYMul8 = mode.workgroupSizeY * 8;

    if (isPowerOf2_32(workgroupSizeYMul8)) {
      // If we have a power of two, we can use a right shift to compute the division more efficiently.
      offset = BinaryOperator::CreateLShr(x, ConstantInt::get(int32Ty, log2(workgroupSizeYMul8)), "", insertPos);
    } else {
      // Otherwise we truncate down to a 16-bit integer, do the division, and zero extend. This will
      // result in significantly less instructions to do the divide.
      offset = CastInst::CreateIntegerCast(x, int16Ty, false, "", insertPos);

      offset = BinaryOperator::CreateUDiv(offset, ConstantInt::get(int16Ty, workgroupSizeYMul8), "", insertPos);

      offset = CastInst::CreateIntegerCast(offset, int32Ty, false, "", insertPos);
    }

    Instruction *const mulOffset =
        BinaryOperator::CreateMul(offset, ConstantInt::get(int32Ty, workgroupSizeYMul8), "", insertPos);

    maskedX = BinaryOperator::CreateSub(x, mulOffset, "", insertPos);
  }

  Instruction *const remainingBits = BinaryOperator::CreateAnd(maskedX, ConstantInt::get(int32Ty, ~0x3), "", insertPos);

  Instruction *div = nullptr;
  Instruction *rem = nullptr;

  if (offset) {
    if ((mode.workgroupSizeX % 8) == 0 && (mode.workgroupSizeY % 8) == 0) {
      // Divide by 16.
      div = BinaryOperator::CreateLShr(remainingBits, ConstantInt::get(int32Ty, 4), "", insertPos);

      // Multiply by 16.
      rem = BinaryOperator::CreateShl(div, ConstantInt::get(int32Ty, 4), "", insertPos);

      // Subtract to get remainder.
      rem = BinaryOperator::CreateSub(remainingBits, rem, "", insertPos);
    } else {
      // Multiply by 8.
      Instruction *divideBy = BinaryOperator::CreateShl(offset, ConstantInt::get(int32Ty, 3), "", insertPos);

      divideBy = BinaryOperator::CreateSub(ConstantInt::get(int32Ty, mode.workgroupSizeX), divideBy, "", insertPos);

      Instruction *const cond = new ICmpInst(insertPos, ICmpInst::ICMP_ULT, divideBy, ConstantInt::get(int32Ty, 8), "");

      // We do a minimum operation to ensure that we never divide by more than 8, which forces our
      // workgroup layout into 8x8 tiles.
      divideBy = SelectInst::Create(cond, divideBy, ConstantInt::get(int32Ty, 8), "", insertPos);

      // Multiply by 2.
      divideBy = BinaryOperator::CreateShl(divideBy, ConstantInt::get(int32Ty, 1), "", insertPos);

      Instruction *const divideByTrunc = CastInst::CreateIntegerCast(divideBy, int16Ty, false, "", insertPos);

      // Truncate down to a 16-bit integer, do the division, and zero extend.
      div = CastInst::CreateIntegerCast(maskedX, int16Ty, false, "", insertPos);

      div = BinaryOperator::CreateUDiv(div, divideByTrunc, "", insertPos);

      div = CastInst::CreateIntegerCast(div, int32Ty, false, "", insertPos);

      Instruction *const mulDiv = BinaryOperator::CreateMul(div, divideBy, "", insertPos);

      rem = BinaryOperator::CreateSub(remainingBits, mulDiv, "", insertPos);
    }
  } else {
    const unsigned workgroupSizeXMul2 = mode.workgroupSizeX * 2;

    if (isPowerOf2_32(workgroupSizeXMul2)) {
      // If we have a power of two, we can use a right shift to compute the division more efficiently.
      div = BinaryOperator::CreateLShr(maskedX, ConstantInt::get(int32Ty, log2(workgroupSizeXMul2)), "", insertPos);
    } else {
      // Otherwise we truncate down to a 16-bit integer, do the division, and zero extend. This will
      // result in significantly less instructions to do the divide.
      div = CastInst::CreateIntegerCast(maskedX, int16Ty, false, "", insertPos);

      div = BinaryOperator::CreateUDiv(div, ConstantInt::get(int16Ty, workgroupSizeXMul2), "", insertPos);

      div = CastInst::CreateIntegerCast(div, int32Ty, false, "", insertPos);
    }

    Instruction *const mulDiv =
        BinaryOperator::CreateMul(div, ConstantInt::get(int32Ty, workgroupSizeXMul2), "", insertPos);

    rem = BinaryOperator::CreateSub(remainingBits, mulDiv, "", insertPos);
  }

  // Now we have all the components to reconstruct X & Y!
  Instruction *newX = BinaryOperator::CreateLShr(rem, ConstantInt::get(int32Ty, 1), "", insertPos);

  newX = BinaryOperator::CreateAdd(newX, bit0, "", insertPos);

  // If we have an offset, we need to incorporate this into X.
  if (offset) {
    const unsigned workgroupSizeYMin8 = std::min(mode.workgroupSizeY, 8u);
    Instruction *const mul =
        BinaryOperator::CreateMul(offset, ConstantInt::get(int32Ty, workgroupSizeYMin8), "", insertPos);

    newX = BinaryOperator::CreateAdd(newX, mul, "", insertPos);
  }

  remappedId = InsertElementInst::Create(remappedId, newX, ConstantInt::get(int32Ty, 0), "", insertPos);

  Instruction *newY = BinaryOperator::CreateShl(div, ConstantInt::get(int32Ty, 1), "", insertPos);

  newY = BinaryOperator::CreateAdd(newY, bit1, "", insertPos);

  remappedId = InsertElementInst::Create(remappedId, newY, ConstantInt::get(int32Ty, 1), "", insertPos);

  return remappedId;
}

// =====================================================================================================================
// This function adds instructions to swizzle thread ID inside a group.
//
// The data layout in TCP is typically 8 x something.
// The typewriter style SIMD layout does not match well with the data layout,
// which will potentially cause partial cache line read/write.
//
// In the case of 16x16 thread group size, wf32 SIMD will be created in eight 16x2 regions.
// This function maps the eight 16x2 regions into eight 8x4 regions.
//
// Originally SIMDs cover 16x2 regions, after swizzling they cover 8x4 regions.
//
// @param localInvocationId : The original workgroup ID.
// @param insertPos : Where to insert instructions.
Value *PatchInOutImportExport::swizzleLocalInvocationIdIn8x4(Value *localInvocationId, Instruction *insertPos) {
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);
  auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();

  const unsigned workgroupSizeX = mode.workgroupSizeX;
  const unsigned workgroupSizeY = mode.workgroupSizeY;

  if (workgroupSizeX < 16)
    return localInvocationId;

  if (workgroupSizeX % 8 != 0)
    return localInvocationId;

  if (workgroupSizeY % 4 != 0)
    return localInvocationId;

  if ((workgroupSizeX >= 16) && (workgroupSizeX % 8 == 0) && (workgroupSizeY % 4 == 0)) {
    Value *threadIdInGroupX = builder.CreateExtractElement(localInvocationId, (uint64_t)0);
    Value *threadIdInGroupY = builder.CreateExtractElement(localInvocationId, 1);
    Value *threadIdInGroupZ = builder.CreateExtractElement(localInvocationId, 2);
    const unsigned blockSizeX = 8;
    const unsigned blockSizeY = 4;
    const unsigned blockDimY = workgroupSizeY / 4; // How many blocks are split in Y level.

    const unsigned blockSize = blockSizeX * blockSizeY; // 32 threads per-block

    Value *linearId = builder.CreateMul(threadIdInGroupY, builder.getInt32(workgroupSizeX));
    linearId = builder.CreateAdd(linearId, threadIdInGroupX);

    Value *waveId = builder.CreateUDiv(linearId, builder.getInt32(blockSize));
    Value *waveLinearId = builder.CreateURem(linearId, builder.getInt32(blockSize));
    Value *blockOffsX = builder.CreateUDiv(waveId, builder.getInt32(blockDimY));
    Value *blockOffsY = builder.CreateURem(waveId, builder.getInt32(blockDimY));

    Value *blockThreadIdX = builder.CreateURem(waveLinearId, builder.getInt32(blockSizeX));
    Value *blockThreadIdY = builder.CreateUDiv(waveLinearId, builder.getInt32(blockSizeX));

    Value *newThreadIdInGroupX = builder.CreateMul(blockOffsX, builder.getInt32(blockSizeX));
    newThreadIdInGroupX = builder.CreateAdd(newThreadIdInGroupX, blockThreadIdX);
    Value *newThreadIdInGroupY = builder.CreateMul(blockOffsY, builder.getInt32(blockSizeY));
    newThreadIdInGroupY = builder.CreateAdd(newThreadIdInGroupY, blockThreadIdY);

    Value *newThreadIdInGroupZ = threadIdInGroupZ;
    Value *newThreadIdInGroup = PoisonValue::get(FixedVectorType::get(builder.getInt32Ty(), 3));
    newThreadIdInGroup = builder.CreateInsertElement(newThreadIdInGroup, newThreadIdInGroupX, uint64_t(0));
    newThreadIdInGroup = builder.CreateInsertElement(newThreadIdInGroup, newThreadIdInGroupY, 1);
    newThreadIdInGroup = builder.CreateInsertElement(newThreadIdInGroup, newThreadIdInGroupZ, 2);
    return newThreadIdInGroup;
  }
  return localInvocationId;
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
  func->addFnAttr(Attribute::ReadNone);
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
          UndefValue::get(ivec3Ty), builder.CreateUDiv(localThreadGroupFlatId, bottomHeight), uint64_t(0));
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
          UndefValue::get(ivec3Ty),
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
          builder.CreateInsertElement(UndefValue::get(ivec3Ty), swizzledWorkgroupIdX, uint64_t(0));
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
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);

  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  Value* hwShadingRate = nullptr;

  {
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

  auto undef = UndefValue::get(builder.getFloatTy());
  // "Done" flag is valid for exporting position 0 ~ 3
  builder.CreateIntrinsic(Intrinsic::amdgcn_exp, builder.getFloatTy(),
                          {builder.getInt32(EXP_TARGET_POS_1), // tgt
                           builder.getInt32(0x2),              // en
                           undef,                              // src0
                           hwShadingRate,                      // src1
                           undef,                              // src2
                           undef,                              // src3
                           builder.getFalse(),                 // done
                           builder.getFalse()});               // src0
}

// =====================================================================================================================
// Gets HW shading rate and converts them to LGC definitions.
//
// @param insertPos : Where to insert instructions.
Value *PatchInOutImportExport::getShadingRate(Instruction *insertPos) {
  IRBuilder<> builder(*m_context);
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

  {
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

  auto undef = UndefValue::get(Type::getFloatTy(*m_context));

  // Vertex attribute not existing, insert a new one and initialize it
  if (m_attribExports.count(location) == 0) {
    for (unsigned i = 0; i < 4; ++i)
      m_attribExports[location][i] = undef;
  }

  for (unsigned i = 0; i < 4; ++i) {
    assert(attribValues[i]);
    if (isa<UndefValue>(attribValues[i]))
      continue; // Here, we only record new attribute values that are valid (not undefined ones)

    // NOTE: The existing values must have been initialized to undefined ones already. Overlapping is disallowed (see
    // such cases):
    //   - Valid:
    //       Existing: attrib0, <1.0, 2.0, undef, undef>
    //       New:      attrib0, <undef, undef, 3.0, 4.0>
    //   - Invalid:
    //       Existing: attrib0, <1.0, 2.0, 3.0, undef>
    //       New:      attrib0, <undef, undef, 4.0, 5.0>
    assert(isa<UndefValue>(m_attribExports[location][i]));
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

  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);

  for (auto &attribExport : m_attribExports) {
    if (m_gfxIp.major <= 10) {
      unsigned channelMask = 0;
      for (unsigned i = 0; i < 4; ++i) {
        assert(attribExport.second[i]);
        if (!isa<UndefValue>(attribExport.second[i]))
          channelMask |= (1u << i); // Update channel mask if the value is valid (not undef)
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
      llvm_unreachable("Not implemented!");
    }
  }
}

// =====================================================================================================================
// Handle the store of tessellation factors.
//   1. Collect outer and inner tessellation factors;
//   2. Write tessellation factors to LDS if they are read as inputs by TES;
//   3. Write tessellation factors to TF buffer.
void PatchInOutImportExport::storeTessFactors() {
  assert(m_shaderStage == ShaderStageTessControl); // Must be tessellation control shader

  //
  // Find the ret instruction as the insert position
  //
  Instruction *insertPos = nullptr;
  for (auto &block : *m_entryPoint) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      assert(retInst->getType()->isVoidTy());
      insertPos = retInst;
    }
  }
  assert(insertPos); // Must have ret instruction

  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);

  //
  // Collect tessellation factors from on-chip LDS
  //
  unsigned outerTessFactorCount = 0;
  unsigned innerTessFactorCount = 0;

  const auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
  switch (primitiveMode) {
  case PrimitiveMode::Triangles:
    outerTessFactorCount = 3;
    innerTessFactorCount = 1;
    break;
  case PrimitiveMode::Quads:
    outerTessFactorCount = 4;
    innerTessFactorCount = 2;
    break;
  case PrimitiveMode::Isolines:
    outerTessFactorCount = 2;
    innerTessFactorCount = 0;
    break;
  default:
    llvm_unreachable("Unknown primitive mode!");
    break;
  }

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();

  // NOTE: We are going to read back tess factors from on-chip LDS. Make sure they have been stored already.
  SyncScope::ID workgroupScope = m_context->getOrInsertSyncScopeID("workgroup");
  builder.CreateFence(AtomicOrdering::Release, workgroupScope);
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
  builder.CreateFence(AtomicOrdering::Acquire, workgroupScope);

  SmallVector<Value *> outerTessFactors, innerTessFactors;

  assert(outerTessFactorCount >= 2 && outerTessFactorCount <= 4);
  // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch
  Value *ldsOffset = builder.CreateMul(relativeId, builder.getInt32(MaxTessFactorsPerPatch));
  ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(calcFactor.onChip.tessFactorStart));
  auto outerTessFactorVec =
      readValueFromLds(false, FixedVectorType::get(builder.getFloatTy(), outerTessFactorCount), ldsOffset, insertPos);
  for (unsigned i = 0; i < outerTessFactorCount; ++i)
    outerTessFactors.push_back(builder.CreateExtractElement(outerTessFactorVec, i));

  assert(innerTessFactorCount <= 2);
  if (innerTessFactorCount > 0) {
    // ldsOffset = tessFactorStart + relativeId * MaxTessFactorsPerPatch + 4
    Value *ldsOffset = builder.CreateMul(relativeId, builder.getInt32(MaxTessFactorsPerPatch));
    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(calcFactor.onChip.tessFactorStart + 4));
    auto innerTessFactorVec =
        readValueFromLds(false, FixedVectorType::get(builder.getFloatTy(), innerTessFactorCount), ldsOffset, insertPos);
    for (unsigned i = 0; i < innerTessFactorCount; ++i)
      innerTessFactors.push_back(builder.CreateExtractElement(innerTessFactorVec, i));
  }

  {
    //
    // Write tessellation factors to LDS if they are read as inputs by TES
    //
    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
    const auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;

    auto tessLevelOuterLocIt = perPatchBuiltInOutLocMap.find(BuiltInTessLevelOuter);
    if (tessLevelOuterLocIt != perPatchBuiltInOutLocMap.end()) {
      const unsigned loc = tessLevelOuterLocIt->second;

      for (unsigned i = 0; i < outerTessFactorCount; ++i) {
        auto ldsOffset =
            calcLdsOffsetForTcsOutput(builder.getFloatTy(), loc, nullptr, builder.getInt32(i), nullptr, insertPos);
        writeValueToLds(m_pipelineState->isTessOffChip(), outerTessFactors[i], ldsOffset, insertPos);
      }
    }

    auto tessLevelInnerLocIt = perPatchBuiltInOutLocMap.find(BuiltInTessLevelInner);
    if (tessLevelInnerLocIt != perPatchBuiltInOutLocMap.end()) {
      const unsigned loc = tessLevelInnerLocIt->second;

      for (unsigned i = 0; i < innerTessFactorCount; ++i) {
        auto ldsOffset =
            calcLdsOffsetForTcsOutput(builder.getFloatTy(), loc, nullptr, builder.getInt32(i), nullptr, insertPos);
        writeValueToLds(m_pipelineState->isTessOffChip(), innerTessFactors[i], ldsOffset, insertPos);
      }
    }

    // NOTE: For isoline, the outer tessellation factors have to be exchanged, which is required by HW.
    if (primitiveMode == PrimitiveMode::Isolines)
      std::swap(outerTessFactors[0], outerTessFactors[1]);

    storeTessFactorToBuffer(outerTessFactors, innerTessFactors, insertPos);
  }
}

// =====================================================================================================================
// Write the collected tessellation factors to TF buffer.
//
// @param outerTessFactors : The collected outer tessellation factors
// @param innerTessFactors : The collected inner tessellation factors
// @param insertPos : Where to insert instructions
void PatchInOutImportExport::storeTessFactorToBuffer(ArrayRef<Value *> outerTessFactors,
                                                     ArrayRef<Value *> innerTessFactors, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessControl);

  // NOTE: Tessellation factors are from tessellation level array and we have:
  //   (1) Isoline
  //      tessFactor[0] = gl_TessLevelOuter[1]
  //      tessFactor[1] = gl_TessLevelOuter[0]
  //   (2) Triangle
  //      tessFactor[0] = gl_TessLevelOuter[0]
  //      tessFactor[1] = gl_TessLevelOuter[1]
  //      tessFactor[2] = gl_TessLevelOuter[2]
  //      tessFactor[3] = gl_TessLevelInner[0]
  //   (3) Quad
  //      tessFactor[0] = gl_TessLevelOuter[0]
  //      tessFactor[1] = gl_TessLevelOuter[1]
  //      tessFactor[2] = gl_TessLevelOuter[2]
  //      tessFactor[3] = gl_TessLevelOuter[3]
  //      tessFactor[4] = gl_TessLevelInner[0]
  //      tessFactor[5] = gl_TessLevelInner[1]

  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);

  Value *tfBufferDesc = m_pipelineSysValues.get(m_entryPoint)->getTessFactorBufDesc();

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl)->entryArgIdxs.tcs;
  Value *tfBufferBase = getFunctionArgument(m_entryPoint, entryArgIdxs.tfBufferBase);
  if (m_pipelineState->isTessOffChip()) {
    if (m_gfxIp.major <= 8) {
      // NOTE: Additional 4-byte offset is required for tessellation off-chip mode (pre-GFX9).
      tfBufferBase =
          BinaryOperator::CreateAdd(tfBufferBase, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);
    }
  }

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  auto relPatchId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
  Value *tfBufferOffset = builder.CreateMul(relPatchId, builder.getInt32(calcFactor.tessFactorStride * sizeof(float)));

  CoherentFlag coherent = {};
  coherent.bits.glc = true;

  auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
  if (primitiveMode == PrimitiveMode::Isolines || primitiveMode == PrimitiveMode::Triangles) {
    // For isoline and triangle, we can combine outer tessellation factors with inner ones
    FixedVectorType *tfValueTy =
        FixedVectorType::get(builder.getFloatTy(), outerTessFactors.size() + innerTessFactors.size());
    Value *tfValue = UndefValue::get(tfValueTy);

    assert(outerTessFactors.size() == 2 || outerTessFactors.size() == 3);
    for (unsigned i = 0; i < outerTessFactors.size(); ++i)
      tfValue = builder.CreateInsertElement(tfValue, outerTessFactors[i], i);

    assert(innerTessFactors.size() == 0 || innerTessFactors.size() == 1);
    for (unsigned i = 0; i < innerTessFactors.size(); ++i)
      tfValue = builder.CreateInsertElement(tfValue, innerTessFactors[i], outerTessFactors.size() + i);

    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, tfValueTy,
                            {tfValue,                                                             // vdata
                             tfBufferDesc,                                                        // rsrc
                             tfBufferOffset,                                                      // voffset
                             tfBufferBase,                                                        // soffset
                             builder.getInt32((*m_buffFormats)[tfValueTy->getNumElements() - 1]), // format
                             builder.getInt32(coherent.u32All)});                                 // glc
  } else {
    assert(primitiveMode == PrimitiveMode::Quads);

    FixedVectorType *outerTfValueTy = FixedVectorType::get(builder.getFloatTy(), outerTessFactors.size());
    Value *outerTfValue = UndefValue::get(outerTfValueTy);

    assert(outerTessFactors.size() == 4);
    for (unsigned i = 0; i < outerTessFactors.size(); ++i)
      outerTfValue = builder.CreateInsertElement(outerTfValue, outerTessFactors[i], i);

    FixedVectorType *innerTfValueTy = FixedVectorType::get(builder.getFloatTy(), innerTessFactors.size());
    Value *innerTfValue = UndefValue::get(innerTfValueTy);

    assert(innerTessFactors.size() == 2);
    for (unsigned i = 0; i < innerTessFactors.size(); ++i)
      innerTfValue = builder.CreateInsertElement(innerTfValue, innerTessFactors[i], i);

    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outerTfValueTy,
                            {outerTfValue,                                                             // vdata
                             tfBufferDesc,                                                             // rsrc
                             tfBufferOffset,                                                           // voffset
                             tfBufferBase,                                                             // soffset
                             builder.getInt32((*m_buffFormats)[outerTfValueTy->getNumElements() - 1]), // format
                             builder.getInt32(coherent.u32All)});                                      // glc

    tfBufferOffset =
        builder.CreateAdd(tfBufferOffset, builder.getInt32(outerTfValueTy->getNumElements() * sizeof(float)));
    builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, innerTfValueTy,
                            {innerTfValue,                                                             // vdata
                             tfBufferDesc,                                                             // rsrc
                             tfBufferOffset,                                                           // voffset
                             tfBufferBase,                                                             // soffset
                             builder.getInt32((*m_buffFormats)[innerTfValueTy->getNumElements() - 1]), // format
                             builder.getInt32(coherent.u32All)});                                      // glc
  }
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for input import and output export.
INITIALIZE_PASS(LegacyPatchInOutImportExport, DEBUG_TYPE, "Patch LLVM for input import and output export operations",
                false, false)
