/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 ***********************************************************************************************************************
 */
#include "PatchInOutImportExport.h"
#include "BuilderDebug.h"
#include "BuilderImpl.h"
#include "FragColorExport.h"
#include "PipelineShaders.h"
#include "VertexFetch.h"
#include "lgc/BuilderBuiltIns.h"
#include "lgc/BuilderContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <unordered_set>

#define DEBUG_TYPE "llpc-patch-in-out-import-export"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchInOutImportExport::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for input import and output export
ModulePass *createPatchInOutImportExport() {
  return new PatchInOutImportExport();
}

// =====================================================================================================================
PatchInOutImportExport::PatchInOutImportExport() : Patch(ID), m_lds(nullptr) {
  memset(&m_gfxIp, 0, sizeof(m_gfxIp));
  initPerShader();
}

// =====================================================================================================================
PatchInOutImportExport::~PatchInOutImportExport() {
  assert(!m_fragColorExport);
  assert(!m_vertexFetch);
}

// =====================================================================================================================
// Initialize per-shader members
void PatchInOutImportExport::initPerShader() {
  m_vertexFetch = nullptr;
  m_fragColorExport = nullptr;
  m_lastExport = nullptr;
  m_clipDistance = nullptr;
  m_cullDistance = nullptr;
  m_primitiveId = nullptr;
  m_fragDepth = nullptr;
  m_fragStencilRef = nullptr;
  m_sampleMask = nullptr;
  m_viewportIndex = nullptr;
  m_layer = nullptr;
  m_threadId = nullptr;
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchInOutImportExport::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-In-Out-Import-Export\n");

  Patch::init(&module);

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  m_pipelineSysValues.initialize(m_pipelineState);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  m_hasTs = (stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0;
  m_hasGs = (stageMask & shaderStageToMask(ShaderStageGeometry)) != 0;

  // Create the global variable that is to model LDS
  // NOTE: ES -> GS ring is always on-chip on GFX9.
  if (m_hasTs || (m_hasGs && (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9)))
    m_lds = Patch::getLdsVariable(m_pipelineState, m_module);

  // Process each shader in turn, in reverse order (because for example VS uses inOutUsage.tcs.calcFactor
  // set by TCS).
  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  for (int shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage) {
    auto entryPoint = pipelineShaders->getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (entryPoint) {
      initPerShader();
      m_entryPoint = entryPoint;
      m_shaderStage = static_cast<ShaderStage>(shaderStage);
      processShader();

      // Now process the call and return instructions.
      visit(*m_entryPoint);
    }
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

  delete m_fragColorExport;
  m_fragColorExport = nullptr;

  delete m_vertexFetch;
  m_vertexFetch = nullptr;

  for (auto &fragColors : m_expFragColors)
    fragColors.clear();
  m_pipelineSysValues.clear();

  return true;
}

// =====================================================================================================================
// Process a single shader
void PatchInOutImportExport::processShader() {
  if (m_shaderStage == ShaderStageVertex) {
    // Create vertex fetch manager
    m_vertexFetch = new VertexFetch(m_entryPoint, m_pipelineSysValues.get(m_entryPoint), m_pipelineState);
  } else if (m_shaderStage == ShaderStageFragment) {
    // Create fragment color export manager
    m_fragColorExport = new FragColorExport(m_pipelineState, m_module);
  }

  // Initialize the output value for gl_PrimitiveID
  const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->builtInUsage;
  const auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs;
  if (m_shaderStage == ShaderStageVertex) {
    if (builtInUsage.vs.primitiveId)
      m_primitiveId = getFunctionArgument(m_entryPoint, entryArgIdxs.vs.primitiveId);
  } else if (m_shaderStage == ShaderStageTessEval) {
    if (builtInUsage.tes.primitiveId) {
      // TODO: Support tessellation shader.
      m_primitiveId = UndefValue::get(Type::getInt32Ty(*m_context));
    }
  }

  // Thread ID will be used in on-chip GS offset calculation (ES -> GS ring is always on-chip on GFX9)
  bool useThreadId = (m_hasGs && (m_pipelineState->isGsOnChip() || m_gfxIp.major >= 9));

  // Thread ID will also be used for stream-out buffer export
  const bool enableXfb = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.enableXfb;
  useThreadId = useThreadId || enableXfb;

  if (useThreadId) {
    // Calculate and store thread ID
    auto insertPos = m_entryPoint->begin()->getFirstInsertionPt();
    m_threadId = getSubgroupLocalInvocationId(&*insertPos);
  }

  // Initialize calculation factors for tessellation shader
  if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval) {
    const unsigned stageMask = m_pipelineState->getShaderStageMask();
    const bool hasTcs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0);

    auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
    if (calcFactor.inVertexStride == InvalidValue && calcFactor.outVertexStride == InvalidValue &&
        calcFactor.patchCountPerThreadGroup == InvalidValue && calcFactor.outPatchSize == InvalidValue &&
        calcFactor.patchConstSize == InvalidValue) {
      // NOTE: The LDS space is divided to three parts:
      //
      //              +----------------------------------------+
      //            / | TCS Vertex (Control Point) In (VS Out) |
      //           /  +----------------------------------------+
      //   LDS Space  | TCS Vertex (Control Point) Out         |
      //           \  +----------------------------------------+
      //            \ | TCS Patch Constant                     |
      //              +----------------------------------------+
      //
      // inPatchTotalSize = inVertexCount * inVertexStride * patchCountPerThreadGroup
      // outPatchTotalSize = outVertexCount * outVertexStride * patchCountPerThreadGroup
      // patchConstTotalSize = patchConstCount * 4 * patchCountPerThreadGroup

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

      calcFactor.outPatchSize = outPatchSize;
      calcFactor.inPatchSize = inPatchSize;

      calcFactor.onChip.outPatchStart = inPatchTotalSize;
      calcFactor.onChip.patchConstStart = inPatchTotalSize + outPatchTotalSize;

      if (m_pipelineState->isTessOffChip()) {
        calcFactor.offChip.outPatchStart = 0;
        calcFactor.offChip.patchConstStart = outPatchTotalSize;
      }

      calcFactor.tessFactorStride = tessFactorStride;

      LLPC_OUTS("===============================================================================\n");
      LLPC_OUTS("// LLPC tessellation calculation factor results\n\n");
      LLPC_OUTS("Patch count per thread group: " << calcFactor.patchCountPerThreadGroup << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Input vertex count: " << inVertexCount << "\n");
      LLPC_OUTS("Input vertex stride: " << calcFactor.inVertexStride << "\n");
      LLPC_OUTS("Input patch size: " << inPatchSize << "\n");
      LLPC_OUTS("Input patch total size: " << inPatchTotalSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Output vertex count: " << outVertexCount << "\n");
      LLPC_OUTS("Output vertex stride: " << calcFactor.outVertexStride << "\n");
      LLPC_OUTS("Output patch size: " << outPatchSize << "\n");
      LLPC_OUTS("Output patch total size: " << outPatchTotalSize << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Patch constant count: " << patchConstCount << "\n");
      LLPC_OUTS("Patch constant size: " << calcFactor.patchConstSize << "\n");
      LLPC_OUTS("Patch constant total size: " << calcFactor.patchConstSize * calcFactor.patchCountPerThreadGroup
                                              << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Tessellation factor stride: " << tessFactorStride << " (");
      switch (m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode) {
      case PrimitiveMode::Triangles:
        LLPC_OUTS("triangles");
        break;
      case PrimitiveMode::Quads:
        LLPC_OUTS("quads");
        tessFactorStride = 6;
        break;
      case PrimitiveMode::Isolines:
        LLPC_OUTS("isolines");
        tessFactorStride = 2;
        break;
      default:
        llvm_unreachable("Should never be called!");
        break;
      }
      LLPC_OUTS(")\n\n");
    }
  }
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void PatchInOutImportExport::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

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
        if (callInst.getNumArgOperands() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.getNumArgOperands() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);

        input = patchTcsBuiltInInputImport(inputTy, builtInId, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageTessEval: {
        // Builtin Call has different number of operands
        Value *elemIdx = nullptr;
        Value *vertexIdx = nullptr;
        if (callInst.getNumArgOperands() > 1)
          elemIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        if (callInst.getNumArgOperands() > 2)
          vertexIdx = isDontCareValue(callInst.getOperand(2)) ? nullptr : callInst.getOperand(2);
        input = patchTesBuiltInInputImport(inputTy, builtInId, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        // Builtin Call has different number of operands
        Value *vertexIdx = nullptr;
        if (callInst.getNumArgOperands() > 1)
          vertexIdx = isDontCareValue(callInst.getOperand(1)) ? nullptr : callInst.getOperand(1);

        input = patchGsBuiltInInputImport(inputTy, builtInId, vertexIdx, &callInst);
        break;
      }
      case ShaderStageFragment: {
        Value *sampleId = nullptr;
        if (callInst.getNumArgOperands() >= 2)
          sampleId = callInst.getArgOperand(1);
        input = patchFsBuiltInInputImport(inputTy, builtInId, sampleId, &callInst);
        break;
      }
      case ShaderStageCompute: {
        input = patchCsBuiltInInputImport(inputTy, builtInId, &callInst);
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

        if (m_shaderStage == ShaderStageTessEval) {
          // NOTE: For generic inputs of tessellation evaluation shader, they could be per-patch ones.
          if (resUsage->inOutUsage.inputLocMap.find(value) != resUsage->inOutUsage.inputLocMap.end())
            loc = resUsage->inOutUsage.inputLocMap[value];
          else {
            assert(resUsage->inOutUsage.perPatchInputLocMap.find(value) !=
                   resUsage->inOutUsage.perPatchInputLocMap.end());
            loc = resUsage->inOutUsage.perPatchInputLocMap[value];
          }
        } else {
          assert(resUsage->inOutUsage.inputLocMap.find(value) != resUsage->inOutUsage.inputLocMap.end());
          loc = resUsage->inOutUsage.inputLocMap[value];
        }
      }
      assert(loc != InvalidValue);

      switch (m_shaderStage) {
      case ShaderStageVertex: {
        assert(callInst.getNumArgOperands() == 2);
        const unsigned compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        input = patchVsGenericInputImport(inputTy, loc, compIdx, &callInst);
        break;
      }
      case ShaderStageTessControl: {
        assert(callInst.getNumArgOperands() == 4);

        auto elemIdx = callInst.getOperand(2);
        assert(isDontCareValue(elemIdx) == false);

        auto vertexIdx = callInst.getOperand(3);
        assert(isDontCareValue(vertexIdx) == false);

        input = patchTcsGenericInputImport(inputTy, loc, locOffset, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageTessEval: {
        assert(callInst.getNumArgOperands() == 4);

        auto elemIdx = callInst.getOperand(2);
        assert(isDontCareValue(elemIdx) == false);

        auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

        input = patchTesGenericInputImport(inputTy, loc, locOffset, elemIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        assert(callInst.getNumArgOperands() == 3);

        const unsigned compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();

        Value *vertexIdx = callInst.getOperand(2);
        assert(isDontCareValue(vertexIdx) == false);

        input = patchGsGenericInputImport(inputTy, loc, compIdx, vertexIdx, &callInst);
        break;
      }
      case ShaderStageFragment: {
        unsigned interpMode = InOutInfo::InterpModeSmooth;
        unsigned interpLoc = InOutInfo::InterpLocCenter;

        Value *elemIdx = callInst.getOperand(isInterpolantInputImport ? 2 : 1);
        assert(isDontCareValue(elemIdx) == false);

        Value *auxInterpValue = nullptr;

        if (isGenericInputImport) {
          assert(callInst.getNumArgOperands() == 4);

          interpMode = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
          interpLoc = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
        } else {
          assert(isInterpolantInputImport);
          assert(callInst.getNumArgOperands() == 5);

          interpMode = cast<ConstantInt>(callInst.getOperand(3))->getZExtValue();
          interpLoc = InOutInfo::InterpLocUnknown;

          auxInterpValue = callInst.getOperand(4);
        }

        input = patchFsGenericInputImport(inputTy, loc, locOffset, elemIdx, auxInterpValue, interpMode, interpLoc,
                                          &callInst);
        break;
      }
      case ShaderStageCompute: {
        llvm_unreachable("Should never be called!");
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

      assert(callInst.getNumArgOperands() == 3);
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
      if (resUsage->inOutUsage.outputLocMap.find(value) != resUsage->inOutUsage.outputLocMap.end())
        loc = resUsage->inOutUsage.outputLocMap[value];
      else {
        assert(resUsage->inOutUsage.perPatchOutputLocMap.find(value) !=
               resUsage->inOutUsage.perPatchOutputLocMap.end());
        loc = resUsage->inOutUsage.perPatchOutputLocMap[value];
      }
      assert(loc != InvalidValue);

      assert(callInst.getNumArgOperands() == 4);
      auto elemIdx = callInst.getOperand(2);
      assert(isDontCareValue(elemIdx) == false);
      auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

      output = patchTcsGenericOutputImport(outputTy, loc, locOffset, elemIdx, vertexIdx, &callInst);
    }

    callInst.replaceAllUsesWith(output);
  } else if (isExport) {
    // Output exports
    assert(isOutput);

    Value *output = callInst.getOperand(callInst.getNumArgOperands() - 1); // Last argument

    // Generic value (location or SPIR-V built-in ID or XFB buffer ID)
    unsigned value = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

    LLVM_DEBUG(dbgs() << "Find output export call: builtin = " << isBuiltInOutputExport << " value = " << value
                      << "\n");

    m_exportCalls.push_back(&callInst);

    if (isXfbOutputExport) {
      unsigned xfbBuffer = value;
      assert(xfbBuffer < MaxTransformFeedbackBuffers);

      unsigned xfbOffset = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
      unsigned xfbExtraOffset = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();

      // NOTE: Transform feedback output will be done in last vertex-processing shader stage.
      switch (m_shaderStage) {
      case ShaderStageVertex: {
        // No TS/GS pipeline, VS is the last stage
        if (!m_hasGs && !m_hasTs)
          patchXfbOutputExport(output, xfbBuffer, xfbOffset, xfbExtraOffset, &callInst);
        break;
      }
      case ShaderStageTessEval: {
        // TS-only pipeline, TES is the last stage
        if (!m_hasGs)
          patchXfbOutputExport(output, xfbBuffer, xfbOffset, xfbExtraOffset, &callInst);
        break;
      }
      case ShaderStageGeometry: {
        // Do nothing, transform feedback output is done in copy shader
        break;
      }
      case ShaderStageCopyShader: {
        // TS-GS or GS-only pipeline, copy shader is the last stage
        patchXfbOutputExport(output, xfbBuffer, xfbOffset, xfbExtraOffset, &callInst);
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
        assert(callInst.getNumArgOperands() == 4);
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
      case ShaderStageFragment: {
        patchFsBuiltInOutputExport(output, builtInId, &callInst);
        break;
      }
      case ShaderStageCopyShader: {
        patchCopyShaderBuiltInOutputExport(output, builtInId, &callInst);
        break;
      }
      case ShaderStageCompute: {
        llvm_unreachable("Should never be called!");
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

      if (m_shaderStage == ShaderStageTessControl) {
        // NOTE: If location offset is a constant, we have to add it to the unmapped location before querying
        // the mapped location. Meanwhile, we have to adjust the location offset to 0 (rebase it).
        locOffset = callInst.getOperand(1);
        if (isa<ConstantInt>(locOffset)) {
          value += cast<ConstantInt>(locOffset)->getZExtValue();
          locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
        }

        // NOTE: For generic outputs of tessellation control shader, they could be per-patch ones.
        if (resUsage->inOutUsage.outputLocMap.find(value) != resUsage->inOutUsage.outputLocMap.end()) {
          exist = true;
          loc = resUsage->inOutUsage.outputLocMap[value];
        } else if (resUsage->inOutUsage.perPatchOutputLocMap.find(value) !=
                   resUsage->inOutUsage.perPatchOutputLocMap.end()) {
          exist = true;
          loc = resUsage->inOutUsage.perPatchOutputLocMap[value];
        }
      } else if (m_shaderStage == ShaderStageCopyShader) {
        exist = true;
        loc = value;
      } else if (m_shaderStage == ShaderStageGeometry) {
        assert(callInst.getNumArgOperands() == 4);

        GsOutLocInfo outLocInfo = {};
        outLocInfo.location = value;
        outLocInfo.isBuiltIn = false;
        outLocInfo.streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();

        if (resUsage->inOutUsage.outputLocMap.find(outLocInfo.u32All) != resUsage->inOutUsage.outputLocMap.end()) {
          exist = true;
          loc = resUsage->inOutUsage.outputLocMap[outLocInfo.u32All];
        }
      } else {
        if (resUsage->inOutUsage.outputLocMap.find(value) != resUsage->inOutUsage.outputLocMap.end()) {
          exist = true;
          loc = resUsage->inOutUsage.outputLocMap[value];
        }
      }

      if (exist) {
        // NOTE: Some outputs are not used by next shader stage. They must have been removed already.
        assert(loc != InvalidValue);

        switch (m_shaderStage) {
        case ShaderStageVertex: {
          assert(callInst.getNumArgOperands() == 3);
          const unsigned compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          patchVsGenericOutputExport(output, loc, compIdx, &callInst);
          break;
        }
        case ShaderStageTessControl: {
          assert(callInst.getNumArgOperands() == 5);

          auto elemIdx = callInst.getOperand(2);
          assert(isDontCareValue(elemIdx) == false);

          auto vertexIdx = isDontCareValue(callInst.getOperand(3)) ? nullptr : callInst.getOperand(3);

          patchTcsGenericOutputExport(output, loc, locOffset, elemIdx, vertexIdx, &callInst);
          break;
        }
        case ShaderStageTessEval: {
          assert(callInst.getNumArgOperands() == 3);
          const unsigned compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          patchTesGenericOutputExport(output, loc, compIdx, &callInst);
          break;
        }
        case ShaderStageGeometry: {
          assert(callInst.getNumArgOperands() == 4);
          const unsigned compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          const unsigned streamId = cast<ConstantInt>(callInst.getOperand(2))->getZExtValue();
          patchGsGenericOutputExport(output, loc, compIdx, streamId, &callInst);
          break;
        }
        case ShaderStageFragment: {
          assert(callInst.getNumArgOperands() == 3);
          const unsigned compIdx = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
          patchFsGenericOutputExport(output, loc, compIdx, &callInst);
          break;
        }
        case ShaderStageCopyShader: {
          patchCopyShaderGenericOutputExport(output, loc, &callInst);
          break;
        }
        case ShaderStageCompute: {
          llvm_unreachable("Should never be called!");
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
        auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

        assert(builtInOutLocMap.find(BuiltInViewIndex) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap[BuiltInViewIndex];

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
        auto emitCounterPtr = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr()[emitStream];
        Value *emitCounter = new LoadInst(emitCounterPtr, "", &callInst);
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
  const bool enableXfb = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.enableXfb;

  // Whether this shader stage has to use "exp" instructions to export outputs
  const bool useExpInst = ((m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
                            (m_shaderStage == ShaderStageCopyShader && !enableXfb)) &&
                           (nextStage == ShaderStageInvalid || nextStage == ShaderStageFragment));

  auto zero = ConstantFP::get(Type::getFloatTy(*m_context), 0.0);
  auto one = ConstantFP::get(Type::getFloatTy(*m_context), 1.0);
  auto undef = UndefValue::get(Type::getFloatTy(*m_context));

  auto insertPos = &retInst;

  if (useExpInst) {
    bool usePosition = false;
    bool usePointSize = false;
    bool usePrimitiveId = false;
    bool useLayer = false;
    bool useViewportIndex = false;
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
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    }

    useLayer = enableMultiView || useLayer;

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
      m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
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

      // NOTE: When gl_PointSize, gl_Layer, or gl_ViewportIndex is used, gl_ClipDistance[] or gl_CullDistance[]
      // should start from pos2.
      unsigned pos = (usePointSize || useLayer || useViewportIndex) ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
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

      // "Done" flag is valid for exporting position 0 ~ 3
      m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

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
        m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
      }

      // NOTE: We have to export gl_ClipDistance[] or gl_CullDistancep[] via generic outputs as well.
      assert(nextStage == ShaderStageInvalid || nextStage == ShaderStageFragment);

      bool hasClipCullExport = true;
      if (nextStage == ShaderStageFragment) {
        const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

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
        unsigned loc = InvalidValue;
        if (m_shaderStage == ShaderStageCopyShader) {
          if (inOutUsage.gs.builtInOutLocs.find(BuiltInClipDistance) != inOutUsage.gs.builtInOutLocs.end())
            loc = inOutUsage.gs.builtInOutLocs[BuiltInClipDistance];
          else {
            assert(inOutUsage.gs.builtInOutLocs.find(BuiltInCullDistance) != inOutUsage.gs.builtInOutLocs.end());
            loc = inOutUsage.gs.builtInOutLocs[BuiltInCullDistance];
          }
        } else {
          if (inOutUsage.builtInOutputLocMap.find(BuiltInClipDistance) != inOutUsage.builtInOutputLocMap.end())
            loc = inOutUsage.builtInOutputLocMap[BuiltInClipDistance];
          else {
            assert(inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end());
            loc = inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
          }
        }

        Value *args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc), // tgt
            ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),                      // en
            clipCullDistance[0],                                                      // src0
            clipCullDistance[1],                                                      // src1
            clipCullDistance[2],                                                      // src2
            clipCullDistance[3],                                                      // src3
            ConstantInt::get(Type::getInt1Ty(*m_context), false),                     // done
            ConstantInt::get(Type::getInt1Ty(*m_context), false)                      // vm
        };
        emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
        ++inOutUsage.expCount;

        if (clipCullDistance.size() > 4) {
          // Do the second exporting
          Value *args[] = {
              ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc + 1), // tgt
              ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),                          // en
              clipCullDistance[4],                                                          // src0
              clipCullDistance[5],                                                          // src1
              clipCullDistance[6],                                                          // src2
              clipCullDistance[7],                                                          // src3
              ConstantInt::get(Type::getInt1Ty(*m_context), false),                         // done
              ConstantInt::get(Type::getInt1Ty(*m_context), false)                          // vm
          };
          emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
          ++inOutUsage.expCount;
        }
      }
    }

    // Export gl_PrimitiveID before entry-point returns
    if (usePrimitiveId) {
      bool hasPrimitiveIdExport = false;
      if (nextStage == ShaderStageFragment) {
        hasPrimitiveIdExport =
            m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs.primitiveId;
      } else if (nextStage == ShaderStageInvalid) {
        if (m_shaderStage == ShaderStageCopyShader) {
          hasPrimitiveIdExport =
              m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs.primitiveId;
        }
      }

      if (hasPrimitiveIdExport) {
        unsigned loc = InvalidValue;
        if (m_shaderStage == ShaderStageCopyShader) {
          assert(inOutUsage.gs.builtInOutLocs.find(BuiltInPrimitiveId) != inOutUsage.gs.builtInOutLocs.end());
          loc = inOutUsage.gs.builtInOutLocs[BuiltInPrimitiveId];
        } else {
          assert(inOutUsage.builtInOutputLocMap.find(BuiltInPrimitiveId) != inOutUsage.builtInOutputLocMap.end());
          loc = inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId];
        }

        assert(m_primitiveId);
        Value *primitiveId = new BitCastInst(m_primitiveId, Type::getFloatTy(*m_context), "", insertPos);

        Value *args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc), // tgt
            ConstantInt::get(Type::getInt32Ty(*m_context), 0x1),                      // en
            primitiveId,                                                              // src0
            undef,                                                                    // src1
            undef,                                                                    // src2
            undef,                                                                    // src3
            ConstantInt::get(Type::getInt1Ty(*m_context), false),                     // done
            ConstantInt::get(Type::getInt1Ty(*m_context), false)                      // vm
        };
        emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
        ++inOutUsage.expCount;
      }
    }
    // NOTE: If multi-view is enabled, always do exporting for gl_Layer.
    if (m_gfxIp.major <= 8 && enableMultiView) {
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

      // "Done" flag is valid for exporting position 0 ~ 3
      m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

      // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
      if (useViewportIndex) {
        bool hasViewportIndexExport = true;
        if (nextStage == ShaderStageFragment) {
          const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

          hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
        }

        if (hasViewportIndexExport) {
          unsigned loc = InvalidValue;
          if (m_shaderStage == ShaderStageCopyShader) {
            assert(inOutUsage.gs.builtInOutLocs.find(BuiltInViewportIndex) != inOutUsage.gs.builtInOutLocs.end());
            loc = inOutUsage.gs.builtInOutLocs[BuiltInViewportIndex];
          } else {
            assert(inOutUsage.builtInOutputLocMap.find(BuiltInViewportIndex) != inOutUsage.builtInOutputLocMap.end());
            loc = inOutUsage.builtInOutputLocMap[BuiltInViewportIndex];
          }

          Value *viewportIndex = new BitCastInst(m_viewportIndex, Type::getFloatTy(*m_context), "", insertPos);

          Value *args[] = {
              ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc), // tgt
              ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),                      // en
              viewportIndex,                                                            // src0
              undef,                                                                    // src1
              undef,                                                                    // src2
              undef,                                                                    // src3
              ConstantInt::get(Type::getInt1Ty(*m_context), false),                     // done
              ConstantInt::get(Type::getInt1Ty(*m_context), false)                      // vm
          };
          emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
          ++inOutUsage.expCount;
        }
      }

      // NOTE: We have to export gl_Layer via generic outputs as well.
      if (useLayer) {
        bool hasLayerExport = true;
        if (nextStage == ShaderStageFragment) {
          const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

          hasLayerExport = nextBuiltInUsage.layer || nextBuiltInUsage.viewIndex;
        }

        if (hasLayerExport) {
          unsigned loc = InvalidValue;
          if (m_shaderStage == ShaderStageCopyShader) {
            assert(inOutUsage.gs.builtInOutLocs.find(BuiltInLayer) != inOutUsage.gs.builtInOutLocs.end() ||
                   inOutUsage.gs.builtInOutLocs.find(BuiltInViewIndex) != inOutUsage.gs.builtInOutLocs.end());

            loc = enableMultiView ? inOutUsage.gs.builtInOutLocs[BuiltInViewIndex]
                                  : inOutUsage.gs.builtInOutLocs[BuiltInLayer];
          } else {
            assert(inOutUsage.builtInOutputLocMap.find(BuiltInLayer) != inOutUsage.builtInOutputLocMap.end() ||
                   inOutUsage.builtInOutputLocMap.find(BuiltInViewIndex) != inOutUsage.builtInOutputLocMap.end());

            loc = enableMultiView ? inOutUsage.builtInOutputLocMap[BuiltInViewIndex]
                                  : inOutUsage.builtInOutputLocMap[BuiltInLayer];
          }

          Value *layer = new BitCastInst(m_layer, Type::getFloatTy(*m_context), "", insertPos);

          Value *args[] = {
              ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc), // tgt
              ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),                      // en
              layer,                                                                    // src0
              undef,                                                                    // src1
              undef,                                                                    // src2
              undef,                                                                    // src3
              ConstantInt::get(Type::getInt1Ty(*m_context), false),                     // done
              ConstantInt::get(Type::getInt1Ty(*m_context), false)                      // vm
          };
          emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
          ++inOutUsage.expCount;
        }
      }
    }

    // NOTE: For GFX10+, dummy generic output is no longer needed. Field NO_PC_EXPORT of SPI_VS_OUT_CONFIG
    // will control the behavior.
    if (m_gfxIp.major <= 9) {
      // NOTE: If no generic outputs is present in this shader, we have to export a dummy one
      if (inOutUsage.expCount == 0) {
        Value *args[] = {
            ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0), // tgt
            ConstantInt::get(Type::getInt32Ty(*m_context), 0),                  // en
            undef,                                                              // src0
            undef,                                                              // src1
            undef,                                                              // src2
            undef,                                                              // src3
            ConstantInt::get(Type::getInt1Ty(*m_context), false),               // done
            ConstantInt::get(Type::getInt1Ty(*m_context), false)                // vm
        };
        emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
        ++inOutUsage.expCount;
      }
    }

    if (m_pipelineState->getBuilderContext()->buildingRelocatableElf()) {
      // If we are building relocatable shaders, it is possible there are
      // generic outputs that are not written to.  We need to count them in
      // the export count.
      auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);
      for (auto locMap : resUsage->inOutUsage.outputLocMap) {
        if (m_expLocs.count(locMap.second) != 0)
          continue;
        ++inOutUsage.expCount;
      }
    }
  } else if (m_shaderStage == ShaderStageGeometry) {
    if (!m_pipelineState->isGsOnChip() && m_gfxIp.major >= 10) {
      // NOTE: This is a workaround because backend compiler does not provide s_waitcnt_vscnt intrinsic, so we
      // use fence release to generate s_waitcnt vmcnt/s_waitcnt_vscnt before s_sendmsg(MSG_GS_DONE)
      new FenceInst(*m_context, AtomicOrdering::Release, SyncScope::System, insertPos);
    }

    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
    auto waveId = getFunctionArgument(m_entryPoint, entryArgIdxs.waveId);
    Value *args[] = {ConstantInt::get(Type::getInt32Ty(*m_context), GsDone), waveId};

    emitCall("llvm.amdgcn.s.sendmsg", Type::getVoidTy(*m_context), args, {}, insertPos);
  } else if (m_shaderStage == ShaderStageFragment) {
    if (m_fragDepth || m_fragStencilRef || m_sampleMask) {
      auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
      Value *fragDepth = undef;
      Value *fragStencilRef = undef;
      Value *sampleMask = undef;

      unsigned channelMask = 0x1; // Always export gl_FragDepth
      if (m_fragDepth) {
        assert(builtInUsage.fragDepth);
        (void(builtInUsage)); // unused
        fragDepth = m_fragDepth;
      }

      if (m_fragStencilRef) {
        assert(builtInUsage.fragStencilRef);
        (void(builtInUsage)); // unused
        channelMask |= 2;
        fragStencilRef = m_fragStencilRef;
      }

      if (m_sampleMask) {
        assert(builtInUsage.sampleMask);
        (void(builtInUsage)); // unused
        channelMask |= 4;
        sampleMask = m_sampleMask;
      }

      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_Z), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), channelMask),  // en
          fragDepth,                                                    // src0
          fragStencilRef,                                               // src1
          sampleMask,                                                   // src2
          undef,                                                        // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),         // done
          ConstantInt::get(Type::getInt1Ty(*m_context), true)           // vm
      };
      m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    }

    // Export fragment colors
    for (unsigned location = 0; location < MaxColorTargets; ++location) {
      auto &expFragColor = m_expFragColors[location];
      if (expFragColor.size() > 0) {
        Value *output = nullptr;
        unsigned compCount = expFragColor.size();
        assert(compCount <= 4);

        // Set CB shader mask
        auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);
        const unsigned channelMask = ((1 << compCount) - 1);
        const unsigned origLoc = resUsage->inOutUsage.fs.outputOrigLocs[location];
        if (origLoc == InvalidValue)
          continue;

        resUsage->inOutUsage.fs.cbShaderMask |= (channelMask << (4 * origLoc));

        // Construct exported fragment colors
        if (compCount == 1)
          output = expFragColor[0];
        else {
          const auto compTy = expFragColor[0]->getType();

          output = UndefValue::get(VectorType::get(compTy, compCount));
          for (unsigned i = 0; i < compCount; ++i) {
            assert(expFragColor[i]->getType() == compTy);
            output = InsertElementInst::Create(output, expFragColor[i],
                                               ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
          }
        }

        // Do fragment color exporting
        auto exportInst = m_fragColorExport->run(output, location, insertPos);
        if (exportInst)
          m_lastExport = cast<CallInst>(exportInst);
      }
    }

    // NOTE: If outputs are present in fragment shader, we have to export a dummy one
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);

    // NOTE: GFX10 can allow no dummy export when the fragment shader does not have discard operation
    // or ROV (Raster-ordered views)
    resUsage->inOutUsage.fs.dummyExport = (m_gfxIp.major < 10 || resUsage->builtInUsage.fs.discard);
    if (!m_lastExport && resUsage->inOutUsage.fs.dummyExport) {
      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_MRT_0), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0x1),              // en
          zero,                                                             // src0
          undef,                                                            // src1
          undef,                                                            // src2
          undef,                                                            // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),             // done
          ConstantInt::get(Type::getInt1Ty(*m_context), true)               // vm
      };
      m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    }
  }

  if (m_lastExport) {
    // Set "done" flag
    auto exportName = m_lastExport->getCalledFunction()->getName();
    if (exportName == "llvm.amdgcn.exp.f32")
      m_lastExport->setOperand(6, ConstantInt::get(Type::getInt1Ty(*m_context), true));
    else {
      assert(exportName == "llvm.amdgcn.exp.compr.v2f16");
      m_lastExport->setOperand(4, ConstantInt::get(Type::getInt1Ty(*m_context), true));
    }
  }
}

// =====================================================================================================================
// Patches import calls for generic inputs of vertex shader.
//
// @param inputTy : Type of input value
// @param location : Location of the input
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchVsGenericInputImport(Type *inputTy, unsigned location, unsigned compIdx,
                                                         Instruction *insertPos) {
  Value *input = UndefValue::get(inputTy);

  // Do vertex fetch operations
  assert(m_vertexFetch);
  auto vertex = m_vertexFetch->run(inputTy, location, compIdx, insertPos);

  // Cast vertex fetch results if necessary
  const Type *vertexTy = vertex->getType();
  if (vertexTy != inputTy) {
    assert(canBitCast(vertexTy, inputTy));
    input = new BitCastInst(vertex, inputTy, "", insertPos);
  } else
    input = vertex;

  return input;
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
  return readValueFromLds(false, inputTy, ldsOffset, insertPos);
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

  const unsigned compCount = inputTy->isVectorTy() ? inputTy->getVectorNumElements() : 1;
  const unsigned bitWidth = inputTy->getScalarSizeInBits();

  Type *origInputTy = inputTy;

  if (bitWidth == 64) {
    compIdx *= 2; // For 64-bit data type, the component indexing must multiply by 2

    // Cast 64-bit data type to float vector
    inputTy = VectorType::get(Type::getFloatTy(*m_context), compCount * 2);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  Value *input = loadValueFromEsGsRing(inputTy, location, compIdx, vertexIdx, insertPos);

  if (inputTy != origInputTy) {
    // Cast back to oringinal input type
    assert(canBitCast(inputTy, origInputTy));
    assert(inputTy->isVectorTy());

    input = new BitCastInst(input, origInputTy, "", insertPos);
  }

  return input;
}

// =====================================================================================================================
// Patches import calls for generic inputs of fragment shader.
//
// @param inputTy : Type of input value
// @param location : Base location of the input
// @param locOffset : Relative location offset
// @param compIdx : Index used for vector element indexing (could be null)
// @param auxInterpValue : Auxiliary value of interpolation: for non "custom" interpolation, it is the explicitly
// calculated I/J; for "custom" interpolation, it is vertex no. (could be null)
// @param interpMode : Interpolation mode
// @param interpLoc : Interpolation location
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchFsGenericInputImport(Type *inputTy, unsigned location, Value *locOffset,
                                                         Value *compIdx, Value *auxInterpValue, unsigned interpMode,
                                                         unsigned interpLoc, Instruction *insertPos) {
  Value *input = UndefValue::get(inputTy);

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment);
  auto &interpInfo = resUsage->inOutUsage.fs.interpInfo;

  const unsigned locCount = inputTy->getPrimitiveSizeInBits() / 8 > SizeOfVec4 ? 2 : 1;
  while (interpInfo.size() <= location + locCount - 1)
    interpInfo.push_back(InvalidFsInterpInfo);
  interpInfo[location] = {
      location,
      (interpMode == InOutInfo::InterpModeFlat),
      (interpMode == InOutInfo::InterpModeCustom),
      (inputTy->getScalarSizeInBits() == 16),
  };

  if (locCount > 1) {
    // The input occupies two consecutive locations
    assert(locCount == 2);
    interpInfo[location + 1] = {
        location + 1,
        (interpMode == InOutInfo::InterpModeFlat),
        (interpMode == InOutInfo::InterpModeCustom),
        false,
    };
  }

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);
  Value *iCoord = nullptr;
  Value *jCoord = nullptr;

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
    iCoord = ExtractElementInst::Create(ij, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);
    jCoord = ExtractElementInst::Create(ij, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
  }

  Attribute::AttrKind attribs[] = {Attribute::ReadNone};

  Type *basicTy = inputTy->isVectorTy() ? inputTy->getVectorElementType() : inputTy;

  const unsigned compCout = inputTy->isVectorTy() ? inputTy->getVectorNumElements() : 1;
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
    interpTy = VectorType::get(interpTy, numChannels);
  Value *interp = UndefValue::get(interpTy);

  unsigned startChannel = 0;
  if (compIdx)
    startChannel = cast<ConstantInt>(compIdx)->getZExtValue();

  Value *loc = ConstantInt::get(Type::getInt32Ty(*m_context), location);
  if (locOffset) {
    loc = BinaryOperator::CreateAdd(loc, locOffset, "", insertPos);
    assert((startChannel + numChannels) <= 4);
  }

  for (unsigned i = startChannel; i < startChannel + numChannels; ++i) {
    Value *compValue = nullptr;

    if (interpMode != InOutInfo::InterpModeFlat && interpMode != InOutInfo::InterpModeCustom) {
      assert((basicTy->isHalfTy() || basicTy->isFloatTy()) && numChannels <= 4);
      (void(basicTy)); // unused

      if (bitWidth == 16) {
        Value *args1[] = {
            iCoord,                                               // i
            ConstantInt::get(Type::getInt32Ty(*m_context), i),    // attr_chan
            loc,                                                  // attr
            ConstantInt::get(Type::getInt1Ty(*m_context), false), // high
            primMask                                              // m0
        };
        compValue = emitCall("llvm.amdgcn.interp.p1.f16", Type::getFloatTy(*m_context), args1, attribs, insertPos);

        Value *args2[] = {
            compValue,                                            // p1
            jCoord,                                               // j
            ConstantInt::get(Type::getInt32Ty(*m_context), i),    // attr_chan
            loc,                                                  // attr
            ConstantInt::get(Type::getInt1Ty(*m_context), false), // high
            primMask                                              // m0
        };
        compValue = emitCall("llvm.amdgcn.interp.p2.f16", Type::getHalfTy(*m_context), args2, attribs, insertPos);
      } else {
        Value *args1[] = {
            iCoord,                                            // i
            ConstantInt::get(Type::getInt32Ty(*m_context), i), // attr_chan
            loc,                                               // attr
            primMask                                           // m0
        };
        compValue = emitCall("llvm.amdgcn.interp.p1", Type::getFloatTy(*m_context), args1, attribs, insertPos);

        Value *args2[] = {
            compValue,                                         // p1
            jCoord,                                            // j
            ConstantInt::get(Type::getInt32Ty(*m_context), i), // attr_chan
            loc,                                               // attr
            primMask                                           // m0
        };
        compValue = emitCall("llvm.amdgcn.interp.p2", Type::getFloatTy(*m_context), args2, attribs, insertPos);
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
      } else
        assert(interpMode == InOutInfo::InterpModeFlat);

      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), interpParam),                        // param
          ConstantInt::get(Type::getInt32Ty(*m_context), i % 4),                              // attr_chan
          locOffset ? loc : ConstantInt::get(Type::getInt32Ty(*m_context), location + i / 4), // attr
          primMask                                                                            // m0
      };
      compValue = emitCall("llvm.amdgcn.interp.mov", Type::getFloatTy(*m_context), args, attribs, insertPos);

      if (bitWidth == 8) {
        compValue = new BitCastInst(compValue, Type::getInt32Ty(*m_context), "", insertPos);
        compValue = new TruncInst(compValue, Type::getInt8Ty(*m_context), "", insertPos);
      } else if (bitWidth == 16) {
        compValue = new BitCastInst(compValue, Type::getInt32Ty(*m_context), "", insertPos);
        compValue = new TruncInst(compValue, Type::getInt16Ty(*m_context), "", insertPos);
        compValue = new BitCastInst(compValue, Type::getHalfTy(*m_context), "", insertPos);
      }
    }

    if (numChannels == 1)
      interp = compValue;
    else {
      interp = InsertElementInst::Create(
          interp, compValue, ConstantInt::get(Type::getInt32Ty(*m_context), i - startChannel), "", insertPos);
    }
  }

  // Store interpolation results to inputs
  if (interpTy == inputTy)
    input = interp;
  else {
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
  return readValueFromLds(true, outputTy, ldsOffset, insertPos);
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

  m_expLocs.insert(location);

  if (m_hasTs) {
    auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, location, compIdx, insertPos);
    writeValueToLds(output, ldsOffset, insertPos);
  } else {
    if (m_hasGs) {
      assert(outputTy->isIntOrIntVectorTy() || outputTy->isFPOrFPVectorTy());

      const unsigned bitWidth = outputTy->getScalarSizeInBits();
      if (bitWidth == 64) {
        // For 64-bit data type, the component indexing must multiply by 2
        compIdx *= 2;

        unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() * 2 : 2;

        outputTy = VectorType::get(Type::getFloatTy(*m_context), compCount);
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
  writeValueToLds(output, ldsOffset, insertPos);
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

      unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() * 2 : 2;
      outputTy = VectorType::get(Type::getFloatTy(*m_context), compCount);

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
      outputTy = VectorType::get(Type::getFloatTy(*m_context), outputTy->getVectorNumElements() * 2);
    else
      outputTy = VectorType::get(Type::getFloatTy(*m_context), 2);

    output = new BitCastInst(output, outputTy, "", insertPos);
  } else
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);

  const unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() : 1;
  // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend BYTE/WORD to DWORD and
  // store DWORD to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size is based on number of DWORDs.
  unsigned byteSize = (outputTy->getScalarSizeInBits() / 8) * compCount;
  if (bitWidth == 8 || bitWidth == 16)
    byteSize *= (32 / bitWidth);

  assert(compIdx <= 4);

  // Field "genericOutByteSizes" now gets set when generating the copy shader. Just assert that we agree on the
  // byteSize.
  auto &genericOutByteSizes =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.genericOutByteSizes;
  assert(genericOutByteSizes[streamId][location][compIdx] == byteSize);
  (void(genericOutByteSizes)); // unused

  storeValueToGsVsRing(output, location, compIdx, streamId, insertPos);
}

// =====================================================================================================================
// Patches export calls for generic outputs of fragment shader.
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchFsGenericOutputExport(Value *output, unsigned location, unsigned compIdx,
                                                        Instruction *insertPos) {
  Type *outputTy = output->getType();

  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);
  (void(bitWidth)); // unused

  auto compTy = outputTy->isVectorTy() ? outputTy->getVectorElementType() : outputTy;
  unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() : 1;

  std::vector<Value *> outputComps;
  for (unsigned i = 0; i < compCount; ++i) {
    Value *outputComp = nullptr;
    if (compCount == 1)
      outputComp = output;
    else {
      outputComp = ExtractElementInst::Create(output, ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }

    outputComps.push_back(outputComp);
  }

  assert(location < MaxColorTargets);
  auto &expFragColor = m_expFragColors[location];

  while (compIdx + compCount > expFragColor.size())
    expFragColor.push_back(UndefValue::get(compTy));

  for (unsigned i = 0; i < compCount; ++i)
    expFragColor[compIdx + i] = outputComps[i];
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
  case BuiltInVertexIndex:
    return m_vertexFetch->getVertexIndex();
  case BuiltInInstanceIndex:
    return m_vertexFetch->getInstanceIndex();
  case BuiltInBaseVertex:
    return getFunctionArgument(m_entryPoint, entryArgIdxs.baseVertex);
  case BuiltInBaseInstance:
    return getFunctionArgument(m_entryPoint, entryArgIdxs.baseInstance);
  case BuiltInDrawIndex:
    return getFunctionArgument(m_entryPoint, entryArgIdxs.drawIndex);
  case BuiltInViewIndex:
    return getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
  case BuiltInSubgroupSize:
    return ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getShaderWaveSize(m_shaderStage));
  case BuiltInSubgroupLocalInvocationId:
    return getSubgroupLocalInvocationId(insertPos);
  case BuiltInDeviceIndex:
    return ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getDeviceIndex());
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

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  auto &inoutUsage = resUsage->inOutUsage;
  auto &builtInInLocMap = inoutUsage.builtInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    input = readValueFromLds(false, inputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTcsInput(inputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    input = readValueFromLds(false, inputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap[builtInId];

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
  case BuiltInSubgroupSize: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getShaderWaveSize(m_shaderStage));
    break;
  }
  case BuiltInSubgroupLocalInvocationId: {
    input = getSubgroupLocalInvocationId(insertPos);
    break;
  }
  case BuiltInDeviceIndex: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getDeviceIndex());
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
  auto &inOutUsage = resUsage->inOutUsage;
  auto &builtInInLocMap = inOutUsage.builtInInputLocMap;
  auto &perPatchBuiltInInLocMap = inOutUsage.perPatchBuiltInInputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    input = readValueFromLds(false, inputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    assert(!elemIdx);
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    input = readValueFromLds(false, inputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    assert(builtInInLocMap.find(builtInId) != builtInInLocMap.end());
    const unsigned loc = builtInInLocMap[builtInId];

    if (!elemIdx) {
      // gl_ClipDistanceIn[]/gl_CullDistanceIn[] is treated as 2 x vec4
      assert(inputTy->isArrayTy());

      auto elemTy = inputTy->getArrayElementType();
      for (unsigned i = 0; i < inputTy->getArrayNumElements(); ++i) {
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTesInput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, insertPos);
        input = InsertValueInst::Create(input, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      input = readValueFromLds(false, inputTy, ldsOffset, insertPos);
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
    unsigned loc = perPatchBuiltInInLocMap[builtInId];

    if (!elemIdx) {
      // gl_TessLevelOuter[4] is treated as vec4
      // gl_TessLevelInner[2] is treated as vec2
      assert(inputTy->isArrayTy());

      auto elemTy = inputTy->getArrayElementType();
      for (unsigned i = 0; i < inputTy->getArrayNumElements(); ++i) {
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTesInput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(false, elemTy, ldsOffset, insertPos);
        input = InsertValueInst::Create(input, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTesInput(inputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      input = readValueFromLds(false, inputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInViewIndex: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.viewIndex);
    break;
  }
  case BuiltInSubgroupSize: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getShaderWaveSize(m_shaderStage));
    break;
  }
  case BuiltInSubgroupLocalInvocationId: {
    input = getSubgroupLocalInvocationId(insertPos);
    break;
  }
  case BuiltInDeviceIndex: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getDeviceIndex());
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
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

  unsigned loc = inOutUsage.builtInInputLocMap[builtInId];
  assert(loc != InvalidValue);

  switch (builtInId) {
  case BuiltInPosition:
  case BuiltInPointSize:
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
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
  case BuiltInSubgroupSize: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getShaderWaveSize(m_shaderStage));
    break;
  }
  case BuiltInSubgroupLocalInvocationId: {
    input = getSubgroupLocalInvocationId(insertPos);
    break;
  }
  case BuiltInDeviceIndex: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getDeviceIndex());
    break;
  }
  // Handle internal-use built-ins
  case BuiltInWaveId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.waveId);
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
// Patches import calls for built-in inputs of fragment shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param sampleId : Sample ID; only needed for BuiltInSamplePosOffset
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchFsBuiltInInputImport(Type *inputTy, unsigned builtInId, Value *sampleId,
                                                         Instruction *insertPos) {
  Value *input = UndefValue::get(inputTy);

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageFragment)->entryArgIdxs.fs;
  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->inOutUsage;

  Attribute::AttrKind attribs[] = {Attribute::ReadNone};

  switch (builtInId) {
  case BuiltInSampleMask: {
    assert(inputTy->isArrayTy());

    auto sampleCoverage = getFunctionArgument(m_entryPoint, entryArgIdxs.sampleCoverage);
    auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

    // gl_SampleID = Ancillary[11:8]
    Value *args[] = {ancillary, ConstantInt::get(Type::getInt32Ty(*m_context), 8),
                     ConstantInt::get(Type::getInt32Ty(*m_context), 4)};
    auto sampleId = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, {}, insertPos);

    auto sampleMaskIn = sampleCoverage;
    if (m_pipelineState->getRasterizerState().perSampleShading) {
      // gl_SampleMaskIn[0] = (SampleCoverage & (1 << gl_SampleID))
      sampleMaskIn =
          BinaryOperator::CreateShl(ConstantInt::get(Type::getInt32Ty(*m_context), 1), sampleId, "", insertPos);
      sampleMaskIn = BinaryOperator::CreateAnd(sampleCoverage, sampleMaskIn, "", insertPos);
    }

    // NOTE: Only gl_SampleMaskIn[0] is valid for us.
    input = InsertValueInst::Create(input, sampleMaskIn, {0}, "", insertPos);

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

    fragCoord[3] = emitCall("llvm.amdgcn.rcp.f32", Type::getFloatTy(*m_context), {fragCoord[3]}, attribs, insertPos);

    for (unsigned i = 0; i < 4; ++i) {
      input = InsertElementInst::Create(input, fragCoord[i], ConstantInt::get(Type::getInt32Ty(*m_context), i), "",
                                        insertPos);
    }

    break;
  }
  case BuiltInFrontFacing: {
    auto frontFacing = getFunctionArgument(m_entryPoint, entryArgIdxs.frontFacing);
    input = new ICmpInst(insertPos, ICmpInst::ICMP_NE, frontFacing, ConstantInt::get(Type::getInt32Ty(*m_context), 0));
    input = CastInst::CreateIntegerCast(input, inputTy, false, "", insertPos);
    break;
  }
  case BuiltInPointCoord: {
    assert(inOutUsage.builtInInputLocMap.find(BuiltInPointCoord) != inOutUsage.builtInInputLocMap.end());
    const unsigned loc = inOutUsage.builtInInputLocMap[BuiltInPointCoord];

    auto &interpInfo = inOutUsage.fs.interpInfo;
    while (interpInfo.size() <= loc)
      interpInfo.push_back(InvalidFsInterpInfo);
    interpInfo[loc] = {loc, false, false, false};

    // Emulation for "in vec2 gl_PointCoord"
    const bool perSampleShading = m_pipelineState->getRasterizerState().perSampleShading;
    input = patchFsGenericInputImport(inputTy, loc, nullptr, nullptr, nullptr, InOutInfo::InterpModeSmooth,
                                      perSampleShading ? InOutInfo::InterpLocSample : InOutInfo::InterpLocCenter,
                                      insertPos);
    break;
  }
  case BuiltInHelperInvocation: {
    input = emitCall("llvm.amdgcn.ps.live", Type::getInt1Ty(*m_context), {}, Attribute::ReadNone, insertPos);
    input = BinaryOperator::CreateNot(input, "", insertPos);
    input = CastInst::CreateIntegerCast(input, inputTy, false, "", insertPos);
    break;
  }
  case BuiltInPrimitiveId:
  case BuiltInLayer:
  case BuiltInViewportIndex:
  case BuiltInViewIndex: {
    unsigned loc = InvalidValue;

    if (builtInId == BuiltInPrimitiveId) {
      assert(inOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) != inOutUsage.builtInInputLocMap.end());
      loc = inOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
    } else if (builtInId == BuiltInLayer) {
      assert(inOutUsage.builtInInputLocMap.find(BuiltInLayer) != inOutUsage.builtInInputLocMap.end());
      loc = inOutUsage.builtInInputLocMap[BuiltInLayer];
    } else if (builtInId == BuiltInViewIndex) {
      assert(inOutUsage.builtInInputLocMap.find(BuiltInViewIndex) != inOutUsage.builtInInputLocMap.end());
      loc = inOutUsage.builtInInputLocMap[BuiltInViewIndex];
    } else {
      assert(builtInId == BuiltInViewportIndex);

      assert(inOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != inOutUsage.builtInInputLocMap.end());
      loc = inOutUsage.builtInInputLocMap[BuiltInViewportIndex];
    }

    auto &interpInfo = inOutUsage.fs.interpInfo;
    while (interpInfo.size() <= loc)
      interpInfo.push_back(InvalidFsInterpInfo);
    interpInfo[loc] = {loc, true, false}; // Flat interpolation

    // Emulation for "in int gl_PrimitiveID" or "in int gl_Layer" or "in int gl_ViewportIndex"
    // or "in int gl_ViewIndex"
    input = patchFsGenericInputImport(inputTy, loc, nullptr, nullptr, nullptr, InOutInfo::InterpModeFlat,
                                      InOutInfo::InterpLocCenter, insertPos);
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
    auto ij = getFunctionArgument(m_entryPoint, entryArgIdxs.linearInterp.center);

    ij = new BitCastInst(ij, VectorType::get(Type::getFloatTy(*m_context), 2), "", insertPos);
    auto iCoord = ExtractElementInst::Create(ij, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);
    auto jCoord = ExtractElementInst::Create(ij, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);

    const unsigned elemCount = inputTy->getArrayNumElements();
    assert(elemCount <= MaxClipCullDistanceCount);

    for (unsigned i = 0; i < elemCount; ++i) {
      Value *args1[] = {
          iCoord,                                                                       // i
          ConstantInt::get(Type::getInt32Ty(*m_context), (startChannel + i) % 4),       // attr_chan
          ConstantInt::get(Type::getInt32Ty(*m_context), loc + (startChannel + i) / 4), // attr
          primMask                                                                      // m0
      };
      auto compValue = emitCall("llvm.amdgcn.interp.p1", Type::getFloatTy(*m_context), args1, attribs, insertPos);

      Value *args2[] = {
          compValue,                                                                    // p1
          jCoord,                                                                       // j
          ConstantInt::get(Type::getInt32Ty(*m_context), (startChannel + i) % 4),       // attr_chan
          ConstantInt::get(Type::getInt32Ty(*m_context), loc + (startChannel + i) / 4), // attr
          primMask                                                                      // m0
      };
      compValue = emitCall("llvm.amdgcn.interp.p2", Type::getFloatTy(*m_context), args2, attribs, insertPos);
      input = InsertValueInst::Create(input, compValue, {i}, "", insertPos);
    }

    break;
  }
  case BuiltInSampleId: {
    auto ancillary = getFunctionArgument(m_entryPoint, entryArgIdxs.ancillary);

    // gl_SampleID = Ancillary[11:8]
    Value *args[] = {ancillary, ConstantInt::get(Type::getInt32Ty(*m_context), 8),
                     ConstantInt::get(Type::getInt32Ty(*m_context), 4)};
    input = emitCall("llvm.amdgcn.ubfe.i32", inputTy, args, {}, insertPos);

    break;
  }
  case BuiltInSubgroupSize: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getShaderWaveSize(m_shaderStage));
    break;
  }
  case BuiltInSubgroupLocalInvocationId: {
    input = getSubgroupLocalInvocationId(insertPos);
    break;
  }
  case BuiltInDeviceIndex: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getDeviceIndex());
    break;
  }
  // Handle internal-use built-ins for sample position emulation
  case BuiltInNumSamples: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getRasterizerState().numSamples);
    break;
  }
  case BuiltInSamplePatternIdx: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getRasterizerState().samplePatternIdx);
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
    input = getSamplePosOffset(inputTy, sampleId, insertPos);
    break;
  }
  case BuiltInSamplePosition: {
    input = getSamplePosition(inputTy, insertPos);
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
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *numSamples = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInNumSamples, nullptr, insertPos);
  Value *patternIdx = patchFsBuiltInInputImport(builder.getInt32Ty(), BuiltInSamplePatternIdx, nullptr, insertPos);
  Value *validOffset = builder.CreateAdd(patternIdx, sampleId);
  // offset = (sampleCount > sampleId) ? (samplePatternOffset + sampleId) : 0
  Value *sampleValid = builder.CreateICmpUGT(numSamples, sampleId);
  Value *offset = builder.CreateSelect(sampleValid, validOffset, builder.getInt32(0));
  // Load sample position descriptor.
  auto desc = emitCall(lgcName::DescriptorLoadBuffer, VectorType::get(builder.getInt32Ty(), 4),
                       {
                           builder.getInt32(InternalResourceTable),
                           builder.getInt32(SiDrvTableSamplepos),
                           builder.getInt32(0),
                       },
                       {}, insertPos);
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
// Patches import calls for built-in inputs of compute shader.
//
// @param inputTy : Type of input value
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchCsBuiltInInputImport(Type *inputTy, unsigned builtInId, Instruction *insertPos) {
  Value *input = nullptr;

  auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageCompute);
  auto &entryArgIdxs = intfData->entryArgIdxs.cs;

  switch (builtInId) {
  case BuiltInWorkgroupSize: {
    input = getWorkgroupSize();
    break;
  }
  case BuiltInNumWorkgroups: {
    input = m_pipelineSysValues.get(m_entryPoint)->getNumWorkgroups();
    break;
  }
  case BuiltInWorkgroupId: {
    input = getFunctionArgument(m_entryPoint, entryArgIdxs.workgroupId);
    break;
  }
  case BuiltInLocalInvocationId: {
    input = getInLocalInvocationId(insertPos);
    break;
  }
  case BuiltInSubgroupSize: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getShaderWaveSize(m_shaderStage));
    break;
  }
  case BuiltInSubgroupLocalInvocationId: {
    input = getSubgroupLocalInvocationId(insertPos);
    break;
  }
  case BuiltInDeviceIndex: {
    input = ConstantInt::get(Type::getInt32Ty(*m_context), m_pipelineState->getDeviceIndex());
    break;
  }
  case BuiltInNumSubgroups: {
    // workgroupSize = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z
    auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
    const unsigned workgroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;

    // gl_NumSubgroups = (workgroupSize + gl_SubGroupSize - 1) / gl_SubgroupSize
    const unsigned subgroupSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
    const unsigned numSubgroups = (workgroupSize + subgroupSize - 1) / subgroupSize;

    input = ConstantInt::get(Type::getInt32Ty(*m_context), numSubgroups);
    break;
  }
  case BuiltInGlobalInvocationId: {
    input = getGlobalInvocationId(inputTy, insertPos);
    break;
  }
  case BuiltInLocalInvocationIndex: {
    input = getLocalInvocationIndex(inputTy, insertPos);
    break;
  }
  case BuiltInSubgroupId: {
    input = getSubgroupId(inputTy, insertPos);
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
// Get GlobalInvocationId
//
// @param inputTy : Type of GlobalInvocationId
// @param insertPos : Insert position
Value *PatchInOutImportExport::getGlobalInvocationId(Type *inputTy, Instruction *insertPos) {
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *workgroupSize = patchCsBuiltInInputImport(inputTy, BuiltInWorkgroupSize, insertPos);
  Value *workgroupId = patchCsBuiltInInputImport(inputTy, BuiltInWorkgroupId, insertPos);
  Value *localInvocationId = patchCsBuiltInInputImport(inputTy, BuiltInLocalInvocationId, insertPos);
  Value *input = builder.CreateMul(workgroupSize, workgroupId);
  input = builder.CreateAdd(input, localInvocationId);
  return input;
}

// =====================================================================================================================
// Get LocalInvocationIndex
//
// @param inputTy : Type of LocalInvocationIndex
// @param insertPos : Insert position
Value *PatchInOutImportExport::getLocalInvocationIndex(Type *inputTy, Instruction *insertPos) {
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *workgroupSize = patchCsBuiltInInputImport(inputTy, BuiltInWorkgroupSize, insertPos);
  Value *localInvocationId = patchCsBuiltInInputImport(inputTy, BuiltInLocalInvocationId, insertPos);
  Value *input = builder.CreateMul(builder.CreateExtractElement(workgroupSize, 1),
                                   builder.CreateExtractElement(localInvocationId, 2));
  input = builder.CreateAdd(input, builder.CreateExtractElement(localInvocationId, 1));
  input = builder.CreateMul(builder.CreateExtractElement(workgroupSize, uint64_t(0)), input);
  input = builder.CreateAdd(input, builder.CreateExtractElement(localInvocationId, uint64_t(0)));
  return input;
}

// =====================================================================================================================
// Get SubgroupId
//
// @param inputTy : Type of LocalInvocationIndex
// @param insertPos : Insert position
Value *PatchInOutImportExport::getSubgroupId(Type *inputTy, Instruction *insertPos) {
  // gl_SubgroupID = gl_LocationInvocationIndex / gl_SubgroupSize
  IRBuilder<> builder(*m_context);
  builder.SetInsertPoint(insertPos);
  Value *localInvocationIndex = patchCsBuiltInInputImport(inputTy, BuiltInLocalInvocationIndex, insertPos);
  unsigned subgroupSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  return builder.CreateLShr(localInvocationIndex, builder.getInt32(Log2_32(subgroupSize)));
}

// =====================================================================================================================
// Patches import calls for built-in outputs of tessellation control shader.
//
// @param outputTy : Type of output value
// @param builtInId : ID of the built-in variable
// @param elemIdx : Index used for array/vector element indexing (could be null)
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param insertPos : Where to insert the patch instruction
Value *PatchInOutImportExport::patchTcsBuiltInOutputImport(Type *outputTy, unsigned builtInId, Value *elemIdx,
                                                           Value *vertexIdx, Instruction *insertPos) {
  Value *output = UndefValue::get(outputTy);

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  auto &builtInUsage = resUsage->builtInUsage.tcs;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
  auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    assert(builtInUsage.position);
    (void(builtInUsage)); // unused

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    output = readValueFromLds(true, outputTy, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    assert(builtInUsage.pointSize);
    (void(builtInUsage)); // unused

    assert(!elemIdx);
    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    output = readValueFromLds(true, outputTy, ldsOffset, insertPos);

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
    unsigned loc = builtInOutLocMap[builtInId];

    if (!elemIdx) {
      // gl_ClipDistance[]/gl_CullDistance[] is treated as 2 x vec4
      assert(outputTy->isArrayTy());

      auto elemTy = outputTy->getArrayElementType();
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(true, elemTy, ldsOffset, insertPos);
        output = InsertValueInst::Create(output, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      output = readValueFromLds(true, outputTy, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner: {
    if (builtInId == BuiltInTessLevelOuter) {
      assert(builtInUsage.tessLevelOuter);
      (void(builtInUsage)); // unused
    } else {
      assert(builtInId == BuiltInTessLevelInner);
      assert(builtInUsage.tessLevelInner);
      (void(builtInUsage)); // unused
    }

    assert(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
    unsigned loc = perPatchBuiltInOutLocMap[builtInId];

    if (!elemIdx) {
      // gl_TessLevelOuter[4] is treated as vec4
      // gl_TessLevelInner[2] is treated as vec2
      assert(outputTy->isArrayTy());

      auto elemTy = outputTy->getArrayElementType();
      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elemTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
        auto elem = readValueFromLds(true, elemTy, ldsOffset, insertPos);
        output = InsertValueInst::Create(output, elem, {i}, "", insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      output = readValueFromLds(true, outputTy, ldsOffset, insertPos);
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

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  auto &builtInUsage = resUsage->builtInUsage.vs;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    if (!static_cast<bool>(builtInUsage.position))
      return;

    if (m_hasTs) {
      unsigned loc = builtInOutLocMap[builtInId];
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, insertPos);
      writeValueToLds(output, ldsOffset, insertPos);
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap[builtInId];

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
      unsigned loc = builtInOutLocMap[builtInId];
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy, loc, 0, insertPos);
      writeValueToLds(output, ldsOffset, insertPos);
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap[builtInId];

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

      unsigned loc = builtInOutLocMap[builtInId];
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy->getArrayElementType(), loc, 0, insertPos);

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(elem, ldsOffset, insertPos);

        ldsOffset =
            BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
      }
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap[builtInId];

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

      unsigned loc = builtInOutLocMap[builtInId];
      auto ldsOffset = calcLdsOffsetForVsOutput(outputTy->getArrayElementType(), loc, 0, insertPos);

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        writeValueToLds(elem, ldsOffset, insertPos);

        ldsOffset =
            BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
      }
    } else {
      if (m_hasGs) {
        assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
        unsigned loc = builtInOutLocMap[builtInId];

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

    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

    // NOTE: Only last non-fragment shader stage has to export the value of gl_Layer.
    if (!m_hasTs && !m_hasGs && !static_cast<bool>(enableMultiView)) {
      if (m_gfxIp.major <= 8)
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
      else {
        // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
        m_layer = output;
      }
    }

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
// @param vertexIdx : Input array outermost index used for vertex indexing (could be null)
// @param insertPos : Where to insert the patch instruction
void PatchInOutImportExport::patchTcsBuiltInOutputExport(Value *output, unsigned builtInId, Value *elemIdx,
                                                         Value *vertexIdx, Instruction *insertPos) {
  auto outputTy = output->getType();

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  auto &builtInUsage = resUsage->builtInUsage.tcs;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;
  auto &perPatchBuiltInOutLocMap = resUsage->inOutUsage.perPatchBuiltInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    if (!static_cast<bool>(builtInUsage.position))
      return;

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
    writeValueToLds(output, ldsOffset, insertPos);

    break;
  }
  case BuiltInPointSize: {
    if (!static_cast<bool>(builtInUsage.pointSize))
      return;

    assert(!elemIdx);
    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap[builtInId];

    auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, nullptr, vertexIdx, insertPos);
    writeValueToLds(output, ldsOffset, insertPos);

    break;
  }
  case BuiltInClipDistance:
  case BuiltInCullDistance: {
    if ((builtInId == BuiltInClipDistance && builtInUsage.clipDistance == 0) ||
        (builtInId == BuiltInCullDistance && builtInUsage.cullDistance == 0))
      return;

    assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
    unsigned loc = builtInOutLocMap[builtInId];

    if (!elemIdx) {
      // gl_ClipDistance[]/gl_CullDistance[] is treated as 2 x vec4
      assert(outputTy->isArrayTy());

      for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
        auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
        auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto ldsOffset = calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, elemIdx, vertexIdx, insertPos);
        writeValueToLds(elem, ldsOffset, insertPos);
      }
    } else {
      auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, vertexIdx, insertPos);
      writeValueToLds(output, ldsOffset, insertPos);
    }

    break;
  }
  case BuiltInTessLevelOuter: {
    if (builtInUsage.tessLevelOuter) {
      // Extract tessellation factors
      std::vector<Value *> tessFactors;
      if (!elemIdx) {
        assert(outputTy->isArrayTy());

        unsigned tessFactorCount = 0;

        auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
        switch (primitiveMode) {
        case PrimitiveMode::Isolines:
          tessFactorCount = 2;
          break;
        case PrimitiveMode::Triangles:
          tessFactorCount = 3;
          break;
        case PrimitiveMode::Quads:
          tessFactorCount = 4;
          break;
        default:
          llvm_unreachable("Should never be called!");
          break;
        }

        for (unsigned i = 0; i < tessFactorCount; ++i) {
          Value *elem = ExtractValueInst::Create(output, {i}, "", insertPos);
          tessFactors.push_back(elem);
        }

        if (primitiveMode == PrimitiveMode::Isolines) {
          assert(tessFactorCount == 2);
          std::swap(tessFactors[0], tessFactors[1]);
        }
      } else {
        assert(outputTy->isFloatTy());
        tessFactors.push_back(output);
      }

      Value *tessFactorOffset = calcTessFactorOffset(true, elemIdx, insertPos);
      storeTessFactorToBuffer(tessFactors, tessFactorOffset, insertPos);

      assert(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
      unsigned loc = perPatchBuiltInOutLocMap[builtInId];

      if (!elemIdx) {
        // gl_TessLevelOuter[4] is treated as vec4
        assert(outputTy->isArrayTy());

        for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
          auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
          auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
          auto ldsOffset = calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, elemIdx, vertexIdx, insertPos);
          writeValueToLds(elem, ldsOffset, insertPos);
        }
      } else {
        auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, nullptr, insertPos);
        writeValueToLds(output, ldsOffset, insertPos);
      }
    }
    break;
  }
  case BuiltInTessLevelInner: {
    if (builtInUsage.tessLevelInner) {
      // Extract tessellation factors
      std::vector<Value *> tessFactors;
      if (!elemIdx) {
        unsigned tessFactorCount = 0;

        switch (m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode) {
        case PrimitiveMode::Isolines:
          tessFactorCount = 0;
          break;
        case PrimitiveMode::Triangles:
          tessFactorCount = 1;
          break;
        case PrimitiveMode::Quads:
          tessFactorCount = 2;
          break;
        default:
          llvm_unreachable("Should never be called!");
          break;
        }

        for (unsigned i = 0; i < tessFactorCount; ++i) {
          Value *elem = ExtractValueInst::Create(output, {i}, "", insertPos);
          tessFactors.push_back(elem);
        }
      } else {
        assert(outputTy->isFloatTy());
        tessFactors.push_back(output);
      }

      Value *tessFactorOffset = calcTessFactorOffset(false, elemIdx, insertPos);
      storeTessFactorToBuffer(tessFactors, tessFactorOffset, insertPos);

      assert(perPatchBuiltInOutLocMap.find(builtInId) != perPatchBuiltInOutLocMap.end());
      unsigned loc = perPatchBuiltInOutLocMap[builtInId];

      if (!elemIdx) {
        // gl_TessLevelInner[2] is treated as vec2
        assert(outputTy->isArrayTy());

        for (unsigned i = 0; i < outputTy->getArrayNumElements(); ++i) {
          auto elem = ExtractValueInst::Create(output, {i}, "", insertPos);
          auto elemIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
          auto ldsOffset = calcLdsOffsetForTcsOutput(elem->getType(), loc, nullptr, elemIdx, vertexIdx, insertPos);
          writeValueToLds(elem, ldsOffset, insertPos);
        }
      } else {
        auto ldsOffset = calcLdsOffsetForTcsOutput(outputTy, loc, nullptr, elemIdx, nullptr, insertPos);
        writeValueToLds(output, ldsOffset, insertPos);
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
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  auto &builtInUsage = resUsage->builtInUsage.tes;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  switch (builtInId) {
  case BuiltInPosition: {
    if (!static_cast<bool>(builtInUsage.position))
      return;

    if (m_hasGs) {
      assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
      unsigned loc = builtInOutLocMap[builtInId];

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
      unsigned loc = builtInOutLocMap[builtInId];

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
      unsigned loc = builtInOutLocMap[builtInId];

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
      unsigned loc = builtInOutLocMap[builtInId];

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
      if (m_gfxIp.major <= 8)
        addExportInstForBuiltInOutput(output, builtInId, insertPos);
      else {
        // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
        m_layer = output;
      }
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
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  auto &builtInUsage = resUsage->builtInUsage.gs;
  auto &builtInOutLocMap = resUsage->inOutUsage.builtInOutputLocMap;

  assert(builtInOutLocMap.find(builtInId) != builtInOutLocMap.end());
  unsigned loc = builtInOutLocMap[builtInId];

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
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  (void(builtInUsage)); // unused
  storeValueToGsVsRing(output, loc, 0, streamId, insertPos);
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
    const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

    if (m_gfxIp.major <= 8 && !static_cast<bool>(enableMultiView))
      addExportInstForBuiltInOutput(output, builtInId, insertPos);
    else {
      // NOTE: The export of gl_Layer is delayed and is done before entry-point returns.
      m_layer = output;
    }

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
// @param xfbExtraOffset : Transform feedback extra offset, passed from aggregate type
// @param insertPos : Where to insert the store instruction
void PatchInOutImportExport::patchXfbOutputExport(Value *output, unsigned xfbBuffer, unsigned xfbOffset,
                                                  unsigned xfbExtraOffset, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader);

  Value *streamOutBufDesc = m_pipelineSysValues.get(m_entryPoint)->getStreamOutBufDesc(xfbBuffer);

  const auto &xfbStrides = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.xfbStrides;
  unsigned xfbStride = xfbStrides[xfbBuffer];

  auto outputTy = output->getType();
  unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() : 1;
  unsigned bitWidth = outputTy->getScalarSizeInBits();

  xfbOffset = xfbOffset + xfbExtraOffset;

  if (bitWidth == 64) {
    // Cast 64-bit output to 32-bit
    compCount *= 2;
    bitWidth = 32;
    outputTy = VectorType::get(Type::getFloatTy(*m_context), compCount);
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

    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);

    Constant *shuffleMask4567[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), 4), ConstantInt::get(Type::getInt32Ty(*m_context), 5),
        ConstantInt::get(Type::getInt32Ty(*m_context), 6), ConstantInt::get(Type::getInt32Ty(*m_context), 7)};
    compX4 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask4567), "", insertPos);

    xfbOffset += 4 * (bitWidth / 8);
    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);
  } else if (compCount == 6) {
    // vec6 -> vec4 + vec2
    assert(bitWidth == 32);

    // NOTE: This case is generated by copy shader, which casts 64-bit outputs to float.
    Constant *shuffleMask0123[] = {
        ConstantInt::get(Type::getInt32Ty(*m_context), 0), ConstantInt::get(Type::getInt32Ty(*m_context), 1),
        ConstantInt::get(Type::getInt32Ty(*m_context), 2), ConstantInt::get(Type::getInt32Ty(*m_context), 3)};
    Value *compX4 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask0123), "", insertPos);

    storeValueToStreamOutBuffer(compX4, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);

    Constant *shuffleMask45[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 4),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 5)};
    Value *compX2 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask45), "", insertPos);

    xfbOffset += 4 * (bitWidth / 8);
    storeValueToStreamOutBuffer(compX2, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);
  } else if (compCount == 3) {
    // 16vec3 -> 16vec2 + 16scalar
    // vec3 -> vec2 + scalar
    Constant *shuffleMask01[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 1)};
    Value *compX2 = new ShuffleVectorInst(output, output, ConstantVector::get(shuffleMask01), "", insertPos);

    storeValueToStreamOutBuffer(compX2, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);

    Value *comp = ExtractElementInst::Create(output, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);

    xfbOffset += 2 * (bitWidth / 8);
    storeValueToStreamOutBuffer(comp, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);
  } else {
    // 16vec4, 16vec2, 16scalar
    // vec4, vec2, scalar
    if (outputTy->isVectorTy() && compCount == 1) {
      // NOTE: We translate vec1 to scalar. SPIR-V translated from DX has such usage.
      output = ExtractElementInst::Create(output, ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);
    }

    storeValueToStreamOutBuffer(output, xfbBuffer, xfbOffset, xfbStride, streamOutBufDesc, insertPos);
  }
}

// =====================================================================================================================
// Creates the LLPC intrinsic "llpc.streamoutbuffer.store.f32" to store value to to stream-out buffer.
//
// @param storeValue : Value to store
// @param xfbStride : Transform feedback stride
// @param [out] funcName : Function name to add mangling to
void PatchInOutImportExport::createStreamOutBufferStoreFunction(Value *storeValue, unsigned xfbStride,
                                                                std::string &funcName) {
  addTypeMangling(nullptr, {storeValue}, funcName);

  // define void @llpc.streamoutbuffer.store.f32(
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
      storeValue->getType(),                            // %storeValue
      VectorType::get(Type::getInt32Ty(*m_context), 4), // %streamOutBufDesc
      Type::getInt32Ty(*m_context),                     // %writeIndex
      Type::getInt32Ty(*m_context),                     // %threadId
      Type::getInt32Ty(*m_context),                     // %vertexCount
      Type::getInt32Ty(*m_context),                     // %xfbOffset
      Type::getInt32Ty(*m_context)                      // %streamOffset
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

  unsigned compCount = storeTy->isVectorTy() ? storeTy->getVectorNumElements() : 1;
  assert(compCount <= 4);

  const uint64_t bitWidth = storeTy->getScalarSizeInBits();
  assert(bitWidth == 16 || bitWidth == 32);

  unsigned format = 0;
  std::string callName = "llvm.amdgcn.struct.tbuffer.store.";

  CombineFormat formatOprd = {};
  formatOprd.bits.nfmt = BUF_NUM_FORMAT_FLOAT;
  switch (compCount) {
  case 1: {
    formatOprd.bits.dfmt = bitWidth == 32 ? BUF_DATA_FORMAT_32 : BUF_DATA_FORMAT_16;
    callName += bitWidth == 32 ? "f32" : "f16";
    break;
  }
  case 2: {
    formatOprd.bits.dfmt = bitWidth == 32 ? BUF_DATA_FORMAT_32_32 : BUF_DATA_FORMAT_16_16;
    callName += bitWidth == 32 ? "v2f32" : "v2f16";
    break;
  }
  case 4: {
    formatOprd.bits.dfmt = bitWidth == 32 ? BUF_DATA_FORMAT_32_32_32_32 : BUF_DATA_FORMAT_16_16_16_16;
    callName += bitWidth == 32 ? "v4f32" : "v4f16";
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  format = formatOprd.u32All;

  if (m_gfxIp.major >= 10) {
    if (compCount == 4)
      format = bitWidth == 32 ? BUF_FORMAT_32_32_32_32_FLOAT : BUF_FORMAT_16_16_16_16_FLOAT;
    else if (compCount == 2)
      format = bitWidth == 32 ? BUF_FORMAT_32_32_FLOAT : BUF_FORMAT_16_16_FLOAT;
    else if (compCount == 1)
      format = bitWidth == 32 ? BUF_FORMAT_32_FLOAT : BUF_FORMAT_16_FLOAT;
    else
      llvm_unreachable("Should never be called!");
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

  std::vector<unsigned> formats;

  if (m_gfxIp.major <= 9) {
    formats = {
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32)),
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32_32)),
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32_32_32)),
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32_32_32_32)),
    };
  } else if (m_gfxIp.major == 10) {
    formats = {BUF_FORMAT_32_FLOAT, BUF_FORMAT_32_32_FLOAT, BUF_FORMAT_32_32_32_FLOAT, BUF_FORMAT_32_32_32_32_FLOAT};
  } else
    llvm_unreachable("Not implemented!");

  Type *storeTys[4] = {
      Type::getInt32Ty(*m_context),
      VectorType::get(Type::getInt32Ty(*m_context), 2),
      VectorType::get(Type::getInt32Ty(*m_context), 3),
      VectorType::get(Type::getInt32Ty(*m_context), 4),
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
        auto storeTy = VectorType::get(Type::getInt32Ty(*m_context), compCount);
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
          storeValue,                                                             // vdata
          bufDesc,                                                                // rsrc
          writeOffset,                                                            // voffset
          bufBase,                                                                // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), formats[compCount - 1]), // format
          ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All)         // glc
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
  std::vector<unsigned> formats;

  if (m_gfxIp.major <= 9) {
    formats = {
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32)),
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32_32)),
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32_32_32)),
        ((BUF_NUM_FORMAT_FLOAT << 4) | (BUF_DATA_FORMAT_32_32_32_32)),
    };
  } else if (m_gfxIp.major == 10) {
    formats = {BUF_FORMAT_32_FLOAT, BUF_FORMAT_32_32_FLOAT, BUF_FORMAT_32_32_32_FLOAT, BUF_FORMAT_32_32_32_32_FLOAT};
  } else
    llvm_unreachable("Not implemented!");

  Type *loadTyps[4] = {
      Type::getInt32Ty(*m_context),
      VectorType::get(Type::getInt32Ty(*m_context), 2),
      VectorType::get(Type::getInt32Ty(*m_context), 3),
      VectorType::get(Type::getInt32Ty(*m_context), 4),
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
          bufDesc,                                                                // rsrc
          writeOffset,                                                            // voffset
          bufBase,                                                                // soffset
          ConstantInt::get(Type::getInt32Ty(*m_context), formats[compCount - 1]), // format
          ConstantInt::get(Type::getInt32Ty(*m_context), coherent.u32All)         // glc
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
// @param streamOutBufDesc : Transform feedback buffer descriptor
// @param insertPos : Where to insert the store instruction
void PatchInOutImportExport::storeValueToStreamOutBuffer(Value *storeValue, unsigned xfbBuffer, unsigned xfbOffset,
                                                         unsigned xfbStride, Value *streamOutBufDesc,
                                                         Instruction *insertPos) {
  auto storeTy = storeValue->getType();

  unsigned compCount = storeTy->isVectorTy() ? storeTy->getVectorNumElements() : 1;
  assert(compCount <= 4);

  const uint64_t bitWidth = storeTy->getScalarSizeInBits();
  assert(bitWidth == 16 || bitWidth == 32);

  if (storeTy->isIntOrIntVectorTy()) {
    Type *bitCastTy = bitWidth == 32 ? Type::getFloatTy(*m_context) : Type::getHalfTy(*m_context);
    if (compCount > 1)
      bitCastTy = VectorType::get(bitCastTy, compCount);
    storeValue = new BitCastInst(storeValue, bitCastTy, "", insertPos);
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

    auto &inoutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
    unsigned streamOffset = CopyShaderUserSgprIdxStreamOffset;

    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (inoutUsage.xfbStrides[i] > 0)
        streamOffsets[i] = streamOffset++;
    }
  }

  assert(xfbBuffer < MaxTransformFeedbackBuffers);
  assert(streamOffsets[xfbBuffer] != 0);

  auto streamOffset = getFunctionArgument(m_entryPoint, streamOffsets[xfbBuffer]);

  streamOffset =
      BinaryOperator::CreateMul(streamOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  // vertexCount = streamInfo[22:16]
  Value *ubfeArgs[] = {getFunctionArgument(m_entryPoint, streamInfo),
                       ConstantInt::get(Type::getInt32Ty(*m_context), 16),
                       ConstantInt::get(Type::getInt32Ty(*m_context), 7)};
  Value *vertexCount = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), ubfeArgs, {}, &*insertPos);

  // Setup write index for stream-out
  auto writeIndexVal = getFunctionArgument(m_entryPoint, writeIndex);

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
    elemTy = storeTy->getArrayElementType();
  else if (storeTy->isVectorTy())
    elemTy = storeTy->getVectorElementType();

  const uint64_t bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  if (storeTy->isArrayTy() || storeTy->isVectorTy()) {
    const unsigned elemCount = storeTy->isArrayTy() ? storeTy->getArrayNumElements() : storeTy->getVectorNumElements();

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
      Value *storePtr = GetElementPtrInst::Create(nullptr, m_lds, idxs, "", insertPos);
      auto storeInst = new StoreInst(storeValue, storePtr, false, insertPos);
      storeInst->setAlignment(MaybeAlign(m_lds->getAlignment()));
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
    elemTy = loadTy->getArrayElementType();
  else if (loadTy->isVectorTy())
    elemTy = loadTy->getVectorElementType();

  const uint64_t bitWidth = elemTy->getScalarSizeInBits();
  assert((elemTy->isFloatingPointTy() || elemTy->isIntegerTy()) && (bitWidth == 8 || bitWidth == 16 || bitWidth == 32));

  Value *loadValue = UndefValue::get(loadTy);

  if (loadTy->isArrayTy() || loadTy->isVectorTy()) {
    const unsigned elemCount = loadTy->isArrayTy() ? loadTy->getArrayNumElements() : loadTy->getVectorNumElements();

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
      Value *loadPtr = GetElementPtrInst::Create(nullptr, m_lds, idxs, "", insertPos);
      auto loadInst = new LoadInst(loadPtr, "", false, insertPos);
      loadInst->setAlignment(MaybeAlign(m_lds->getAlignment()));
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
    elemTy = storeTy->getArrayElementType();
  else if (storeTy->isVectorTy())
    elemTy = storeTy->getVectorElementType();

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
    const unsigned elemCount = storeTy->isArrayTy() ? storeTy->getArrayNumElements() : storeTy->getVectorNumElements();

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
      // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend BYTE/WORD
      // to DWORD. This is because copy shader does not know the actual data type. It only generates output
      // export calls based on number of DWORDs.
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

    auto emitCounterPtr = m_pipelineSysValues.get(m_entryPoint)->getEmitCounterPtr()[streamId];
    auto emitCounter = new LoadInst(emitCounterPtr, "", insertPos);

    auto ringOffset = calcGsVsRingOffsetForOutput(location, compIdx, streamId, emitCounter, gsVsOffset, insertPos);

    if (m_pipelineState->isGsOnChip()) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ringOffset};
      Value *storePtr = GetElementPtrInst::Create(nullptr, m_lds, idxs, "", insertPos);
      auto storeInst = new StoreInst(storeValue, storePtr, false, insertPos);
      storeInst->setAlignment(MaybeAlign(m_lds->getAlignment()));
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
      } else if (m_gfxIp.major == 10) {
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
      } else
        llvm_unreachable("Not implemented!");
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
    //              (vertexIdx * vertexSizePerStream) + location * 4 + compIdx + streamBase (in DWORDS)

    auto esGsLdsSize = ConstantInt::get(Type::getInt32Ty(*m_context), resUsage->inOutUsage.gs.calcFactor.esGsLdsSize);

    gsVsOffset = BinaryOperator::CreateExact(Instruction::LShr, gsVsOffset,
                                             ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);

    auto ringItemOffset = BinaryOperator::CreateMul(
        m_threadId, ConstantInt::get(Type::getInt32Ty(*m_context), resUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize),
        "", insertPos);

    // VertexSize is stream output vertexSize x 4 (in DWORDS)
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
// @param isOutput : is the value from output variable
// @param readTy : Type of value read from LDS
// @param ldsOffset : Start offset to do LDS read operations
// @param insertPos : Where to insert read instructions
Value *PatchInOutImportExport::readValueFromLds(bool isOutput, Type *readTy, Value *ldsOffset, Instruction *insertPos) {
  assert(m_lds);
  assert(readTy->isSingleValueType());

  // Read DWORDs from LDS
  const unsigned compCount = readTy->isVectorTy() ? readTy->getVectorNumElements() : 1;
  const unsigned bitWidth = readTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);
  const unsigned numChannels = compCount * (bitWidth == 64 ? 2 : 1);

  std::vector<Value *> loadValues(numChannels);

  const bool isTcsOutput = (isOutput && m_shaderStage == ShaderStageTessControl);
  const bool isTesInput = (!isOutput && m_shaderStage == ShaderStageTessEval);

  if (m_pipelineState->isTessOffChip() && (isTcsOutput || isTesInput)) // Read from off-chip LDS buffer
  {
    const auto &offChipLdsBaseArgIdx =
        m_shaderStage == ShaderStageTessEval
            ? m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tes.offChipLdsBase
            : m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs.offChipLdsBase;

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, offChipLdsBaseArgIdx);

    // Convert DWORD off-chip LDS offset to byte offset
    ldsOffset = BinaryOperator::CreateMul(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

    CoherentFlag coherent = {};
    if (m_gfxIp.major <= 9)
      coherent.bits.glc = true;
    else if (m_gfxIp.major == 10) {
      coherent.bits.glc = true;
      coherent.bits.dlc = true;
    } else
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
  } else // Read from on-chip LDS
  {
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ldsOffset};
      Value *loadPtr = GetElementPtrInst::Create(nullptr, m_lds, idxs, "", insertPos);
      auto loadInst = new LoadInst(loadPtr, "", false, insertPos);
      loadInst->setAlignment(MaybeAlign(m_lds->getAlignment()));
      loadValues[i] = loadInst;

      if (bitWidth == 8)
        loadValues[i] = new TruncInst(loadValues[i], Type::getInt8Ty(*m_context), "", insertPos);
      else if (bitWidth == 16)
        loadValues[i] = new TruncInst(loadValues[i], Type::getInt16Ty(*m_context), "", insertPos);

      ldsOffset =
          BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
    }
  }

  // Construct <n x i8>, <n x i16>, or <n x i32> vector from load values (DWORDs)
  Value *castValue = nullptr;
  if (numChannels > 1) {
    auto intTy = bitWidth == 32 || bitWidth == 64
                     ? Type::getInt32Ty(*m_context)
                     : (bitWidth == 16 ? Type::getInt16Ty(*m_context) : Type::getInt8Ty(*m_context));
    auto castTy = VectorType::get(intTy, numChannels);
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
// @param writeValue : Value written to LDS
// @param ldsOffset : Start offset to do LDS write operations
// @param insertPos : Where to insert write instructions
void PatchInOutImportExport::writeValueToLds(Value *writeValue, Value *ldsOffset, Instruction *insertPos) {
  assert(m_lds);

  auto writeTy = writeValue->getType();
  assert(writeTy->isSingleValueType());

  const unsigned compCout = writeTy->isVectorTy() ? writeTy->getVectorNumElements() : 1;
  const unsigned bitWidth = writeTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);
  const unsigned numChannels = compCout * (bitWidth == 64 ? 2 : 1);

  // Cast write value to <n x i32> vector
  Type *intTy = bitWidth == 32 || bitWidth == 64
                    ? Type::getInt32Ty(*m_context)
                    : (bitWidth == 16 ? Type::getInt16Ty(*m_context) : Type::getInt8Ty(*m_context));
  Type *castTy = numChannels > 1 ? cast<Type>(VectorType::get(intTy, numChannels)) : intTy;
  Value *castValue = new BitCastInst(writeValue, castTy, "", insertPos);

  // Extract store values (DWORDs) from <n x i8>, <n x i16> or <n x i32> vector
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

  if (m_pipelineState->isTessOffChip() && m_shaderStage == ShaderStageTessControl) // Write to off-chip LDS buffer
  {
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.tcs;

    auto offChipLdsBase = getFunctionArgument(m_entryPoint, entryArgIdxs.offChipLdsBase);
    // Convert DWORD off-chip LDS offset to byte offset
    ldsOffset = BinaryOperator::CreateMul(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

    auto offChipLdsDesc = m_pipelineSysValues.get(m_entryPoint)->getOffChipLdsDesc();

    CoherentFlag coherent = {};
    coherent.bits.glc = true;

    for (unsigned i = 0, combineCount = 0; i < numChannels; i += combineCount) {
      combineCount =
          combineBufferStore(storeValues, i, i, offChipLdsDesc, ldsOffset, offChipLdsBase, coherent, insertPos);
    }
  } else // Write to on-chip LDS
  {
    for (unsigned i = 0; i < numChannels; ++i) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), ldsOffset};
      Value *storePtr = GetElementPtrInst::Create(nullptr, m_lds, idxs, "", insertPos);
      auto storeInst = new StoreInst(storeValues[i], storePtr, false, insertPos);
      storeInst->setAlignment(MaybeAlign(m_lds->getAlignment()));

      ldsOffset =
          BinaryOperator::CreateAdd(ldsOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);
    }
  }
}

// =====================================================================================================================
// Calculates start offset of tessellation factors in the TF buffer.
//
// @param isOuter : Whether the calculation is for tessellation outer factors
// @param elemIdxVal : Index used for array element indexing (could be null)
// @param insertPos : Where to insert store instructions
Value *PatchInOutImportExport::calcTessFactorOffset(bool isOuter, Value *elemIdxVal, Instruction *insertPos) {
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

  unsigned tessFactorCount = 0;
  unsigned tessFactorStart = 0;
  auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
  switch (primitiveMode) {
  case PrimitiveMode::Isolines:
    tessFactorCount = isOuter ? 2 : 0;
    tessFactorStart = isOuter ? 0 : 2;
    break;
  case PrimitiveMode::Triangles:
    tessFactorCount = isOuter ? 3 : 1;
    tessFactorStart = isOuter ? 0 : 3;
    break;
  case PrimitiveMode::Quads:
    tessFactorCount = isOuter ? 4 : 2;
    tessFactorStart = isOuter ? 0 : 4;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  Value *tessFactorOffset = ConstantInt::get(Type::getInt32Ty(*m_context), tessFactorStart);
  if (elemIdxVal) {
    if (isa<ConstantInt>(elemIdxVal)) {
      // Constant element indexing
      unsigned elemIdx = cast<ConstantInt>(elemIdxVal)->getZExtValue();
      if (elemIdx < tessFactorCount) {
        if (primitiveMode == PrimitiveMode::Isolines && isOuter) {
          // NOTE: In case of the isoline,  hardware wants two tessellation factor: the first is detail
          // TF, the second is density TF. The order is reversed, different from GLSL spec.
          assert(tessFactorCount == 2);
          elemIdx = 1 - elemIdx;
        }

        tessFactorOffset = ConstantInt::get(Type::getInt32Ty(*m_context), tessFactorStart + elemIdx);
      } else {
        // Out of range, drop it
        tessFactorOffset = ConstantInt::get(Type::getInt32Ty(*m_context), InvalidValue);
      }
    } else {
      // Dynamic element indexing
      if (primitiveMode == PrimitiveMode::Isolines && isOuter) {
        // NOTE: In case of the isoline,  hardware wants two tessellation factor: the first is detail
        // TF, the second is density TF. The order is reversed, different from GLSL spec.
        assert(tessFactorCount == 2);

        // elemIdx = (elemIdx <= 1) ? 1 - elemIdx : elemIdx
        auto cond = new ICmpInst(insertPos, ICmpInst::ICMP_ULE, elemIdxVal,
                                 ConstantInt::get(Type::getInt32Ty(*m_context), 1), "");

        auto swapElemIdx =
            BinaryOperator::CreateSub(ConstantInt::get(Type::getInt32Ty(*m_context), 1), elemIdxVal, "", insertPos);

        elemIdxVal = SelectInst::Create(cond, swapElemIdx, elemIdxVal, "", insertPos);
      }

      // tessFactorOffset = (elemIdx < tessFactorCount) ? (tessFactorStart + elemIdx) : invalidValue
      tessFactorOffset = BinaryOperator::CreateAdd(tessFactorOffset, elemIdxVal, "", insertPos);

      auto cond = new ICmpInst(insertPos, ICmpInst::ICMP_ULT, elemIdxVal,
                               ConstantInt::get(Type::getInt32Ty(*m_context), tessFactorCount), "");

      tessFactorOffset = SelectInst::Create(
          cond, tessFactorOffset, ConstantInt::get(Type::getInt32Ty(*m_context), InvalidValue), "", insertPos);
    }
  }

  return tessFactorOffset;
}

// =====================================================================================================================
// Stores tessellation factors (outer/inner) to corresponding tessellation factor (TF) buffer.
//
// @param tessFactors : Tessellation factors to be stored
// @param tessFactorOffsetVal : Start offset to store the specified tessellation factors
// @param insertPos : Where to insert store instructions
void PatchInOutImportExport::storeTessFactorToBuffer(const std::vector<Value *> &tessFactors,
                                                     Value *tessFactorOffsetVal, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessControl);

  if (tessFactors.size() == 0) {
    // No tessellation factor should be stored
    return;
  }

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs;
  const auto &calcFactor = inOutUsage.calcFactor;

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl)->entryArgIdxs.tcs;
  auto tfBufferBase = getFunctionArgument(m_entryPoint, entryArgIdxs.tfBufferBase);

  auto tessFactorStride = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.tessFactorStride);

  if (isa<ConstantInt>(tessFactorOffsetVal)) {
    unsigned tessFactorOffset = cast<ConstantInt>(tessFactorOffsetVal)->getZExtValue();
    if (tessFactorOffset == InvalidValue) {
      // Out of range, drop it
      return;
    }

    auto relativeId = m_pipelineSysValues.get(m_entryPoint)->getRelativeId();
    Value *tfBufferOffset = BinaryOperator::CreateMul(relativeId, tessFactorStride, "", insertPos);
    tfBufferOffset =
        BinaryOperator::CreateMul(tfBufferOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

    auto tfBufDesc = m_pipelineSysValues.get(m_entryPoint)->getTessFactorBufDesc();
    std::vector<Value *> tfValues(tessFactors.size());
    for (unsigned i = 0; i < tessFactors.size(); i++)
      tfValues[i] = new BitCastInst(tessFactors[i], Type::getInt32Ty(*m_context), "", insertPos);

    CoherentFlag coherent = {};
    coherent.bits.glc = true;

    for (unsigned i = 0, combineCount = 0; i < tessFactors.size(); i += combineCount) {
      unsigned tfValueOffset = i + tessFactorOffset;
      if (m_gfxIp.major <= 8) {
        // NOTE: Additional 4-byte offset is required for tessellation off-chip mode (pre-GFX9).
        tfValueOffset += (m_pipelineState->isTessOffChip() ? 1 : 0);
      }
      combineCount =
          combineBufferStore(tfValues, i, tfValueOffset, tfBufDesc, tfBufferOffset, tfBufferBase, coherent, insertPos);
    }
  } else {
    // Must be element indexing of tessellation level array
    assert(tessFactors.size() == 1);

    if (!m_module->getFunction(lgcName::TfBufferStore))
      createTessBufferStoreFunction();

    if (m_pipelineState->isTessOffChip()) {
      if (m_gfxIp.major <= 8) {
        // NOTE: Additional 4-byte offset is required for tessellation off-chip mode (pre-GFX9).
        tfBufferBase =
            BinaryOperator::CreateAdd(tfBufferBase, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);
      }
    }

    Value *args[] = {
        m_pipelineSysValues.get(m_entryPoint)->getTessFactorBufDesc(), // tfBufferDesc
        tfBufferBase,                                                  // tfBufferBase
        m_pipelineSysValues.get(m_entryPoint)->getRelativeId(),        // relPatchId
        tessFactorStride,                                              // tfStride
        tessFactorOffsetVal,                                           // tfOffset
        tessFactors[0]                                                 // tfValue
    };
    emitCall(lgcName::TfBufferStore, Type::getVoidTy(*m_context), args, {}, insertPos);
  }
}

// =====================================================================================================================
// Creates the LLPC intrinsic "llpc.tfbuffer.store.f32" to store tessellation factor (dynamic element indexing for
// tessellation level array).
void PatchInOutImportExport::createTessBufferStoreFunction() {
  // define void @llpc.tfbuffer.store.f32(
  //     <4 x i32> %tfBufferDesc, i32 %tfBufferBase, i32 %relPatchId, i32 %tfStride, i32 %tfOffset, float %tfValue)
  // {
  //     %1 = icmp ne i32 %tfOffset, -1 (invalidValue)
  //     br i1 %1, label %.tfstore, label %.end
  //
  // .tfstore:
  //     %2 = mul i32 %tfStride, 4
  //     %3 = mul i32 %relPatchId, %2
  //     %4 = mul i32 %tfOffset, 4
  //     %5 = add i32 %3, %4
  //     %6 = add i32 %tfBufferBase, %5
  //     call void @llvm.amdgcn.raw.buffer.store.f32(
  //         float %tfValue, <4 x i32> %tfBufferDesc, i32 %6, i32 0, i32 1)
  //     br label %.end
  //
  // .end:
  //     ret void
  // }
  Type *argTys[] = {
      VectorType::get(Type::getInt32Ty(*m_context), 4), // TF buffer descriptor
      Type::getInt32Ty(*m_context),                     // TF buffer base
      Type::getInt32Ty(*m_context),                     // Relative patch ID
      Type::getInt32Ty(*m_context),                     // TF stride
      Type::getInt32Ty(*m_context),                     // TF offset
      Type::getFloatTy(*m_context)                      // TF value
  };
  auto funcTy = FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::TfBufferStore, m_module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::NoUnwind);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();

  Value *tfBufferDesc = argIt++;
  tfBufferDesc->setName("tfBufferDesc");

  Value *tfBufferBase = argIt++;
  tfBufferBase->setName("tfBufferBase");

  Value *relPatchId = argIt++;
  relPatchId->setName("relPatchId");

  Value *tfStride = argIt++;
  tfStride->setName("tfStride");

  Value *tfOffset = argIt++;
  tfOffset->setName("tfOffset");

  Value *tfValue = argIt++;
  tfValue->setName("tfValue");

  // Create ".end" block
  BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);
  ReturnInst::Create(*m_context, endBlock);

  // Create ".tfstore" block
  BasicBlock *tfStoreBlock = BasicBlock::Create(*m_context, ".tfstore", func, endBlock);

  Value *tfByteOffset =
      BinaryOperator::CreateMul(tfOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", tfStoreBlock);

  Value *tfByteStride =
      BinaryOperator::CreateMul(tfStride, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", tfStoreBlock);
  Value *tfBufferOffset = BinaryOperator::CreateMul(relPatchId, tfByteStride, "", tfStoreBlock);

  tfBufferOffset = BinaryOperator::CreateAdd(tfBufferOffset, tfByteOffset, "", tfStoreBlock);
  tfBufferOffset = BinaryOperator::CreateAdd(tfBufferOffset, tfBufferBase, "", tfStoreBlock);

  auto branch = BranchInst::Create(endBlock, tfStoreBlock);

  Value *args[] = {
      tfValue,                                           // vdata
      tfBufferDesc,                                      // rsrc
      tfBufferOffset,                                    // offset
      ConstantInt::get(Type::getInt32Ty(*m_context), 0), // soffset
      ConstantInt::get(Type::getInt32Ty(*m_context), 1)  // cachepolicy: glc = 1
  };
  emitCall("llvm.amdgcn.raw.buffer.store.f32", Type::getVoidTy(*m_context), args, {}, branch);

  // Create entry block
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func, tfStoreBlock);
  Value *cond = new ICmpInst(*entryBlock, ICmpInst::ICMP_NE, tfOffset,
                             ConstantInt::get(Type::getInt32Ty(*m_context), InvalidValue), "");
  BranchInst::Create(tfStoreBlock, endBlock, cond, entryBlock);
}

// =====================================================================================================================
// Calculates the DWORD offset to write value to LDS based on the specified VS output info.
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
// Calculates the DWORD offset to read value from LDS based on the specified TCS input info.
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
// Calculates the DWORD offset to read/write value from/to LDS based on the specified TCS output info.
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
  Value *attibOffset = ConstantInt::get(Type::getInt32Ty(*m_context), location);

  if (locOffset)
    attibOffset = BinaryOperator::CreateAdd(attibOffset, locOffset, "", insertPos);

  attibOffset =
      BinaryOperator::CreateMul(attibOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  if (compIdx) {
    const unsigned bitWidth = outputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = BinaryOperator::CreateMul(compIdx, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    }

    attibOffset = BinaryOperator::CreateAdd(attibOffset, compIdx, "", insertPos);
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

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attibOffset, "", insertPos);
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

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attibOffset, "", insertPos);
  }

  return ldsOffset;
}

// =====================================================================================================================
// Calculates the DWORD offset to read/write value from/to LDS based on the specified TES input info.
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
  Value *attibOffset = ConstantInt::get(Type::getInt32Ty(*m_context), location);

  if (locOffset)
    attibOffset = BinaryOperator::CreateAdd(attibOffset, locOffset, "", insertPos);

  attibOffset =
      BinaryOperator::CreateMul(attibOffset, ConstantInt::get(Type::getInt32Ty(*m_context), 4), "", insertPos);

  if (compIdx) {
    const unsigned bitWidth = inputTy->getScalarSizeInBits();
    assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

    if (bitWidth == 64) {
      // For 64-bit data type, the component indexing must multiply by 2
      compIdx = BinaryOperator::CreateMul(compIdx, ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    }

    attibOffset = BinaryOperator::CreateAdd(attibOffset, compIdx, "", insertPos);
  }

  Value *ldsOffset = nullptr;

  const bool perPatch = (!vertexIdx); // Vertex indexing is unavailable for per-patch input
  if (perPatch) {
    // dwordOffset = patchConstStart + relPatchId * patchConstSize + attribOffset
    auto patchConstSize = ConstantInt::get(Type::getInt32Ty(*m_context), calcFactor.patchConstSize);
    ldsOffset = BinaryOperator::CreateMul(relPatchId, patchConstSize, "", insertPos);

    auto patchConstStartVal = ConstantInt::get(Type::getInt32Ty(*m_context), patchConstStart);
    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, patchConstStartVal, "", insertPos);

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attibOffset, "", insertPos);
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

    ldsOffset = BinaryOperator::CreateAdd(ldsOffset, attibOffset, "", insertPos);
  }

  return ldsOffset;
}

// =====================================================================================================================
// Calculates the patch count for per-thread group.
//
// @param inVertexCount : Count of vertices of input patch
// @param inVertexStride : Vertex stride of input patch in (DWORDs)
// @param outVertexCount : Count of vertices of output patch
// @param outVertexStride : Vertex stride of output patch in (DWORDs)
// @param patchConstCount : Count of output patch constants
// @param tessFactorStride : Stride of tessellation factors (DWORDs)
unsigned PatchInOutImportExport::calcPatchCountPerThreadGroup(unsigned inVertexCount, unsigned inVertexStride,
                                                              unsigned outVertexCount, unsigned outVertexStride,
                                                              unsigned patchConstCount,
                                                              unsigned tessFactorStride) const {
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);

  // NOTE: The limit of thread count for tessellation control shader is 4 wavefronts per thread group.
  const unsigned maxThreadCountPerThreadGroup = (4 * waveSize);
  const unsigned maxThreadCountPerPatch = std::max(inVertexCount, outVertexCount);
  const unsigned patchCountLimitedByThread = maxThreadCountPerThreadGroup / maxThreadCountPerPatch;

  const unsigned inPatchSize = (inVertexCount * inVertexStride);
  const unsigned outPatchSize = (outVertexCount * outVertexStride);
  const unsigned patchConstSize = patchConstCount * 4;

  // Compute the required LDS size per patch, always include the space for VS vertex out
  unsigned ldsSizePerPatch = inPatchSize;
  unsigned patchCountLimitedByLds =
      (m_pipelineState->getTargetInfo().getGpuProperty().ldsSizePerThreadGroup / ldsSizePerPatch);

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

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;

  const unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() : 1;
  const unsigned bitWidth = outputTy->getScalarSizeInBits();
  assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32 || bitWidth == 64);

  // Convert the output value to floating-point export value
  Value *exportInst = nullptr;
  const unsigned numChannels = bitWidth == 64 ? compCount * 2 : compCount;
  unsigned startChannel = bitWidth == 64 ? compIdx * 2 : compIdx;
  Type *exportTy =
      numChannels > 1 ? VectorType::get(Type::getFloatTy(*m_context), numChannels) : Type::getFloatTy(*m_context);

  if (outputTy != exportTy) {
    if (bitWidth == 8) {
      // NOTE: For 16-bit output export, we have to cast the 8-bit value to 32-bit floating-point value.
      assert(outputTy->isIntOrIntVectorTy());
      Type *zExtTy = Type::getInt32Ty(*m_context);
      zExtTy = outputTy->isVectorTy() ? cast<Type>(VectorType::get(zExtTy, compCount)) : zExtTy;
      exportInst = new ZExtInst(output, zExtTy, "", insertPos);
      exportInst = new BitCastInst(exportInst, exportTy, "", insertPos);
    } else if (bitWidth == 16) {
      // NOTE: For 16-bit output export, we have to cast the 16-bit value to 32-bit floating-point value.
      if (outputTy->isFPOrFPVectorTy()) {
        Type *bitCastTy = Type::getInt16Ty(*m_context);
        bitCastTy = outputTy->isVectorTy() ? cast<Type>(VectorType::get(bitCastTy, compCount)) : bitCastTy;
        exportInst = new BitCastInst(output, bitCastTy, "", insertPos);
      } else {
        assert(outputTy->isIntOrIntVectorTy());
        exportInst = output;
      }

      Type *zExtTy = Type::getInt32Ty(*m_context);
      zExtTy = outputTy->isVectorTy() ? cast<Type>(VectorType::get(zExtTy, compCount)) : zExtTy;
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

  std::vector<Value *> args;

  if (numChannels <= 4) {
    assert(startChannel + numChannels <= 4);
    const unsigned channelMask = ((1 << (startChannel + numChannels)) - 1) - ((1 << startChannel) - 1);

    args.clear();
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + location)); // tgt
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), channelMask));                   // en

    // src0 ~ src3
    for (unsigned i = 0; i < startChannel; ++i) {
      // Inactive components (dummy)
      args.push_back(UndefValue::get(Type::getFloatTy(*m_context)));
    }

    for (unsigned i = startChannel; i < startChannel + numChannels; ++i)
      args.push_back(exportValues[i - startChannel]);

    for (unsigned i = startChannel + numChannels; i < 4; ++i) {
      // Inactive components (dummy)
      args.push_back(UndefValue::get(Type::getFloatTy(*m_context)));
    }

    args.push_back(ConstantInt::get(Type::getInt1Ty(*m_context), false)); // done
    args.push_back(ConstantInt::get(Type::getInt1Ty(*m_context), false)); // vm

    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    ++inOutUsage.expCount;
  } else {
    // We have to do exporting twice for this output
    assert(startChannel == 0); // Other values are disallowed according to GLSL spec
    assert(numChannels == 6 || numChannels == 8);

    // Do the first exporting
    args.clear();
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + location)); // tgt
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0xF));                           // en

    // src0 ~ src3
    for (unsigned i = 0; i < 4; ++i)
      args.push_back(exportValues[i]);

    args.push_back(ConstantInt::get(Type::getInt1Ty(*m_context), false)); // done
    args.push_back(ConstantInt::get(Type::getInt1Ty(*m_context), false)); // vm

    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    ++inOutUsage.expCount;

    // Do the second exporting
    const unsigned channelMask = ((1 << (numChannels - 4)) - 1);

    args.clear();
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + location + 1)); // tgt
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), channelMask));                       // en

    // src0 ~ src3
    for (unsigned i = 4; i < numChannels; ++i)
      args.push_back(exportValues[i]);

    for (unsigned i = numChannels; i < 8; ++i) {
      // Inactive components (dummy)
      args.push_back(UndefValue::get(Type::getFloatTy(*m_context)));
    }

    args.push_back(ConstantInt::get(Type::getInt1Ty(*m_context), false)); // done
    args.push_back(ConstantInt::get(Type::getInt1Ty(*m_context), false)); // vm

    emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
    ++inOutUsage.expCount;
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
                           (nextStage == ShaderStageInvalid || nextStage == ShaderStageFragment));
  assert(useExpInst);
  (void(useExpInst)); // unused

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;

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

    // "Done" flag is valid for exporting position 0 ~ 3
    m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
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
    // "Done" flag is valid for exporting position 0 ~ 3
    m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
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
    // "Done" flag is valid for exporting position 0 ~ 3
    m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

    // NOTE: We have to export gl_Layer via generic outputs as well.
    bool hasLayerExport = true;
    if (nextStage == ShaderStageFragment) {
      const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

      hasLayerExport = nextBuiltInUsage.layer || nextBuiltInUsage.viewIndex;
    }

    if (hasLayerExport) {
      unsigned loc = InvalidValue;
      if (m_shaderStage == ShaderStageCopyShader) {
        assert(inOutUsage.gs.builtInOutLocs.find(BuiltInLayer) != inOutUsage.gs.builtInOutLocs.end() ||
               inOutUsage.gs.builtInOutLocs.find(BuiltInViewIndex) != inOutUsage.gs.builtInOutLocs.end());
        loc = enableMultiView ? inOutUsage.gs.builtInOutLocs[BuiltInViewIndex]
                              : inOutUsage.gs.builtInOutLocs[BuiltInLayer];
      } else {
        assert(inOutUsage.builtInOutputLocMap.find(BuiltInLayer) != inOutUsage.builtInOutputLocMap.end() ||
               inOutUsage.builtInOutputLocMap.find(BuiltInViewIndex) != inOutUsage.builtInOutputLocMap.end());

        loc = enableMultiView ? inOutUsage.builtInOutputLocMap[BuiltInViewIndex]
                              : inOutUsage.builtInOutputLocMap[BuiltInLayer];
      }

      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),                      // en
          layer,                                                                    // src0
          undef,                                                                    // src1
          undef,                                                                    // src2
          undef,                                                                    // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),                     // done
          ConstantInt::get(Type::getInt1Ty(*m_context), false)                      // vm
      };
      emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
      ++inOutUsage.expCount;
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
    // "Done" flag is valid for exporting position 0 ~ 3
    m_lastExport = emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);

    // NOTE: We have to export gl_ViewportIndex via generic outputs as well.
    bool hasViewportIndexExport = true;
    if (nextStage == ShaderStageFragment) {
      const auto &nextBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageFragment)->builtInUsage.fs;

      hasViewportIndexExport = nextBuiltInUsage.viewportIndex;
    }

    if (hasViewportIndexExport) {
      unsigned loc = InvalidValue;
      if (m_shaderStage == ShaderStageCopyShader) {
        assert(inOutUsage.gs.builtInOutLocs.find(BuiltInViewportIndex) != inOutUsage.gs.builtInOutLocs.end());
        loc = inOutUsage.gs.builtInOutLocs[BuiltInViewportIndex];
      } else {
        assert(inOutUsage.builtInOutputLocMap.find(BuiltInViewportIndex) != inOutUsage.builtInOutputLocMap.end());
        loc = inOutUsage.builtInOutputLocMap[BuiltInViewportIndex];
      }

      Value *args[] = {
          ConstantInt::get(Type::getInt32Ty(*m_context), EXP_TARGET_PARAM_0 + loc), // tgt
          ConstantInt::get(Type::getInt32Ty(*m_context), 0xF),                      // en
          viewportIndex,                                                            // src0
          undef,                                                                    // src1
          undef,                                                                    // src2
          undef,                                                                    // src3
          ConstantInt::get(Type::getInt1Ty(*m_context), false),                     // done
          ConstantInt::get(Type::getInt1Ty(*m_context), false)                      // vm
      };
      emitCall("llvm.amdgcn.exp.f32", Type::getVoidTy(*m_context), args, {}, insertPos);
      ++inOutUsage.expCount;
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
// @param insertPos : Where to insert this call
Value *PatchInOutImportExport::getSubgroupLocalInvocationId(Instruction *insertPos) {
  Value *args[] = {ConstantInt::get(Type::getInt32Ty(*m_context), -1),
                   ConstantInt::get(Type::getInt32Ty(*m_context), 0)};
  Value *subgroupLocalInvocationId =
      emitCall("llvm.amdgcn.mbcnt.lo", Type::getInt32Ty(*m_context), args, {}, &*insertPos);

  unsigned waveSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
  if (waveSize == 64) {
    Value *args[] = {ConstantInt::get(Type::getInt32Ty(*m_context), -1), subgroupLocalInvocationId};
    subgroupLocalInvocationId = emitCall("llvm.amdgcn.mbcnt.hi", Type::getInt32Ty(*m_context), args, {}, &*insertPos);
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
      offset = BinaryOperator::CreateLShr(x, ConstantInt::get(int32Ty, Log2(workgroupSizeYMul8)), "", insertPos);
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
      div = BinaryOperator::CreateLShr(maskedX, ConstantInt::get(int32Ty, Log2(workgroupSizeXMul2)), "", insertPos);
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
// Get the value of compute shader built-in WorkgroupSize
Value *PatchInOutImportExport::getWorkgroupSize() {
  assert(m_shaderStage == ShaderStageCompute);

  auto &builtInUsage = m_pipelineState->getShaderModes()->getComputeShaderMode();
  auto workgroupSizeX = ConstantInt::get(Type::getInt32Ty(*m_context), builtInUsage.workgroupSizeX);
  auto workgroupSizeY = ConstantInt::get(Type::getInt32Ty(*m_context), builtInUsage.workgroupSizeY);
  auto workgroupSizeZ = ConstantInt::get(Type::getInt32Ty(*m_context), builtInUsage.workgroupSizeZ);

  return ConstantVector::get({workgroupSizeX, workgroupSizeY, workgroupSizeZ});
}

// =====================================================================================================================
// Get the value of compute shader built-in LocalInvocationId
//
// @param insertPos : Where to insert instructions.
Value *PatchInOutImportExport::getInLocalInvocationId(Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageCompute);

  auto &builtInUsage = m_pipelineState->getShaderModes()->getComputeShaderMode();
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageCompute)->entryArgIdxs.cs;
  Value *locaInvocatioId = getFunctionArgument(m_entryPoint, entryArgIdxs.localInvocationId);

  WorkgroupLayout workgroupLayout = calculateWorkgroupLayout();

  // If we do not need to configure our workgroup in linear layout and the layout info is not specified, we
  // do the reconfiguration for this workgroup.
  if (workgroupLayout != WorkgroupLayout::Unknown && workgroupLayout != WorkgroupLayout::Linear)
    locaInvocatioId = reconfigWorkgroup(locaInvocatioId, insertPos);
  else {
    if (builtInUsage.workgroupSizeZ > 1) {
      // XYZ, do nothing
    } else if (builtInUsage.workgroupSizeY > 1) {
      // XY
      locaInvocatioId = InsertElementInst::Create(locaInvocatioId, ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                                  ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    } else {
      // X
      locaInvocatioId = InsertElementInst::Create(locaInvocatioId, ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                                  ConstantInt::get(Type::getInt32Ty(*m_context), 1), "", insertPos);

      locaInvocatioId = InsertElementInst::Create(locaInvocatioId, ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                                  ConstantInt::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    }
  }
  return locaInvocatioId;
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for input import and output export.
INITIALIZE_PASS(PatchInOutImportExport, DEBUG_TYPE, "Patch LLVM for input import and output export operations", false,
                false)
