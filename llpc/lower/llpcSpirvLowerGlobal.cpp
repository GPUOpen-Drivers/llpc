/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerGlobal.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerGlobal.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerGlobal.h"
#include "SPIRVInternal.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypesMetadata.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcGraphicsContext.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <unordered_set>

#define DEBUG_TYPE "llpc-spirv-lower-global"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;
using namespace lgc::rt;

namespace Llpc {

// The code here relies on the SPIR-V built-in kind being the same as the Builder built-in kind.

static_assert(lgc::BuiltInBaryCoord == static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordKHR),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordNoPerspKHR == static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordNoPerspKHR),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordNoPersp == static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordNoPerspAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordNoPerspCentroid ==
                  static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordNoPerspCentroidAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordNoPerspSample ==
                  static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordNoPerspSampleAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordPullModel == static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordPullModelAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordSmooth == static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordSmoothAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordSmoothCentroid ==
                  static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordSmoothCentroidAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaryCoordSmoothSample == static_cast<lgc::BuiltInKind>(spv::BuiltInBaryCoordSmoothSampleAMD),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaseInstance == static_cast<lgc::BuiltInKind>(spv::BuiltInBaseInstance),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInBaseVertex == static_cast<lgc::BuiltInKind>(spv::BuiltInBaseVertex),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInClipDistance == static_cast<lgc::BuiltInKind>(spv::BuiltInClipDistance),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInCullDistance == static_cast<lgc::BuiltInKind>(spv::BuiltInCullDistance),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInDeviceIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInDeviceIndex),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInDrawIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInDrawIndex), "Built-in kind mismatch");
static_assert(lgc::BuiltInFragCoord == static_cast<lgc::BuiltInKind>(spv::BuiltInFragCoord), "Built-in kind mismatch");
static_assert(lgc::BuiltInFragDepth == static_cast<lgc::BuiltInKind>(spv::BuiltInFragDepth), "Built-in kind mismatch");
static_assert(lgc::BuiltInFragStencilRef == static_cast<lgc::BuiltInKind>(spv::BuiltInFragStencilRefEXT),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInFrontFacing == static_cast<lgc::BuiltInKind>(spv::BuiltInFrontFacing),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInGlobalInvocationId == static_cast<lgc::BuiltInKind>(spv::BuiltInGlobalInvocationId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInHelperInvocation == static_cast<lgc::BuiltInKind>(spv::BuiltInHelperInvocation),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInInstanceIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInInstanceIndex),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInInvocationId == static_cast<lgc::BuiltInKind>(spv::BuiltInInvocationId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInLayer == static_cast<lgc::BuiltInKind>(spv::BuiltInLayer), "Built-in kind mismatch");
static_assert(lgc::BuiltInLocalInvocationId == static_cast<lgc::BuiltInKind>(spv::BuiltInLocalInvocationId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInLocalInvocationIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInLocalInvocationIndex),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInNumSubgroups == static_cast<lgc::BuiltInKind>(spv::BuiltInNumSubgroups),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInNumWorkgroups == static_cast<lgc::BuiltInKind>(spv::BuiltInNumWorkgroups),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPatchVertices == static_cast<lgc::BuiltInKind>(spv::BuiltInPatchVertices),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPointCoord == static_cast<lgc::BuiltInKind>(spv::BuiltInPointCoord),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPointSize == static_cast<lgc::BuiltInKind>(spv::BuiltInPointSize), "Built-in kind mismatch");
static_assert(lgc::BuiltInPosition == static_cast<lgc::BuiltInKind>(spv::BuiltInPosition), "Built-in kind mismatch");
static_assert(lgc::BuiltInPrimitiveId == static_cast<lgc::BuiltInKind>(spv::BuiltInPrimitiveId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSampleId == static_cast<lgc::BuiltInKind>(spv::BuiltInSampleId), "Built-in kind mismatch");
static_assert(lgc::BuiltInSampleMask == static_cast<lgc::BuiltInKind>(spv::BuiltInSampleMask),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSamplePosition == static_cast<lgc::BuiltInKind>(spv::BuiltInSamplePosition),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupEqMask == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupEqMask),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupGeMask == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupGeMask),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupGtMask == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupGtMask),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupId == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupLeMask == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupLeMask),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupLocalInvocationId ==
                  static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupLocalInvocationId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupLtMask == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupLtMask),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInSubgroupSize == static_cast<lgc::BuiltInKind>(spv::BuiltInSubgroupSize),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInTessCoord == static_cast<lgc::BuiltInKind>(spv::BuiltInTessCoord), "Built-in kind mismatch");
static_assert(lgc::BuiltInTessLevelInner == static_cast<lgc::BuiltInKind>(spv::BuiltInTessLevelInner),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInTessLevelOuter == static_cast<lgc::BuiltInKind>(spv::BuiltInTessLevelOuter),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInVertexIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInVertexIndex),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInViewIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInViewIndex), "Built-in kind mismatch");
static_assert(lgc::BuiltInViewportIndex == static_cast<lgc::BuiltInKind>(spv::BuiltInViewportIndex),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInWorkgroupId == static_cast<lgc::BuiltInKind>(spv::BuiltInWorkgroupId),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPrimitiveShadingRate == static_cast<lgc::BuiltInKind>(spv::BuiltInPrimitiveShadingRateKHR),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInShadingRate == static_cast<lgc::BuiltInKind>(spv::BuiltInShadingRateKHR),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInCullPrimitive == static_cast<lgc::BuiltInKind>(spv::BuiltInCullPrimitiveEXT),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPrimitivePointIndices == static_cast<lgc::BuiltInKind>(spv::BuiltInPrimitivePointIndicesEXT),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPrimitiveLineIndices == static_cast<lgc::BuiltInKind>(spv::BuiltInPrimitiveLineIndicesEXT),
              "Built-in kind mismatch");
static_assert(lgc::BuiltInPrimitiveTriangleIndices ==
                  static_cast<lgc::BuiltInKind>(spv::BuiltInPrimitiveTriangleIndicesEXT),
              "Built-in kind mismatch");

static_assert(lgc::ShadingRateNone == static_cast<lgc::ShadingRateFlags>(spv::FragmentShadingRateMaskNone),
              "Shading rate flag mismatch");
static_assert(lgc::ShadingRateVertical2Pixels ==
                  static_cast<lgc::ShadingRateFlags>(spv::FragmentShadingRateVertical2PixelsMask),
              "Shading rate flag mismatch");
static_assert(lgc::ShadingRateVertical4Pixels ==
                  static_cast<lgc::ShadingRateFlags>(spv::FragmentShadingRateVertical4PixelsMask),
              "Shading rate flag mismatch");
static_assert(lgc::ShadingRateHorizontal2Pixels ==
                  static_cast<lgc::ShadingRateFlags>(spv::FragmentShadingRateHorizontal2PixelsMask),
              "Shading rate flag mismatch");
static_assert(lgc::ShadingRateHorizontal4Pixels ==
                  static_cast<lgc::ShadingRateFlags>(spv::FragmentShadingRateHorizontal4PixelsMask),
              "Shading rate flag mismatch");

// =====================================================================================================================
SpirvLowerGlobal::SpirvLowerGlobal() : m_lastVertexProcessingStage(ShaderStageInvalid) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerGlobal::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Global\n");

  SpirvLower::init(&module);

  changeRtFunctionSignature();

  // Special handling of explicit interpolation (InterpolateAt* instructions) in fragment shaders -- get those out of
  // the way.
  if (m_shaderStage == ShaderStageFragment)
    handleCallInst(false, true);

  // Preparations for output lowering
  m_unifiedReturn = nullptr;

  if (m_shaderStage == ShaderStageGeometry) {
    // Collect "emit" calls
    handleCallInst(true, false);
  } else if (m_shaderStage < ShaderStageGfxCount) {
    ensureUnifiedReturn();
  }

  // Preparations for XFB handling
  auto shaderStageMask = m_context->getShaderStageMask();
  m_lastVertexProcessingStage = ShaderStageInvalid;

  if (m_shaderStage < ShaderStageFragment) {
    if (shaderStageMask & ShaderStageGeometryBit)
      m_lastVertexProcessingStage = ShaderStageGeometry;
    else if (shaderStageMask & ShaderStageTessEvalBit)
      m_lastVertexProcessingStage = ShaderStageTessEval;
    else if (shaderStageMask & ShaderStageVertexBit)
      m_lastVertexProcessingStage = ShaderStageVertex;

    if (m_shaderStage == m_lastVertexProcessingStage)
      buildApiXfbMap();
  }

  // First pass over globals
  for (GlobalVariable &global : llvm::make_early_inc_range(m_module->globals())) {
    auto addrSpace = global.getType()->getAddressSpace();

    if (addrSpace == SPIRAS_Private || addrSpace == SPIRAS_Input || addrSpace == SPIRAS_Output) {
      // Remove constant indexing expression and remove any proxy variables that are needed. (But the proxies aren't
      // used yet for inputs/outputs.)
      convertUsersOfConstantsToInstructions(&global);

      if (addrSpace == SPIRAS_Private) {
        mapGlobalVariableToProxy(&global);
      } else {
        lowerInOut(&global);
      }
    } else if (addrSpace == SPIRAS_Local) {
      // Prefix all LDS variables to avoid downstream conflicts when linking shaders together
      if (global.hasName()) {
        global.setName(Twine("lds_") + getShaderStageName(m_shaderStage) + "_" + global.getName());
      }
    }
  }

  // Now that outputs have been lowered, replace the Emit(Stream)Vertex calls with builder code.
  for (auto emitCall : m_emitCalls) {
    unsigned emitStreamId =
        emitCall->arg_size() != 0 ? cast<ConstantInt>(emitCall->getArgOperand(0))->getZExtValue() : 0;
    m_builder->SetInsertPoint(emitCall);
    m_builder->CreateEmitVertex(emitStreamId);
    emitCall->eraseFromParent();
  }
  m_emitCalls.clear();

  // Do further lowering operations
  if (m_shaderStage == ShaderStageVertex)
    lowerEdgeFlag();

  lowerBufferBlock();
  lowerPushConsts();
  lowerTaskPayload();
  lowerUniformConstants();
  lowerAliasedVal();
  lowerShaderRecordBuffer();

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// add edgeflag input output
void SpirvLowerGlobal::lowerEdgeFlag() {
  const unsigned int edgeflagInputLocation = Vkgc::GlCompatibilityAttributeLocation::EdgeFlag;

  Llpc::PipelineContext *pipelineContext = m_context->getPipelineContext();
  const Vkgc::GraphicsPipelineBuildInfo *pipelineInfo =
      static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(pipelineContext->getPipelineBuildInfo());
  const VkPipelineVertexInputStateCreateInfo *vertexInfo = pipelineInfo->pVertexInput;

  if (!vertexInfo)
    return;

  for (unsigned i = 0; i < vertexInfo->vertexBindingDescriptionCount; i++) {
    auto binding = &vertexInfo->pVertexBindingDescriptions[i];

    if (binding->binding == edgeflagInputLocation) {
      Type *int32Ty = Type::getInt32Ty(*m_context);
      Value *zeroValue = m_builder->getInt32(0);

      lgc::InOutInfo inOutInfo;
      Value *edgeflagValue = m_builder->CreateReadGenericInput(int32Ty, edgeflagInputLocation, zeroValue, zeroValue, 0,
                                                               inOutInfo, nullptr);
      m_builder->CreateWriteBuiltInOutput(edgeflagValue, lgc::BuiltInEdgeFlag, inOutInfo, nullptr, nullptr);
      return;
    }
  }
}

// =====================================================================================================================
// Ensure that there is exactly one "ret" instruction. This is used for writing output variables for many shader types.
void SpirvLowerGlobal::ensureUnifiedReturn() {
  SmallVector<ReturnInst *> retInsts;

  for (BasicBlock &block : *m_entryPoint) {
    if (auto *retInst = dyn_cast<ReturnInst>(block.getTerminator()))
      retInsts.push_back(retInst);
  }

  if (retInsts.size() == 1) {
    m_unifiedReturn = retInsts[0];
    return;
  }

  // There are more than 2 returns; create a unified return block.
  //
  // Also create a "unified return block" if there are no returns at all. Such a shader will surely hang or otherwise
  // trigger UB if it is ever executed, but we still need to compile it correctly in case it never runs.
  BasicBlock *retBlock = BasicBlock::Create(*m_context, "", m_entryPoint);

  for (ReturnInst *retInst : retInsts) {
    m_builder->SetInsertPoint(retInst);
    m_builder->CreateBr(retBlock);
    retInst->eraseFromParent();
  }

  m_builder->SetInsertPoint(retBlock);
  m_unifiedReturn = m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Handle "call" instructions.
//
// @param checkEmitCall : Whether we should handle emit call or not
// @param checkInterpCall : Whether we should handle interpolate call or not
void SpirvLowerGlobal::handleCallInst(bool checkEmitCall, bool checkInterpCall) {
  assert(checkEmitCall != checkInterpCall);

  for (Function &function : m_module->functions()) {
    auto mangledName = function.getName();
    // We get all users before iterating because the iterator can be invalidated
    // by interpolateInputElement
    SmallVector<User *> users(function.users());
    for (User *user : make_early_inc_range(users)) {
      assert(isa<CallInst>(user) && "We should only have CallInst instructions here.");
      CallInst *callInst = cast<CallInst>(user);
      if (checkEmitCall) {
        if (mangledName.starts_with(gSPIRVName::EmitVertex) || mangledName.starts_with(gSPIRVName::EmitStreamVertex))
          m_emitCalls.insert(callInst);
      } else {
        assert(checkInterpCall);

        if (mangledName.starts_with(gSPIRVName::InterpolateAtCentroid) ||
            mangledName.starts_with(gSPIRVName::InterpolateAtSample) ||
            mangledName.starts_with(gSPIRVName::InterpolateAtOffset) ||
            mangledName.starts_with(gSPIRVName::InterpolateAtVertexAMD)) {
          m_builder->SetInsertPoint(callInst);

          // Translate interpolation functions to LLPC intrinsic calls
          auto loadSrc = callInst->getArgOperand(0);
          unsigned interpLoc = InterpLocUnknown;
          Value *auxInterpValue = nullptr;

          if (mangledName.starts_with(gSPIRVName::InterpolateAtCentroid))
            interpLoc = InterpLocCentroid;
          else if (mangledName.starts_with(gSPIRVName::InterpolateAtSample)) {
            interpLoc = InterpLocSample;
            auxInterpValue = callInst->getArgOperand(1); // Sample ID
          } else if (mangledName.starts_with(gSPIRVName::InterpolateAtOffset)) {
            interpLoc = InterpLocCenter;
            auxInterpValue = callInst->getArgOperand(1); // Offset from pixel center
            auto info = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
            if (info->getGlState().originUpperLeft) {
              auto yInvertOffset = m_builder->CreateExtractElement(auxInterpValue, 1);
              yInvertOffset = m_builder->CreateFNeg(yInvertOffset);
              auxInterpValue = m_builder->CreateInsertElement(auxInterpValue, yInvertOffset, 1);
            }
          } else {
            assert(mangledName.starts_with(gSPIRVName::InterpolateAtVertexAMD));
            interpLoc = InterpLocCustom;
            auxInterpValue = callInst->getArgOperand(1); // Vertex no.
          }

          GlobalVariable *gv = nullptr;
          SmallVector<Value *, 6> indexOperands;
          if (auto getElemPtr = dyn_cast<GEPOperator>(loadSrc)) {
            // The interpolant is an element of the input
            for (auto &index : getElemPtr->indices())
              indexOperands.push_back(m_builder->CreateZExtOrTrunc(index, m_builder->getInt32Ty()));
            gv = cast<GlobalVariable>(getElemPtr->getPointerOperand());
          } else {
            gv = cast<GlobalVariable>(loadSrc);
          }
          Value *result = interpolateInputElement(callInst->getType(), interpLoc, auxInterpValue, gv, indexOperands);
          callInst->replaceAllUsesWith(result);
          callInst->eraseFromParent();
        }
      }
    }
  }
}

// =====================================================================================================================
// Check if the given metadata value has a vertex index.
//
// @param metaVal : Metadata
static bool hasVertexIdx(const Constant &metaVal) {
  assert(metaVal.getNumOperands() == 4);
  ShaderInOutMetadata inOutMeta = {};
  inOutMeta.U64All[0] = cast<ConstantInt>(metaVal.getOperand(2))->getZExtValue();
  inOutMeta.U64All[1] = cast<ConstantInt>(metaVal.getOperand(3))->getZExtValue();

  if (inOutMeta.IsBuiltIn) {
    unsigned builtInId = inOutMeta.Value;
    return (builtInId == spv::BuiltInPerVertex || // GLSL style per-vertex data
            builtInId == spv::BuiltInPosition ||  // HLSL style per-vertex data
            builtInId == spv::BuiltInPointSize || builtInId == spv::BuiltInClipDistance ||
            builtInId == spv::BuiltInCullDistance);
  }

  return !static_cast<bool>(inOutMeta.PerPatch);
}

// =====================================================================================================================
// Check if the given metadata value has a primitive index.
//
// @param metaVal : Metadata
static bool hasPrimitiveIdx(const Constant &metaVal) {
  assert(metaVal.getNumOperands() == 4);
  ShaderInOutMetadata inOutMeta = {};
  inOutMeta.U64All[0] = cast<ConstantInt>(metaVal.getOperand(2))->getZExtValue();
  inOutMeta.U64All[1] = cast<ConstantInt>(metaVal.getOperand(3))->getZExtValue();

  if (inOutMeta.IsBuiltIn) {
    unsigned builtInId = inOutMeta.Value;
    return (builtInId == spv::BuiltInPerPrimitive || builtInId == spv::BuiltInPrimitivePointIndicesEXT ||
            builtInId == spv::BuiltInPrimitiveLineIndicesEXT || builtInId == spv::BuiltInPrimitiveTriangleIndicesEXT);
  }

  return static_cast<bool>(inOutMeta.PerPrimitive);
}

// =====================================================================================================================
// Maps the specified global variable to proxy variable.
//
// @param globalVar : Global variable to be mapped
void SpirvLowerGlobal::mapGlobalVariableToProxy(GlobalVariable *globalVar) {
  const auto &dataLayout = m_module->getDataLayout();
  Type *globalVarTy = globalVar->getValueType();

  Value *proxy = nullptr;

  // Collect used functions
  SmallSet<Function *, 4> funcs;
  for (User *user : globalVar->users()) {
    auto inst = cast<Instruction>(user);
    funcs.insert(inst->getFunction());
  }
  for (Function *func : funcs) {
    m_builder->SetInsertPointPastAllocas(func);
    proxy = m_builder->CreateAlloca(globalVarTy, dataLayout.getAllocaAddrSpace(), nullptr,
                                    Twine(LlpcName::GlobalProxyPrefix) + globalVar->getName());

    if (globalVar->hasInitializer()) {
      auto initializer = globalVar->getInitializer();
      m_builder->CreateStore(initializer, proxy);
    }
    globalVar->mutateType(proxy->getType());
    globalVar->replaceUsesWithIf(proxy, [func](Use &U) {
      Instruction *userInst = cast<Instruction>(U.getUser());
      return userInst->getFunction() == func;
    });
  }

  globalVar->dropAllReferences();
  globalVar->eraseFromParent();
}

// =====================================================================================================================
// Lowers an input or output global variable.
//
// @param globalVar : the global variable to be lowered
void SpirvLowerGlobal::lowerInOut(llvm::GlobalVariable *globalVar) {
  assert(globalVar->getAddressSpace() == SPIRAS_Input || globalVar->getAddressSpace() == SPIRAS_Output);
  const bool isInput = globalVar->getAddressSpace() == SPIRAS_Input;

  // Apply output initializer, if any
  if (!isInput && globalVar->hasInitializer()) {
    m_builder->SetInsertPointPastAllocas(m_entryPoint);
    auto initializer = globalVar->getInitializer();
    m_builder->CreateStore(initializer, globalVar);
  }

  const bool mapToProxy = isInput ? (m_shaderStage != ShaderStageTessControl && m_shaderStage != ShaderStageTessEval)
                                  : (m_shaderStage != ShaderStageTessControl && m_shaderStage != ShaderStageTask &&
                                     m_shaderStage != ShaderStageMesh);

  if (mapToProxy) {
    const auto &dataLayout = m_module->getDataLayout();
    Type *ty = globalVar->getValueType();
    if (ty->isPointerTy())
      ty = m_builder->getInt64Ty();
    MDNode *metaNode = globalVar->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);
    auto meta = mdconst::extract<Constant>(metaNode->getOperand(0));

    m_builder->SetInsertPointPastAllocas(m_entryPoint);
    Value *proxy = m_builder->CreateAlloca(ty, dataLayout.getAllocaAddrSpace(), nullptr,
                                           Twine(LlpcName::InputProxyPrefix) + globalVar->getName());

    if (isInput) {
      // Import input to proxy variable
      auto inputValue = addCallInstForInOutImport(ty, SPIRAS_Input, meta, nullptr, 0, nullptr, nullptr,
                                                  InterpLocUnknown, nullptr, false);

      m_builder->CreateStore(inputValue, proxy);

      handleVolatileInput(globalVar, proxy);
    } else {
      // Export the output at shader end or vertex emit
      if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
          m_shaderStage == ShaderStageFragment) {
        m_builder->SetInsertPoint(m_unifiedReturn);
        Value *outputValue = m_builder->CreateLoad(ty, proxy);
        addCallInstForOutputExport(outputValue, meta, nullptr, 0, 0, 0, nullptr, nullptr, InvalidValue);
      } else {
        assert(m_shaderStage == ShaderStageGeometry);

        for (auto emitCall : m_emitCalls) {
          unsigned emitStreamId = 0;

          m_builder->SetInsertPoint(emitCall);

          auto mangledName = emitCall->getCalledFunction()->getName();
          if (mangledName.starts_with(gSPIRVName::EmitStreamVertex))
            emitStreamId = cast<ConstantInt>(emitCall->getOperand(0))->getZExtValue();
          else
            assert(mangledName.starts_with(gSPIRVName::EmitVertex));

          Value *outputValue = m_builder->CreateLoad(ty, proxy);
          addCallInstForOutputExport(outputValue, meta, nullptr, 0, 0, 0, nullptr, nullptr, emitStreamId);
        }
      }
    }

    SmallVector<Instruction *> toErase;
    CompilerUtils::replaceAllPointerUses(m_builder, globalVar, proxy, toErase);
    for (auto inst : toErase)
      inst->eraseFromParent();
  } else {
    // In-place lowering.
    SmallVector<Value *> indexStack;
    lowerInOutUsersInPlace(globalVar, globalVar, indexStack);
  }

  assert(globalVar->use_empty());
  globalVar->eraseFromParent();
}

// =====================================================================================================================
// Recursively lower all users of `current`, which can be traced back to `globalVar` via the given GEP indices,
// to in-place import/export ops.
//
// This makes the assumption that GEPs have not been type-punned (though 0 indices may have been dropped).
void SpirvLowerGlobal::lowerInOutUsersInPlace(llvm::GlobalVariable *globalVar, llvm::Value *current,
                                              SmallVectorImpl<llvm::Value *> &indexStack) {
  for (User *user : llvm::make_early_inc_range(current->users())) {
    Instruction *inst = cast<Instruction>(user);

    if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
      // TODO: As LLVM is moving away from GEPs towards ptradds, we need a better solution, probably by adding our
      //       own "structured GEP" operation.
      assert(cast<ConstantInt>(gep->idx_begin()[0])->isNullValue());

      for (unsigned i = 1, e = gep->getNumIndices(); i < e; ++i)
        indexStack.push_back(m_builder->CreateZExtOrTrunc(gep->idx_begin()[i], m_builder->getInt32Ty()));

      lowerInOutUsersInPlace(globalVar, gep, indexStack);

      indexStack.clear();
    } else if (isa<LoadInst>(inst) || isa<StoreInst>(inst)) {
      auto *loadInst = dyn_cast<LoadInst>(inst);
      auto *storeInst = dyn_cast<StoreInst>(inst);

      m_builder->SetInsertPoint(inst);

      Value *vertexOrPrimitiveIdx = nullptr;
      auto inOutTy = globalVar->getValueType();
      auto accessTy = loadInst ? loadInst->getType() : storeInst->getValueOperand()->getType();
      auto addrSpace = globalVar->getAddressSpace();

      MDNode *metaNode = globalVar->getMetadata(gSPIRVMD::InOut);
      assert(metaNode);
      auto inOutMetaVal = mdconst::extract<Constant>(metaNode->getOperand(0));

      auto indexOperands = ArrayRef(indexStack);

      // If the input/output is arrayed, the outermost index might be used for vertex indexing
      if (inOutTy->isArrayTy() && (hasVertexIdx(*inOutMetaVal) || hasPrimitiveIdx(*inOutMetaVal))) {
        if (!indexOperands.empty()) {
          vertexOrPrimitiveIdx = indexOperands.front();
          indexOperands = indexOperands.drop_front();
        } else if (inOutTy != accessTy) {
          vertexOrPrimitiveIdx = m_builder->getInt32(0);
        }
        inOutTy = inOutTy->getArrayElementType();
        inOutMetaVal = cast<Constant>(inOutMetaVal->getOperand(1));
      }

      if (loadInst) {
        Value *loadValue = loadInOutMember(inOutTy, accessTy, addrSpace, indexOperands, 0, inOutMetaVal, nullptr,
                                           vertexOrPrimitiveIdx, InterpLocUnknown, nullptr, false);
        loadInst->replaceAllUsesWith(loadValue);
      } else {
        Value *storeValue = storeInst->getOperand(0);
        storeOutputMember(inOutTy, accessTy, storeValue, indexOperands, 0, inOutMetaVal, nullptr, vertexOrPrimitiveIdx);
      }
    } else {
      llvm_unreachable("unhandled user of input/output variable");
    }

    inst->eraseFromParent();
  }
}

// =====================================================================================================================
// @param builtIn : BuiltIn value
// @param elemIdx : Element Index of struct
Value *SpirvLowerGlobal::createRaytracingBuiltIn(BuiltIn builtIn) {
  switch (builtIn) {
  case BuiltInLaunchIdKHR:
    return m_builder->create<DispatchRaysIndexOp>();
  case BuiltInLaunchSizeKHR:
    return m_builder->create<DispatchRaysDimensionsOp>();
  case BuiltInWorldRayOriginKHR:
    return m_builder->create<WorldRayOriginOp>();
  case BuiltInWorldRayDirectionKHR:
    return m_builder->create<WorldRayDirectionOp>();
  case BuiltInObjectRayOriginKHR:
    return m_builder->create<ObjectRayOriginOp>();
  case BuiltInObjectRayDirectionKHR:
    return m_builder->create<ObjectRayDirectionOp>();
  case BuiltInRayTminKHR:
    return m_builder->create<RayTminOp>();
  case BuiltInHitTNV:
  case BuiltInRayTmaxKHR:
    return m_builder->create<RayTcurrentOp>();
  case BuiltInInstanceCustomIndexKHR:
    // Note: GPURT(HLSL) has just the opposite naming of index/ID compares to SPIR-V. For dialect calls, we use
    // GPURT-style.
    return m_builder->create<InstanceIdOp>();
  case BuiltInObjectToWorldKHR:
    return m_builder->create<ObjectToWorldOp>();
  case BuiltInWorldToObjectKHR:
    return m_builder->create<WorldToObjectOp>();
  case BuiltInHitKindKHR:
    return m_builder->create<HitKindOp>();
  case BuiltInHitTriangleVertexPositionsKHR:
    return m_builder->create<TriangleVertexPositionsOp>();
  case BuiltInIncomingRayFlagsKHR:
    return m_builder->create<RayFlagsOp>();
  case BuiltInRayGeometryIndexKHR:
    return m_builder->create<GeometryIndexOp>();
  case BuiltInInstanceId:
    // Note: GPURT(HLSL) has just the opposite naming of index/ID compares to SPIR-V. For dialect calls, we use
    // GPURT-style.
    return m_builder->create<InstanceIndexOp>();
  case BuiltInPrimitiveId:
    return m_builder->create<PrimitiveIndexOp>();
  case BuiltInCullMaskKHR:
    return m_builder->create<InstanceInclusionMaskOp>();
  default:
    llvm_unreachable("Should never be called");
    return nullptr;
  }
}

// =====================================================================================================================
//
// @param builtIn : BuiltIn value
// @param stage : Shader stage
inline bool isRayTracingBuiltIn(unsigned builtIn, ShaderStage stage) {
  bool rtbuiltIn =
      (builtIn >= BuiltInLaunchIdKHR && builtIn <= BuiltInRayGeometryIndexKHR) || (builtIn == BuiltInCullMaskKHR);
  bool rtStage = stage == ShaderStageRayTracingIntersect || stage == ShaderStageRayTracingAnyHit ||
                 stage == ShaderStageRayTracingClosestHit;
  bool nonRtBuiltIn = builtIn == BuiltInInstanceId || builtIn == BuiltInPrimitiveId;
  return rtbuiltIn || (rtStage && nonRtBuiltIn);
}

// =====================================================================================================================
// Inserts LLVM call instruction to import input/output.
//
// @param inOutTy : Type of value imported from input/output
// @param addrSpace : Address space
// @param inOutMetaVal : Metadata of this input/output
// @param locOffset : Relative location offset, passed from aggregate type
// @param maxLocOffset : Max+1 location offset if variable index has been encountered. For an array built-in with a
// variable index, this is the array size.
// @param elemIdx : Element index used for element indexing, valid for tessellation shader (usually, it is vector
// component index, for built-in input/output, it could be element index of scalar array)
// @param vertexIdx : Input array outermost index used for vertex indexing, valid for tessellation shader and geometry
// shader
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader) - Value is sample ID for
// "InterpLocSample" - Value is offset from the center of the pixel for "InterpLocCenter" - Value is vertex no. (0 ~ 2)
// for "InterpLocCustom"
// @param isPerVertexDimension : Whether this is a per vertex variable
Value *SpirvLowerGlobal::addCallInstForInOutImport(Type *inOutTy, unsigned addrSpace, Constant *inOutMetaVal,
                                                   Value *locOffset, unsigned maxLocOffset, Value *elemIdx,
                                                   Value *vertexIdx, unsigned interpLoc, Value *auxInterpValue,
                                                   bool isPerVertexDimension) {
  assert(addrSpace == SPIRAS_Input || (addrSpace == SPIRAS_Output && m_shaderStage == ShaderStageTessControl));

  Value *inOutValue = PoisonValue::get(inOutTy);

  ShaderInOutMetadata inOutMeta = {};

  if (inOutTy->isArrayTy()) {
    // Array type
    assert(!elemIdx);

    assert(inOutMetaVal->getNumOperands() == 4);
    unsigned stride = cast<ConstantInt>(inOutMetaVal->getOperand(0))->getZExtValue();
    inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaVal->getOperand(2))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaVal->getOperand(3))->getZExtValue();

    if (inOutMeta.IsBuiltIn) {
      assert(!locOffset);

      unsigned builtInId = inOutMeta.Value;

      if (isRayTracingBuiltIn(builtInId, m_shaderStage))
        inOutValue = createRaytracingBuiltIn(static_cast<BuiltIn>(inOutMeta.Value));
      else {
        if (!vertexIdx && m_shaderStage == ShaderStageGeometry &&
            (builtInId == spv::BuiltInPerVertex || // GLSL style per-vertex data
             builtInId == spv::BuiltInPosition ||  // HLSL style per-vertex data
             builtInId == spv::BuiltInPointSize || builtInId == spv::BuiltInClipDistance ||
             builtInId == spv::BuiltInCullDistance)) {
          // NOTE: We are handling vertex indexing of built-in inputs of geometry shader. For tessellation
          // shader, vertex indexing is handled by "load"/"store" instruction lowering.
          assert(!vertexIdx); // For per-vertex data, make a serial of per-vertex import calls.

          assert(m_shaderStage == ShaderStageGeometry || m_shaderStage == ShaderStageTessControl ||
                 m_shaderStage == ShaderStageTessEval);

          auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));
          auto elemTy = inOutTy->getArrayElementType();

          const uint64_t elemCount = inOutTy->getArrayNumElements();
          for (unsigned idx = 0; idx < elemCount; ++idx) {
            // Handle array elements recursively
            vertexIdx = m_builder->getInt32(idx);
            auto elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, nullptr, maxLocOffset, nullptr,
                                                  vertexIdx, interpLoc, auxInterpValue, false);
            inOutValue = m_builder->CreateInsertValue(inOutValue, elem, {idx});
          }
        } else {
          // Array built-in without vertex indexing (ClipDistance/CullDistance).
          lgc::InOutInfo inOutInfo;
          inOutInfo.setArraySize(inOutTy->getArrayNumElements());
          // For Barycentric interplotation
          inOutInfo.setInterpLoc(interpLoc);
          assert(!inOutMeta.PerPrimitive); // No per-primitive arrayed built-in
          if (addrSpace == SPIRAS_Input) {
            inOutValue =
                m_builder->CreateReadBuiltInInput(static_cast<lgc::BuiltInKind>(inOutMeta.Value), inOutInfo, vertexIdx);
          } else {
            inOutValue = m_builder->CreateReadBuiltInOutput(static_cast<lgc::BuiltInKind>(inOutMeta.Value), inOutInfo,
                                                            vertexIdx, nullptr);
          }
        }
      }
    } else {
      auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));
      auto elemTy = inOutTy->getArrayElementType();

      const uint64_t elemCount = inOutTy->getArrayNumElements();

      if (!vertexIdx && m_shaderStage == ShaderStageGeometry) {
        // NOTE: We are handling vertex indexing of generic inputs of geometry shader. For tessellation shader,
        // vertex indexing is handled by "load"/"store" instruction lowering.
        for (unsigned idx = 0; idx < elemCount; ++idx) {
          vertexIdx = m_builder->getInt32(idx);
          auto elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, locOffset, maxLocOffset, nullptr,
                                                vertexIdx, InterpLocUnknown, nullptr, false);
          inOutValue = m_builder->CreateInsertValue(inOutValue, elem, {idx});
        }
      } else {
        // NOTE: If the relative location offset is not specified, initialize it to 0.
        if (!locOffset)
          locOffset = m_builder->getInt32(0);

        for (unsigned idx = 0; idx < elemCount; ++idx) {
          Value *elem = nullptr;
          if (inOutMeta.PerVertexDimension) {
            assert(inOutMeta.InterpMode == InterpModeCustom);
            elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, nullptr, 0, nullptr, 0, inOutMeta.InterpLoc,
                                             m_builder->getInt32(idx), true);
          } else {
            // Handle array elements recursively
            // elemLocOffset = locOffset + stride * idx
            Value *elemLocOffset = nullptr;
            if (isa<ConstantInt>(locOffset))
              elemLocOffset = m_builder->getInt32(cast<ConstantInt>(locOffset)->getZExtValue() + stride * idx);
            else
              elemLocOffset = m_builder->CreateAdd(locOffset, m_builder->getInt32(stride * idx));

            elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, elemLocOffset, maxLocOffset, elemIdx,
                                             vertexIdx, interpLoc, auxInterpValue, isPerVertexDimension);
          }
          inOutValue = m_builder->CreateInsertValue(inOutValue, elem, {idx});
        }
      }
    }
  } else if (inOutTy->isStructTy()) {
    // Structure type
    assert(!elemIdx);

    const uint64_t memberCount = inOutTy->getStructNumElements();
    for (unsigned memberIdx = 0; memberIdx < memberCount; ++memberIdx) {
      // Handle structure member recursively
      auto memberTy = inOutTy->getStructElementType(memberIdx);
      auto memberMeta = cast<Constant>(inOutMetaVal->getOperand(memberIdx));
      auto member = addCallInstForInOutImport(memberTy, addrSpace, memberMeta, locOffset, maxLocOffset, nullptr,
                                              vertexIdx, interpLoc, auxInterpValue, isPerVertexDimension);
      inOutValue = m_builder->CreateInsertValue(inOutValue, member, {memberIdx});
    }
  } else {
    Constant *inOutMetaValConst = cast<Constant>(inOutMetaVal);
    inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaValConst->getOperand(0))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaValConst->getOperand(1))->getZExtValue();

    assert(inOutMeta.IsLoc || inOutMeta.IsBuiltIn);

    if (inOutMeta.IsBuiltIn) {
      if (isRayTracingBuiltIn(inOutMeta.Value, m_shaderStage))
        inOutValue = createRaytracingBuiltIn(static_cast<BuiltIn>(inOutMeta.Value));
      else {
        auto builtIn = static_cast<lgc::BuiltInKind>(inOutMeta.Value);
        elemIdx = elemIdx == m_builder->getInt32(InvalidValue) ? nullptr : elemIdx;
        vertexIdx = vertexIdx == m_builder->getInt32(InvalidValue) ? nullptr : vertexIdx;

        lgc::InOutInfo inOutInfo;
        inOutInfo.setArraySize(maxLocOffset);
        inOutInfo.setInterpLoc(interpLoc);

        if (builtIn == lgc::BuiltInBaryCoord || builtIn == lgc::BuiltInBaryCoordNoPerspKHR) {
          inOutInfo.setInterpMode(InterpModeCustom);
          if (inOutInfo.getInterpLoc() == InterpLocUnknown)
            inOutInfo.setInterpLoc(inOutMeta.InterpLoc);
          return m_builder->CreateReadBaryCoord(builtIn, inOutInfo, auxInterpValue);
        }

        inOutInfo.setPerPrimitive(inOutMeta.PerPrimitive);
        if (addrSpace == SPIRAS_Input) {
          // In the case where the command has no baseVertex parameter, force the value of gl_BaseVertex to zero
          if (builtIn == lgc::BuiltInBaseVertex &&
              m_context->getPipelineContext()->getPipelineOptions()->getGlState().disableBaseVertex)
            inOutValue = m_builder->getInt32(0);
          else
            inOutValue = m_builder->CreateReadBuiltInInput(builtIn, inOutInfo, vertexIdx, elemIdx);
        } else {
          inOutValue = m_builder->CreateReadBuiltInOutput(builtIn, inOutInfo, vertexIdx, elemIdx);
        }
        if ((builtIn == lgc::BuiltInSubgroupEqMask || builtIn == lgc::BuiltInSubgroupGeMask ||
             builtIn == lgc::BuiltInSubgroupGtMask || builtIn == lgc::BuiltInSubgroupLeMask ||
             builtIn == lgc::BuiltInSubgroupLtMask) &&
            inOutTy->isIntegerTy(64)) {
          // NOTE: Glslang has a bug. For gl_SubGroupXXXMaskARB, they are implemented as "uint64_t" while
          // for gl_subgroupXXXMask they are "uvec4". And the SPIR-V enumerants "BuiltInSubgroupXXXMaskKHR"
          // and "BuiltInSubgroupXXXMask" share the same numeric values.
          inOutValue = m_builder->CreateBitCast(inOutValue, FixedVectorType::get(inOutTy, 2));
          inOutValue = m_builder->CreateExtractElement(inOutValue, uint64_t(0));
        } else if (builtIn == lgc::BuiltInFragCoord) {
          auto buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
          if (buildInfo->getGlState().originUpperLeft !=
              static_cast<const ShaderModuleData *>(buildInfo->fs.pModuleData)->usage.originUpperLeft) {
            unsigned offset = 0;
            auto winSize = getUniformConstantEntryByLocation(m_context, m_shaderStage,
                                                             Vkgc::GlCompatibilityUniformLocation::FrameBufferSize);
            if (winSize) {
              offset = winSize->offset;
              assert(m_shaderStage != Vkgc::ShaderStageTask && m_shaderStage != Vkgc::ShaderStageMesh);
              unsigned constBufferBinding =
                  Vkgc::ConstantBuffer0Binding + static_cast<GraphicsContext *>(m_context->getPipelineContext())
                                                     ->getPipelineShaderInfo(m_shaderStage)
                                                     ->options.constantBufferBindingOffset;

              Value *bufferDesc =
                  m_builder->create<lgc::LoadBufferDescOp>(Vkgc::InternalDescriptorSetId, constBufferBinding,
                                                           m_builder->getInt32(0), lgc::Builder::BufferFlagNonConst);
              // Layout is {width, height}, so the offset of height is added sizeof(float).
              Value *winHeightPtr =
                  m_builder->CreateConstInBoundsGEP1_32(m_builder->getInt8Ty(), bufferDesc, offset + sizeof(float));
              auto winHeight = m_builder->CreateLoad(m_builder->getFloatTy(), winHeightPtr);
              auto fragCoordY = m_builder->CreateExtractElement(inOutValue, 1);
              fragCoordY = m_builder->CreateFSub(winHeight, fragCoordY);
              inOutValue = m_builder->CreateInsertElement(inOutValue, fragCoordY, 1);
            }
          }
        }
        if (inOutValue->getType()->isIntegerTy(1)) {
          // Convert i1 to i32.
          inOutValue = m_builder->CreateZExt(inOutValue, m_builder->getInt32Ty());
        }
      }
    } else {
      lgc::InOutInfo inOutInfo;
      inOutInfo.setComponent(inOutMeta.Component);
      // Specify NumComponents if components are dynamically indexed
      if (elemIdx && !isa<ConstantInt>(elemIdx))
        inOutInfo.setNumComponents(inOutMeta.NumComponents);

      unsigned idx = inOutMeta.Component;
      assert(inOutMeta.Component <= 3);
      if (inOutTy->getScalarSizeInBits() == 64) {
        assert(inOutMeta.Component % 2 == 0); // Must be even for 64-bit type
        idx = inOutMeta.Component / 2;
      }
      elemIdx = !elemIdx ? m_builder->getInt32(idx) : m_builder->CreateAdd(elemIdx, m_builder->getInt32(idx));

      if (!locOffset)
        locOffset = m_builder->getInt32(0);

      if (inOutTy->isPointerTy())
        inOutTy = m_builder->getInt64Ty();

      if (addrSpace == SPIRAS_Input) {
        if (m_shaderStage == ShaderStageFragment) {
          if (interpLoc != InterpLocUnknown) {
            // Use auxiliary value of interpolation (calculated I/J or vertex no.) for
            // interpolant inputs of fragment shader.
            vertexIdx = auxInterpValue;
            inOutInfo.setHasInterpAux();
          } else
            interpLoc = inOutMeta.InterpLoc;
          inOutInfo.setInterpLoc(interpLoc);
          inOutInfo.setInterpMode(inOutMeta.InterpMode);
          inOutInfo.setPerPrimitive(inOutMeta.PerPrimitive);
        }
        if (isPerVertexDimension)
          inOutValue = m_builder->CreateReadPerVertexInput(inOutTy, inOutMeta.Value, locOffset, elemIdx, maxLocOffset,
                                                           inOutInfo, vertexIdx);
        else
          inOutValue = m_builder->CreateReadGenericInput(inOutTy, inOutMeta.Value, locOffset, elemIdx, maxLocOffset,
                                                         inOutInfo, vertexIdx);
      } else {
        inOutValue = m_builder->CreateReadGenericOutput(inOutTy, inOutMeta.Value, locOffset, elemIdx, maxLocOffset,
                                                        inOutInfo, vertexIdx);
      }
    }
  }

  return inOutValue;
}

// =====================================================================================================================
// Inserts LLVM call instruction to export output.
//
// @param outputValue : Value exported to output
// @param outputMetaVal : Metadata of this output
// @param locOffset : Relative location offset, passed from aggregate type
// @param maxLocOffset : Max+1 location offset if variable index has been encountered. For an array built-in with a
// variable index, this is the array size.
// @param xfbOffsetAdjust : Adjustment of transform feedback offset (for array type)
// @param xfbBufferAdjust : Adjustment of transform feedback buffer ID (for array type, default is 0)
// @param elemIdx : Element index used for element indexing, valid for tessellation control shader (usually, it is
// vector component index, for built-in input/output, it could be element index of scalar array)
// @param vertexOrPrimitiveIdx : Output array outermost index used for vertex indexing
// @param emitStreamId : ID of emitted vertex stream, valid for geometry shader (0xFFFFFFFF for others)
void SpirvLowerGlobal::addCallInstForOutputExport(Value *outputValue, Constant *outputMetaVal, Value *locOffset,
                                                  unsigned maxLocOffset, unsigned xfbOffsetAdjust,
                                                  unsigned xfbBufferAdjust, Value *elemIdx, Value *vertexOrPrimitiveIdx,
                                                  unsigned emitStreamId) {
  Type *outputTy = outputValue->getType();

  ShaderInOutMetadata outputMeta = {};

  if (outputTy->isArrayTy()) {
    // Array type
    assert(!elemIdx);

    assert(outputMetaVal->getNumOperands() == 4);
    unsigned stride = cast<ConstantInt>(outputMetaVal->getOperand(0))->getZExtValue();

    outputMeta.U64All[0] = cast<ConstantInt>(outputMetaVal->getOperand(2))->getZExtValue();
    outputMeta.U64All[1] = cast<ConstantInt>(outputMetaVal->getOperand(3))->getZExtValue();

    if (m_shaderStage == ShaderStageGeometry && emitStreamId != outputMeta.StreamId) {
      // NOTE: For geometry shader, if the output is not bound to this vertex stream, we skip processing.
      return;
    }

    if (outputMeta.IsBuiltIn) {
      // NOTE: For geometry shader, we add stream ID for outputs.
      assert(m_shaderStage != ShaderStageGeometry || emitStreamId == outputMeta.StreamId);

      auto builtInId = static_cast<lgc::BuiltInKind>(outputMeta.Value);
      lgc::InOutInfo outputInfo;
      if (emitStreamId != InvalidValue)
        outputInfo.setStreamId(emitStreamId);
      outputInfo.setArraySize(outputTy->getArrayNumElements());
      assert(!outputMeta.PerPrimitive); // No per-primitive arrayed built-in
      m_builder->CreateWriteBuiltInOutput(outputValue, builtInId, outputInfo, vertexOrPrimitiveIdx, nullptr);

      if (m_lastVertexProcessingStage == m_shaderStage) {
        auto elemTy = outputTy->getArrayElementType();
        assert(elemTy->isFloatingPointTy() || elemTy->isIntegerTy()); // Must be scalar
        const uint64_t elemCount = outputTy->getArrayNumElements();
        const uint64_t byteSize = elemTy->getScalarSizeInBits() / 8;

        for (unsigned idx = 0; idx < elemCount; ++idx) {
          auto elem = m_builder->CreateExtractValue(outputValue, {idx}, "");
          unsigned elemXfbOffset = byteSize * idx;
          addCallInstForXfbOutput(outputMeta, elem, 0, elemXfbOffset, 0, outputInfo);
        }
      }
    } else {
      // NOTE: If the relative location offset is not specified, initialize it to 0.
      if (!locOffset)
        locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);

      auto elemMeta = cast<Constant>(outputMetaVal->getOperand(1));

      const uint64_t elemCount = outputTy->getArrayNumElements();
      for (unsigned idx = 0; idx < elemCount; ++idx) {
        // Handle array elements recursively
        Value *elem = m_builder->CreateExtractValue(outputValue, {idx}, "");

        Value *elemLocOffset = nullptr;
        ConstantInt *locOffsetConst = dyn_cast<ConstantInt>(locOffset);

        // elemLocOffset = locOffset + stride * idx
        if (locOffsetConst) {
          unsigned locOffset = locOffsetConst->getZExtValue();
          elemLocOffset = m_builder->getInt32(locOffset + stride * idx);
        } else {
          elemLocOffset = m_builder->CreateAdd(locOffset, m_builder->getInt32(stride * idx));
        }

        // NOTE: GLSL spec says: an array of size N of blocks is captured by N consecutive buffers,
        // with all members of block array-element E captured by buffer B, where B equals the declared or
        // inherited xfb_buffer plus E.
        bool blockArray = outputMeta.IsBlockArray;
        addCallInstForOutputExport(elem, elemMeta, elemLocOffset, maxLocOffset,
                                   xfbOffsetAdjust + (blockArray ? 0 : outputMeta.XfbArrayStride * idx),
                                   xfbBufferAdjust + (blockArray ? outputMeta.XfbArrayStride * idx : 0), nullptr,
                                   vertexOrPrimitiveIdx, emitStreamId);
      }
    }
  } else if (outputTy->isStructTy()) {
    // Structure type
    assert(!elemIdx);

    const uint64_t memberCount = outputTy->getStructNumElements();
    for (unsigned memberIdx = 0; memberIdx < memberCount; ++memberIdx) {
      // Handle structure member recursively
      auto memberMeta = cast<Constant>(outputMetaVal->getOperand(memberIdx));
      Value *member = m_builder->CreateExtractValue(outputValue, {memberIdx});
      addCallInstForOutputExport(member, memberMeta, locOffset, maxLocOffset, xfbOffsetAdjust, xfbBufferAdjust, nullptr,
                                 vertexOrPrimitiveIdx, emitStreamId);
    }
  } else {
    // Normal scalar or vector type
    Constant *inOutMetaConst = cast<Constant>(outputMetaVal);
    outputMeta.U64All[0] = cast<ConstantInt>(inOutMetaConst->getOperand(0))->getZExtValue();
    outputMeta.U64All[1] = cast<ConstantInt>(inOutMetaConst->getOperand(1))->getZExtValue();

    if (m_shaderStage == ShaderStageGeometry && emitStreamId != outputMeta.StreamId) {
      // NOTE: For geometry shader, if the output is not bound to this vertex stream, we skip processing.
      return;
    }

    assert(outputMeta.IsLoc || outputMeta.IsBuiltIn);

    lgc::InOutInfo outputInfo;
    if (emitStreamId != InvalidValue)
      outputInfo.setStreamId(emitStreamId);
    outputInfo.setIsSigned(outputMeta.Signedness);
    outputInfo.setPerPrimitive(outputMeta.PerPrimitive);
    outputInfo.setComponent(outputMeta.Component);

    if (outputMeta.IsBuiltIn) {
      auto builtInId = static_cast<lgc::BuiltInKind>(outputMeta.Value);
      outputInfo.setArraySize(maxLocOffset);
      if (m_lastVertexProcessingStage == m_shaderStage)
        addCallInstForXfbOutput(outputMeta, outputValue, 0, 0, 0, outputInfo);

      if (builtInId == lgc::BuiltInCullPrimitive && outputTy->isIntegerTy(32)) {
        // NOTE: In SPIR-V translation, the boolean type (i1) in output block is converted to i32. Here, we convert it
        // back to i1 for further processing in LGC.
        outputValue = m_builder->CreateTrunc(outputValue, m_builder->getInt1Ty());
      }
      m_builder->CreateWriteBuiltInOutput(outputValue, builtInId, outputInfo, vertexOrPrimitiveIdx, elemIdx);
      return;
    } else {
      // Specify NumComponents if components are dynamically indexed
      if (elemIdx && !isa<ConstantInt>(elemIdx))
        outputInfo.setNumComponents(outputMeta.NumComponents);
    }

    unsigned location = outputMeta.Value + outputMeta.Index;
    assert((outputMeta.Index == 1 && outputMeta.Value == 0) || outputMeta.Index == 0);
    assert(outputTy->isSingleValueType());

    unsigned idx = outputMeta.Component;
    assert(outputMeta.Component <= 3);
    if (outputTy->getScalarSizeInBits() == 64) {
      assert(outputMeta.Component % 2 == 0); // Must be even for 64-bit type
      idx = outputMeta.Component / 2;
    }
    elemIdx = !elemIdx ? m_builder->getInt32(idx) : m_builder->CreateAdd(elemIdx, m_builder->getInt32(idx));
    locOffset = !locOffset ? m_builder->getInt32(0) : locOffset;
    if (m_lastVertexProcessingStage == m_shaderStage) {
      assert(isa<ConstantInt>(locOffset));
      addCallInstForXfbOutput(outputMeta, outputValue, xfbBufferAdjust, xfbOffsetAdjust,
                              cast<ConstantInt>(locOffset)->getZExtValue(), outputInfo);
    }

    m_builder->CreateWriteGenericOutput(outputValue, location, locOffset, elemIdx, maxLocOffset, outputInfo,
                                        vertexOrPrimitiveIdx);
  }
}

// =====================================================================================================================
// Inserts instructions to load possibly dynamic indexed members from input/ouput location.
//
// Sometimes, we are accessing data with dynamic index, but the hardware currently may not be able to do this under
// situations like interpolation in fragment shader, what we do here is check whether the index is dynamic, if that
// is true, we pre-load all the possibly accessed members, if the index is a static constant, we just pre-load the
// specific one. Then later after this function been called, you could load the really needed data from the pre-loaded
// data.
//
// @param inOutTy : Type of this input/output member
// @param addrSpace : Address space
// @param indexOperands : Index operands to process
// @param inOutMetaVal : Metadata of this input/output member
// @param locOffset : Relative location offset of this input/output member
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader):
//                       - Sample ID for "InterpLocSample"
//                       - Offset from the center of the pixel for "InterpLocCenter"
//                       - Vertex no. (0 ~ 2) for "InterpLocCustom"
Value *SpirvLowerGlobal::loadDynamicIndexedMembers(Type *inOutTy, unsigned addrSpace, ArrayRef<Value *> indexOperands,
                                                   Constant *inOutMetaVal, Value *locOffset, unsigned interpLoc,
                                                   Value *auxInterpValue, bool isPerVertexDimension) {
  // Currently this is only used in fragment shader on loading interpolate sources.
  assert(m_shaderStage == ShaderStageFragment);

  ShaderInOutMetadata inOutMeta = {};
  Value *inOutValue = PoisonValue::get(inOutTy);
  if (inOutTy->isArrayTy()) {
    assert(inOutMetaVal->getNumOperands() == 4);
    inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaVal->getOperand(2))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaVal->getOperand(3))->getZExtValue();
    if (inOutMeta.PerVertexDimension) {
      assert(inOutMeta.InterpMode == InterpModeCustom);
      isPerVertexDimension = true;
    }

    auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));
    unsigned stride = cast<ConstantInt>(inOutMetaVal->getOperand(0))->getZExtValue();
    auto elemTy = inOutTy->getArrayElementType();
    if (!locOffset)
      locOffset = m_builder->getInt32(0);

    if (!isa<Constant>(indexOperands.front())) {
      // The index is not constant, we don't know which value will be accessed, just load all members.
      const uint64_t elemCount = inOutTy->getArrayNumElements();
      for (unsigned idx = 0; idx < elemCount; ++idx) {
        Value *elemLocOffset = nullptr;
        if (inOutMeta.PerVertexDimension) {
          auxInterpValue = m_builder->getInt32(idx);
          elemLocOffset = m_builder->getInt32(0);
        } else {
          if (isa<ConstantInt>(locOffset))
            elemLocOffset = m_builder->getInt32(cast<ConstantInt>(locOffset)->getZExtValue() + stride * idx);
          else
            elemLocOffset = m_builder->CreateAdd(locOffset, m_builder->getInt32(stride * idx));
        }

        auto elem = loadDynamicIndexedMembers(elemTy, addrSpace, indexOperands.drop_front(), elemMeta, elemLocOffset,
                                              interpLoc, auxInterpValue, isPerVertexDimension);
        inOutValue = m_builder->CreateInsertValue(inOutValue, elem, {idx});
      }
      return inOutValue;
    }

    // For constant index, we only need to load the specified value
    unsigned elemIdx = cast<ConstantInt>(indexOperands.front())->getZExtValue();
    Value *elemLocOffset = nullptr;
    if (isa<ConstantInt>(locOffset))
      elemLocOffset = m_builder->getInt32(cast<ConstantInt>(locOffset)->getZExtValue() + stride * elemIdx);
    else
      elemLocOffset = m_builder->CreateAdd(locOffset, m_builder->getInt32(stride * elemIdx));

    Value *elem = loadDynamicIndexedMembers(elemTy, addrSpace, indexOperands.drop_front(), elemMeta, elemLocOffset,
                                            interpLoc, auxInterpValue, isPerVertexDimension);
    return m_builder->CreateInsertValue(inOutValue, elem, {elemIdx});
  }

  if (inOutTy->isStructTy()) {
    // Struct type always has a constant index
    unsigned memberIdx = cast<ConstantInt>(indexOperands.front())->getZExtValue();

    auto memberTy = inOutTy->getStructElementType(memberIdx);
    auto memberMeta = cast<Constant>(inOutMetaVal->getOperand(memberIdx));

    auto loadValue = loadDynamicIndexedMembers(memberTy, addrSpace, indexOperands.drop_front(), memberMeta, locOffset,
                                               interpLoc, auxInterpValue, isPerVertexDimension);
    return m_builder->CreateInsertValue(inOutValue, loadValue, {memberIdx});
  }

  if (inOutTy->isVectorTy()) {
    Type *loadTy = inOutTy;
    Value *compIdx = nullptr;
    if (!indexOperands.empty() && isa<ConstantInt>(indexOperands.front())) {
      // Loading a component of the vector
      loadTy = cast<VectorType>(inOutTy)->getElementType();
      compIdx = indexOperands.front();
      Value *compValue = addCallInstForInOutImport(loadTy, addrSpace, inOutMetaVal, locOffset, 0, compIdx, nullptr,
                                                   interpLoc, auxInterpValue, isPerVertexDimension);
      return m_builder->CreateInsertElement(inOutValue, compValue, compIdx);
    }
    return addCallInstForInOutImport(loadTy, addrSpace, inOutMetaVal, locOffset, 0, compIdx, nullptr, interpLoc,
                                     auxInterpValue, isPerVertexDimension);
  }

  // Simple scalar type
  return addCallInstForInOutImport(inOutTy, addrSpace, inOutMetaVal, locOffset, 0, nullptr, nullptr, interpLoc,
                                   auxInterpValue, isPerVertexDimension);
}

// =====================================================================================================================
// Inserts instructions to load value from input/ouput member.
//
// @param inOutTy : Type of this input/output member
// @param loadTy : Type of load instruction
// @param addrSpace : Address space
// @param indexOperands : Index operands to process.
// @param maxLocOffset : Max+1 location offset if variable index has been encountered
// @param inOutMetaVal : Metadata of this input/output member
// @param locOffset : Relative location offset of this input/output member
// @param vertexIdx : Input/output array outermost index used for vertex indexing
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader): - Sample ID for
// "InterpLocSample" - Offset from the center of the pixel for "InterpLocCenter" - Vertex no. (0 ~ 2) for
// "InterpLocCustom"
Value *SpirvLowerGlobal::loadInOutMember(Type *inOutTy, Type *loadTy, unsigned addrSpace,
                                         ArrayRef<Value *> indexOperands, unsigned maxLocOffset, Constant *inOutMetaVal,
                                         Value *locOffset, Value *vertexIdx, unsigned interpLoc, Value *auxInterpValue,
                                         bool isPerVertexDimension) {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageMesh || m_shaderStage == ShaderStageFragment);

  // indexOperands can be empty with mismatch of types, if zero-index GEP was removed and global is used directly by
  // load.
  if (indexOperands.empty() && inOutTy == loadTy) {
    // All indices have been processed
    return addCallInstForInOutImport(inOutTy, addrSpace, inOutMetaVal, locOffset, maxLocOffset, nullptr, vertexIdx,
                                     interpLoc, auxInterpValue, isPerVertexDimension);
  }

  if (inOutTy->isArrayTy()) {
    // Array type
    assert(inOutMetaVal->getNumOperands() == 4);
    ShaderInOutMetadata inOutMeta = {};

    inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaVal->getOperand(2))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaVal->getOperand(3))->getZExtValue();

    auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));
    auto elemTy = inOutTy->getArrayElementType();

    if (inOutMeta.IsBuiltIn) {
      auto elemIdx = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();
      return addCallInstForInOutImport(elemTy, addrSpace, elemMeta, locOffset, inOutTy->getArrayNumElements(), elemIdx,
                                       vertexIdx, interpLoc, auxInterpValue, isPerVertexDimension);
    }

    // NOTE: If the relative location offset is not specified, initialize it to 0.
    if (!locOffset)
      locOffset = m_builder->getInt32(0);

    Value *elemLocOffset = nullptr;

    if (inOutMeta.PerVertexDimension) {
      // The input is a pervertex variable. The location offset is 0.
      assert(inOutMeta.InterpMode == InterpModeCustom);
      auxInterpValue = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();
      elemLocOffset = m_builder->getInt32(0);
    } else {
      // elemLocOffset = locOffset + stride * elemIdx
      unsigned stride = cast<ConstantInt>(inOutMetaVal->getOperand(0))->getZExtValue();
      auto elemIdx = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();
      elemLocOffset = m_builder->CreateMul(m_builder->getInt32(stride), elemIdx);
      elemLocOffset = m_builder->CreateAdd(locOffset, elemLocOffset);

      // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
      // so it knows how many locations to mark as used by this access.
      if (maxLocOffset == 0 && !isa<ConstantInt>(elemIdx)) {
        maxLocOffset = cast<ConstantInt>(locOffset)->getZExtValue() + stride * inOutTy->getArrayNumElements();
      }
    }

    if (!indexOperands.empty())
      indexOperands = indexOperands.drop_front();

    return loadInOutMember(elemTy, loadTy, addrSpace, indexOperands, maxLocOffset, elemMeta, elemLocOffset, vertexIdx,
                           interpLoc, auxInterpValue, inOutMeta.PerVertexDimension);
  }

  if (inOutTy->isStructTy()) {
    // Struct type
    unsigned memberIdx = indexOperands.empty() ? 0 : cast<ConstantInt>(indexOperands.front())->getZExtValue();

    auto memberTy = inOutTy->getStructElementType(memberIdx);
    auto memberMeta = cast<Constant>(inOutMetaVal->getOperand(memberIdx));

    if (!indexOperands.empty())
      indexOperands = indexOperands.drop_front();

    return loadInOutMember(memberTy, loadTy, addrSpace, indexOperands, maxLocOffset, memberMeta, locOffset, vertexIdx,
                           interpLoc, auxInterpValue, isPerVertexDimension);
  }

  if (inOutTy->isVectorTy()) {
    // Vector type
    Type *loadTy = cast<VectorType>(inOutTy)->getElementType();
    Value *compIdx = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();

    return addCallInstForInOutImport(loadTy, addrSpace, inOutMetaVal, locOffset, maxLocOffset, compIdx, vertexIdx,
                                     interpLoc, auxInterpValue, isPerVertexDimension);
  }

  llvm_unreachable("Should never be called!");
  return nullptr;
}

// =====================================================================================================================
// Inserts instructions to store value to output member.
//
// @param outputTy : Type of this output member
// @param storeTy : Type of store instruction
// @param storeValue : Value stored to output member
// @param indexOperands : Index operands to process (if empty, all indices have been processed)
// @param maxLocOffset : Max+1 location offset if variable index has been encountered
// @param outputMetaVal : Metadata of this output member
// @param locOffset : Relative location offset of this output member
// @param vertexOrPrimitiveIdx : Input array outermost index used for vertex indexing
void SpirvLowerGlobal::storeOutputMember(Type *outputTy, Type *storeTy, Value *storeValue,
                                         ArrayRef<Value *> indexOperands, unsigned maxLocOffset,
                                         Constant *outputMetaVal, Value *locOffset, Value *vertexOrPrimitiveIdx) {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageMesh);

  // indexOperands can be empty with mismatch of types, if zero-index GEP was removed and global is used directly by
  // store.
  if (indexOperands.empty() && outputTy == storeTy) {
    // All indices have been processed
    return addCallInstForOutputExport(storeValue, outputMetaVal, locOffset, maxLocOffset, InvalidValue, 0, nullptr,
                                      vertexOrPrimitiveIdx, InvalidValue);
  }

  if (outputTy->isArrayTy()) {
    assert(outputMetaVal->getNumOperands() == 4);
    ShaderInOutMetadata outputMeta = {};

    outputMeta.U64All[0] = cast<ConstantInt>(outputMetaVal->getOperand(2))->getZExtValue();
    outputMeta.U64All[1] = cast<ConstantInt>(outputMetaVal->getOperand(3))->getZExtValue();

    auto elemMeta = cast<Constant>(outputMetaVal->getOperand(1));
    auto elemTy = outputTy->getArrayElementType();

    if (outputMeta.IsBuiltIn) {
      assert(!locOffset);
      assert(indexOperands.empty() || indexOperands.size() == 1);

      auto elemIdx = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();
      return addCallInstForOutputExport(storeValue, elemMeta, nullptr, outputTy->getArrayNumElements(), InvalidValue, 0,
                                        elemIdx, vertexOrPrimitiveIdx, InvalidValue);
    }

    // NOTE: If the relative location offset is not specified, initialize it.
    if (!locOffset)
      locOffset = m_builder->getInt32(0);

    // elemLocOffset = locOffset + stride * elemIdx
    unsigned stride = cast<ConstantInt>(outputMetaVal->getOperand(0))->getZExtValue();
    auto elemIdx = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();
    Value *elemLocOffset = m_builder->CreateMul(m_builder->getInt32(stride), elemIdx);
    elemLocOffset = m_builder->CreateAdd(locOffset, elemLocOffset);

    // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
    // so it knows how many locations to mark as used by this access.
    if (maxLocOffset == 0 && !isa<ConstantInt>(elemIdx)) {
      maxLocOffset = cast<ConstantInt>(locOffset)->getZExtValue() + stride * outputTy->getArrayNumElements();
    }

    if (!indexOperands.empty())
      indexOperands = indexOperands.drop_front();

    return storeOutputMember(elemTy, storeTy, storeValue, indexOperands, maxLocOffset, elemMeta, elemLocOffset,
                             vertexOrPrimitiveIdx);
  }

  if (outputTy->isStructTy()) {
    // Structure type
    unsigned memberIdx = indexOperands.empty() ? 0 : cast<ConstantInt>(indexOperands.front())->getZExtValue();

    auto memberTy = outputTy->getStructElementType(memberIdx);
    auto memberMeta = cast<Constant>(outputMetaVal->getOperand(memberIdx));

    if (!indexOperands.empty())
      indexOperands = indexOperands.drop_front();

    return storeOutputMember(memberTy, storeTy, storeValue, indexOperands, maxLocOffset, memberMeta, locOffset,
                             vertexOrPrimitiveIdx);
  }

  if (outputTy->isVectorTy()) {
    // Vector type
    assert(indexOperands.empty() || indexOperands.size() == 1);
    auto compIdx = indexOperands.empty() ? m_builder->getInt32(0) : indexOperands.front();

    return addCallInstForOutputExport(storeValue, outputMetaVal, locOffset, maxLocOffset, InvalidValue, 0, compIdx,
                                      vertexOrPrimitiveIdx, InvalidValue);
  }

  llvm_unreachable("Should never be called!");
}

// =====================================================================================================================
// Lowers buffer blocks.
void SpirvLowerGlobal::lowerBufferBlock() {
  SmallVector<GlobalVariable *, 8> globalsToRemove;

  // With opaque pointers actually any instruction can be the user of the global variable since, zero-index GEPs
  // are removed. However we need to handle non-zero-index GEPs and selects differently.
  struct ReplaceInstsInfo {
    Instruction *otherInst;                           // This can be any instruction which is using global, since
                                                      // opaque pointers are removing zero-index GEP.
    SelectInst *selectInst;                           // The user is a select
    SmallVector<GetElementPtrInst *> getElemPtrInsts; // The user is a GEP. If the user is a select, we store its users.
  };

  // Skip the globals that are handled with previous global.
  SmallSet<GlobalVariable *, 4> skipGlobals;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a block or default uniform or acceleration structure.
    if ((global.getAddressSpace() != SPIRAS_Uniform && global.getAddressSpace() != SPIRAS_Constant) ||
        global.hasMetadata(gSPIRVMD::UniformConstant) || global.hasMetadata(gSPIRVMD::TaskPayload))
      continue;
    if (skipGlobals.count(&global) > 0) {
      globalsToRemove.push_back(&global);
      continue;
    }

    bool isAccelerationStructure = false;
    bool isAliased = false;
    MDNode *blockMetaNode = global.getMetadata(gSPIRVMD::Block);
    if (blockMetaNode) {
      ShaderBlockMetadata blockMeta = {};
      auto blockMetaNodeVal = mdconst::extract<Constant>(blockMetaNode->getOperand(0));
      if (auto meta = dyn_cast<ConstantInt>(blockMetaNodeVal)) {
        blockMeta.U64All = meta->getZExtValue();
      } else if (auto metaStruct = dyn_cast<ConstantStruct>(blockMetaNodeVal)) {
        const Constant *metaStructVal = metaStruct->getOperand(0);
        blockMeta.U64All = cast<ConstantInt>(metaStructVal)->getZExtValue();
      }
      isAccelerationStructure = blockMeta.IsAccelerationStructure;
      isAliased = blockMeta.Aliased;
    }

    if (global.getAddressSpace() == SPIRAS_Constant && !isAccelerationStructure)
      continue;

    MDNode *const resMetaNode = global.getMetadata(gSPIRVMD::Resource);
    assert(resMetaNode);

    const unsigned descSet = mdconst::extract<ConstantInt>(resMetaNode->getOperand(0))->getZExtValue();
    const unsigned binding = mdconst::extract<ConstantInt>(resMetaNode->getOperand(1))->getZExtValue();

    // AtomicCounter is emulated following same impl of SSBO, only qualifier 'offset' will be used in its
    // MD now. Using a new MD kind to detect it. AtomicCounter's type should be uint, not a structure.
    // We will use GEP to access them.
    MDNode *atomicCounterMD = global.getMetadata(gSPIRVMD::AtomicCounter);
    ShaderBlockMetadata atomicCounterMeta = {};
    if (atomicCounterMD) {
      atomicCounterMeta.U64All =
          cast<ConstantInt>(mdconst::extract<Constant>(atomicCounterMD->getOperand(0)))->getZExtValue();
    }

    convertUsersOfConstantsToInstructions(&global);

    // Record of all the functions that our global is used within.
    SmallSet<Function *, 4> funcsUsedIn;

    for (User *const user : global.users()) {
      if (Instruction *const inst = dyn_cast<Instruction>(user))
        funcsUsedIn.insert(inst->getFunction());
    }

    // Collect the instructions to be replaced per-global
    SmallVector<ReplaceInstsInfo> instructionsToReplace;
    bool isConstant = false;
    bool isReadOnly = true;

    for (Function *const func : funcsUsedIn) {
      SmallVector<Value *, 4> worklist;
      for (User *const user : global.users())
        worklist.push_back(user);

      while (!worklist.empty()) {
        Value *current = worklist.pop_back_val();
        if (auto inst = dyn_cast<Instruction>(current)) {
          if (inst->getFunction() != func)
            continue;

          if (auto *GEP = dyn_cast<GetElementPtrInst>(inst)) {
            for (auto *gepUser : GEP->users())
              worklist.push_back(gepUser);
            continue;
          }

          if (auto load = dyn_cast<LoadInst>(inst))
            if (!load->isAtomic())
              continue;

          // Anything that is not a load prevents the buffer being treated as readonly.
          isReadOnly = false;
          break;
        }
      }

      if (global.isConstant() || (isReadOnly && !isAliased))
        isConstant = true;

      // Check if our block is an array of blocks.
      if (!atomicCounterMD && global.getValueType()->isArrayTy()) {
        Type *const elementType = global.getValueType()->getArrayElementType();

        // We need to run over the users of the global, find the GEPs, and add a load for each.
        for (User *const user : global.users()) {
          // Skip over non-instructions.
          if (auto *inst = dyn_cast<Instruction>(user)) {
            // Skip instructions in other functions.
            if (inst->getFunction() != func)
              continue;

            ReplaceInstsInfo replaceInstsInfo = {};
            // We have a user of the global, expect a GEP, a bitcast or a select.
            if (auto *getElemPtr = dyn_cast<GetElementPtrInst>(inst)) {
              replaceInstsInfo.getElemPtrInsts.push_back(getElemPtr);
            } else if (auto *selectInst = dyn_cast<SelectInst>(inst)) {
              // The users of the select must be a GEP.
              assert(selectInst->getTrueValue() == &global || selectInst->getFalseValue() == &global);
              replaceInstsInfo.selectInst = selectInst;
              for (User *selectUser : selectInst->users()) {
                if (auto *userInst = dyn_cast<Instruction>(selectUser)) {
                  assert(userInst->getFunction() == func);
                  if (auto *getElemPtr = dyn_cast<GetElementPtrInst>(userInst))
                    replaceInstsInfo.getElemPtrInsts.push_back(getElemPtr);
                }
              }
            } else {
              replaceInstsInfo.otherInst = inst;
            }
            instructionsToReplace.push_back(replaceInstsInfo);
          }
        }

        for (const auto &replaceInstsInfo : instructionsToReplace) {
          if (replaceInstsInfo.otherInst) {
            // All instructions here are for GEPs that indexed by 0, 0 into the arrayed resource. Opaque
            // pointers are removing zero-index GEPs and BitCast with pointer to pointer cast.
            m_builder->SetInsertPoint(replaceInstsInfo.otherInst);
            unsigned bufferFlags = global.isConstant() ? 0 : lgc::Builder::BufferFlagWritten;

            auto descTy = lgc::ResourceNodeType::DescriptorBuffer;
            Value *const bufferDesc =
                isAccelerationStructure
                    ? m_builder->CreateGetDescPtr(descTy, descTy, descSet, binding)
                    : m_builder->create<lgc::LoadBufferDescOp>(descSet, binding, m_builder->getInt32(0), bufferFlags);

            // If the global variable is a constant, the data it points to is invariant.
            if (isConstant)
              m_builder->CreateInvariantStart(bufferDesc);

            replaceInstsInfo.otherInst->replaceUsesOfWith(&global, bufferDesc);
          } else {
            assert(!replaceInstsInfo.getElemPtrInsts.empty());

            for (GetElementPtrInst *getElemPtr : replaceInstsInfo.getElemPtrInsts) {
              // The second index is the block offset, so we need at least two indices!
              assert(getElemPtr->getNumIndices() >= 2);
              SmallVector<Value *, 8> indices;

              // Types of Global Variable and GEP can be different, these may happen when zero-index elimination
              // occurred. For opaque pointers this is quite often. If types are not equal it means leading zeros where
              // removed and we can assume that BlockIndex is '0' (since second index is describing BlockIndex).
              bool isBlockIndexZero = getElemPtr->getSourceElementType() != global.getValueType();
              bool gepsLeadingZerosEliminated = isBlockIndexZero;

              for (Value *const index : getElemPtr->indices())
                indices.push_back(index);

              // Verify GEPs indices if zero-index elimination did not occurred.
              assert(gepsLeadingZerosEliminated ||
                     (isa<ConstantInt>(indices[0]) && cast<ConstantInt>(indices[0])->getZExtValue() == 0));

              // Get block index from the second gep index, if it is not zero.
              Value *const blockIndex = isBlockIndexZero ? m_builder->getInt32(0) : indices[1];

              bool isNonUniform = isShaderStageInMask(
                  m_shaderStage,
                  m_context->getPipelineContext()->getPipelineOptions()->forceNonUniformResourceIndexStageMask);

              if (!isNonUniform) {
                // Run the users of the GEP to check for any nonuniform calls.
                for (User *const user : getElemPtr->users()) {
                  CallInst *const call = dyn_cast<CallInst>(user);
                  // If the user is not a call or the call is the function pointer call, bail.
                  if (!call)
                    continue;
                  auto callee = call->getCalledFunction();
                  if (!callee)
                    continue;
                  // If the call is our non uniform decoration, record we are non uniform.
                  isNonUniform = callee->getName().starts_with(gSPIRVName::NonUniform);
                  break;
                }
              }
              if (!isNonUniform) {
                // Run the users of the block index to check for any nonuniform calls.
                for (User *const user : blockIndex->users()) {
                  CallInst *const call = dyn_cast<CallInst>(user);

                  // If the user is not a call, bail.
                  if (!call)
                    continue;
                  // If the call is our non uniform decoration, record we are non uniform.
                  auto callee = call->getCalledFunction();
                  if (callee && callee->getName().starts_with(gSPIRVName::NonUniform)) {
                    isNonUniform = true;
                    break;
                  }
                }
              }

              // If the user of the global is a GEP, we need specify blockIndex to invoke CreateLoadBufferDesc and
              // remove the second index (blockIndex) from GEP indices. If the user of the global is a select, the
              // bufferFlags and blockIndex are obtained from the GEP (select's user) to respectively invoke
              // CreateLoadBufferDesc for the true and false value of the select.
              auto &select = replaceInstsInfo.selectInst;
              if (select)
                m_builder->SetInsertPoint(select);
              else
                m_builder->SetInsertPoint(getElemPtr);

              unsigned bufferFlags = 0;
              if (isNonUniform)
                bufferFlags |= lgc::Builder::BufferFlagNonUniform;
              if (!global.isConstant())
                bufferFlags |= lgc::Builder::BufferFlagWritten;

              Value *bufferDescs[2] = {nullptr};
              unsigned descSets[2] = {descSet, 0};
              unsigned bindings[2] = {binding, 0};
              GlobalVariable *globals[2] = {&global, nullptr};
              const unsigned descCount = replaceInstsInfo.selectInst ? 2 : 1;
              if (descCount == 2) {
                // The true value and false value must be global variable
                assert(isa<GlobalVariable>(select->getTrueValue()));
                assert(isa<GlobalVariable>(select->getFalseValue()));
                globals[0] = cast<GlobalVariable>(select->getTrueValue());
                globals[1] = cast<GlobalVariable>(select->getFalseValue());
                unsigned nextGlobalIdx = (&global == select->getTrueValue()) ? 1 : 0;

                MDNode *const resMetaNode1 = globals[nextGlobalIdx]->getMetadata(gSPIRVMD::Resource);
                assert(resMetaNode);
                descSets[1] = mdconst::extract<ConstantInt>(resMetaNode1->getOperand(0))->getZExtValue();
                bindings[1] = mdconst::extract<ConstantInt>(resMetaNode1->getOperand(1))->getZExtValue();

                if (!nextGlobalIdx) {
                  std::swap(descSets[0], descSets[1]);
                  std::swap(bindings[0], bindings[1]);
                }
                skipGlobals.insert(globals[nextGlobalIdx]);
              }
              for (unsigned idx = 0; idx < descCount; ++idx) {
                if (isAccelerationStructure) {
                  // For acceleration structure array, get the base descriptor pointer and use index + stride to access
                  // the actual needed pointer.
                  auto descTy = lgc::ResourceNodeType::DescriptorBuffer;
                  auto descPtr = m_builder->CreateGetDescPtr(descTy, descTy, descSet, binding);
                  auto stride = m_builder->CreateGetDescStride(descTy, descTy, descSet, binding);
                  auto index = m_builder->CreateMul(blockIndex, stride);
                  bufferDescs[idx] = m_builder->CreateGEP(m_builder->getInt8Ty(), descPtr, index);
                } else {
                  bufferDescs[idx] =
                      m_builder->create<lgc::LoadBufferDescOp>(descSets[idx], bindings[idx], blockIndex, bufferFlags);
                }
                // If the global variable is a constant, the data it points to is invariant.
                if (isConstant)
                  m_builder->CreateInvariantStart(bufferDescs[idx]);
              }

              Value *newSelect = nullptr;
              if (select)
                newSelect = m_builder->CreateSelect(select->getCondition(), bufferDescs[0], bufferDescs[1]);

              Value *base = newSelect ? newSelect : bufferDescs[0];
              // We need to remove the block index from the original GEP indices so that we can use them, but first we
              // have to check if it was not removed already by zero-index elimination.
              if (!gepsLeadingZerosEliminated)
                indices[1] = indices[0];

              ArrayRef<Value *> newIndices(indices);
              // Drop first index only if it was not removed earlier by zero-index elimination while creating GEP
              // instructions.
              if (!gepsLeadingZerosEliminated)
                newIndices = newIndices.drop_front(1);

              Value *newGetElemPtr = nullptr;
              // If zero-index elimination removed leading zeros from OldGEP indices then we need to use OldGEP Source
              // type as a Source type for newGEP. In other cases use global variable array element type.
              Type *newGetElemType = gepsLeadingZerosEliminated ? getElemPtr->getSourceElementType() : elementType;

              if (getElemPtr->isInBounds())
                newGetElemPtr = m_builder->CreateInBoundsGEP(newGetElemType, base, newIndices);
              else
                newGetElemPtr = m_builder->CreateGEP(newGetElemType, base, newIndices);

              getElemPtr->replaceAllUsesWith(newGetElemPtr);
              getElemPtr->eraseFromParent();

              if (select)
                select->eraseFromParent();
            }
          }
        }
      } else {
        m_builder->SetInsertPointPastAllocas(func);
        unsigned bufferFlags = global.isConstant() ? 0 : lgc::Builder::BufferFlagWritten;

        auto descTy = lgc::ResourceNodeType::DescriptorBuffer;
        Value *const bufferDesc =
            isAccelerationStructure
                ? m_builder->CreateGetDescPtr(descTy, descTy, descSet, binding)
                : m_builder->create<lgc::LoadBufferDescOp>(descSet, binding, m_builder->getInt32(0), bufferFlags);

        SmallVector<Instruction *, 8> usesToReplace;
        for (User *const user : global.users()) {
          // Skip over non-instructions that we've already made useless.
          if (!isa<Instruction>(user))
            continue;

          Instruction *const inst = cast<Instruction>(user);

          // Skip instructions in other functions.
          if (inst->getFunction() != func)
            continue;

          usesToReplace.push_back(inst);
        }

        // If the global variable is a constant, the data it points to is invariant.
        if (isConstant)
          m_builder->CreateInvariantStart(bufferDesc);

        Value *newLoadPtr = bufferDesc;
        if (atomicCounterMD) {
          SmallVector<Value *, 8> indices;
          indices.push_back(m_builder->getInt32(atomicCounterMeta.offset / 4));
          newLoadPtr = m_builder->CreateInBoundsGEP(m_builder->getInt32Ty(), bufferDesc, indices);
        }

        for (Instruction *const use : usesToReplace)
          use->replaceUsesOfWith(&global, newLoadPtr);
      }
    }

    globalsToRemove.push_back(&global);
  }

  for (GlobalVariable *const global : globalsToRemove) {
    global->dropAllReferences();
    global->eraseFromParent();
  }
}

// =====================================================================================================================
// Lowers aliased variables.
void SpirvLowerGlobal::lowerAliasedVal() {
  // NOTE: When enable CapabilityWorkgroupMemoryExplicitLayoutKHR, Workgroup variables can be declared in blocks,
  // and then use the same explicit layout decorations (e.g. Offset, ArrayStride) as other storage classes. All the
  // Workgroup blocks share the same underlying storage, and either all or none of the variables must be explicitly
  // laid out.
  std::vector<GlobalVariable *> aliasedVals;
  unsigned maxInBits = 0;
  unsigned index = 0;
  // Aliased variables can contain different byte size, we require the maximum size to be as base variable to replace
  // the others.
  for (GlobalVariable &global : m_module->globals()) {
    auto addrSpace = global.getType()->getAddressSpace();
    if (addrSpace == SPIRAS_Local) {
      auto meta = global.getMetadata(gSPIRVMD::Lds);
      if (!meta)
        return;
      const unsigned aliased = mdconst::extract<ConstantInt>(meta->getOperand(0))->getZExtValue();
      if (aliased) {
        unsigned inBits = static_cast<unsigned>(m_module->getDataLayout().getTypeSizeInBits(global.getValueType()));
        if (inBits > maxInBits) {
          maxInBits = inBits;
          index = aliasedVals.size();
        }
        aliasedVals.push_back(&global);
      }
    }
  }

  for (uint32_t i = 0; i < aliasedVals.size(); i++) {
    if (i != index)
      replaceGlobal(m_context, aliasedVals[i], aliasedVals[index]);
  }
}

// =====================================================================================================================
// Lowers task payload.
void SpirvLowerGlobal::lowerTaskPayload() {
  GlobalVariable *globalToRemove = nullptr;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a task payload
    if (!global.hasMetadata(gSPIRVMD::TaskPayload))
      continue;

    convertUsersOfConstantsToInstructions(&global);

    SmallVector<Instruction *, 8> instsToReplace;
    for (User *const user : global.users()) {
      Instruction *const inst = cast<Instruction>(user);
      instsToReplace.push_back(inst);
    }

    SmallDenseMap<Function *, Value *> taskPayloads;
    for (Instruction *const inst : instsToReplace) {
      Value *taskPayload = nullptr;
      auto func = inst->getFunction();
      if (taskPayloads.find(func) == taskPayloads.end()) {
        m_builder->SetInsertPointPastAllocas(inst->getFunction());
        taskPayload = m_builder->create<lgc::TaskPayloadPtrOp>();
        taskPayloads[func] = taskPayload;
      } else {
        // The task payload global has already been lowered, just use it.
        taskPayload = taskPayloads[func];
      }
      inst->replaceUsesOfWith(&global, taskPayload);
    }

    globalToRemove = &global;
    break;
  }

  if (globalToRemove) {
    globalToRemove->dropAllReferences();
    globalToRemove->eraseFromParent();
  }
}

// =====================================================================================================================
// Lowers push constants.
void SpirvLowerGlobal::lowerPushConsts() {
  SmallVector<GlobalVariable *, 1> globalsToRemove;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a push constant.
    if (global.getAddressSpace() != SPIRAS_Constant || !global.hasMetadata(gSPIRVMD::PushConst))
      continue;

    // There should only be a single push constant variable!
    assert(globalsToRemove.empty());

    convertUsersOfConstantsToInstructions(&global);

    // Record of all the functions that our global is used within.
    SmallSet<Function *, 4> funcsUsedIn;

    for (User *const user : global.users()) {
      if (Instruction *const inst = dyn_cast<Instruction>(user))
        funcsUsedIn.insert(inst->getFunction());
    }

    for (Function *const func : funcsUsedIn) {
      m_builder->SetInsertPointPastAllocas(func);

      Value *pushConstants = m_builder->CreateLoadPushConstantsPtr();

      auto addrSpace = pushConstants->getType()->getPointerAddressSpace();
      Type *const castType = global.getValueType()->getPointerTo(addrSpace);
      pushConstants = m_builder->CreateBitCast(pushConstants, castType);

      SmallVector<Instruction *, 8> usesToReplace;

      for (User *const user : global.users()) {
        // Skip over non-instructions that we've already made useless.
        if (!isa<Instruction>(user))
          continue;

        Instruction *const inst = cast<Instruction>(user);

        // Skip instructions in other functions.
        if (inst->getFunction() != func)
          continue;

        usesToReplace.push_back(inst);
      }

      for (Instruction *const inst : usesToReplace)
        inst->replaceUsesOfWith(&global, pushConstants);
    }

    globalsToRemove.push_back(&global);
  }

  for (GlobalVariable *const global : globalsToRemove) {
    global->dropAllReferences();
    global->eraseFromParent();
  }
}

// =====================================================================================================================
// Lowers uniform constants.
void SpirvLowerGlobal::lowerUniformConstants() {
  SmallVector<GlobalVariable *, 1> globalsToRemove;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a default uniform.
    if (global.getAddressSpace() != SPIRAS_Uniform || !global.hasMetadata(gSPIRVMD::UniformConstant))
      continue;

    convertUsersOfConstantsToInstructions(&global);

    // A map from the function to the instructions inside it which access the global variable.
    SmallMapVector<Function *, SmallVector<Instruction *>, 8> globalUsers;

    for (User *const user : global.users()) {
      Instruction *inst = cast<Instruction>(user);
      globalUsers[inst->getFunction()].push_back(inst);
    }

    for (auto &eachFunc : globalUsers) {
      MDNode *metaNode = global.getMetadata(gSPIRVMD::UniformConstant);
      auto uniformConstantsSet = mdconst::extract<ConstantInt>(metaNode->getOperand(0))->getZExtValue();
      auto uniformConstantsBinding = mdconst::extract<ConstantInt>(metaNode->getOperand(1))->getZExtValue();
      auto uniformConstantsOffset = mdconst::extract<ConstantInt>(metaNode->getOperand(2))->getZExtValue();

      m_builder->SetInsertPointPastAllocas(eachFunc.first);
      Value *bufferDesc = m_builder->create<lgc::LoadBufferDescOp>(
          uniformConstantsSet, uniformConstantsBinding, m_builder->getInt32(0), lgc::Builder::BufferFlagNonConst);
      Value *newPtr = m_builder->CreateConstInBoundsGEP1_32(m_builder->getInt8Ty(), bufferDesc, uniformConstantsOffset);
      for (auto *inst : eachFunc.second)
        inst->replaceUsesOfWith(&global, newPtr);
    }

    globalsToRemove.push_back(&global);
  }

  for (GlobalVariable *const global : globalsToRemove) {
    global->dropAllReferences();
    global->eraseFromParent();
  }
}

// =====================================================================================================================
// Interpolates an element of the input.
//
// @param returnTy : the return type of the interpolation
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader): - Sample ID for
// "InterpLocSample" - Offset from the center of the pixel for "InterpLocCenter" - Vertex no. (0 ~ 2) for
// "InterpLocCustom"
// @param callInst : "Call" instruction
// @param indexOperands : indices of GEP instruction
// @param gv : Global Variable instruction
Value *SpirvLowerGlobal::interpolateInputElement(Type *returnTy, unsigned interpLoc, Value *auxInterpValue,
                                                 GlobalVariable *gv, ArrayRef<Value *> indexOperands) {
  assert((indexOperands.empty() || cast<ConstantInt>(indexOperands.front())->isZero()) && "Non-zero GEP first index\n");

  auto inputTy = gv->getValueType();

  MDNode *metaNode = gv->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto inputMeta = mdconst::extract<Constant>(metaNode->getOperand(0));

  auto hasAllConstantIndices = [](ArrayRef<Value *> &indexOperands) {
    // if indexOperands is empty then add_of will return TRUE.
    return std::all_of(indexOperands.begin(), indexOperands.end(), [](auto &idx) {
      if (isa<ConstantInt>(idx))
        return true;
      return false;
    });
  };

  if (hasAllConstantIndices(indexOperands)) {
    if (!indexOperands.empty())
      indexOperands = indexOperands.drop_front();
    return loadInOutMember(inputTy, returnTy, SPIRAS_Input, indexOperands, 0, inputMeta, nullptr, nullptr, interpLoc,
                           auxInterpValue, false);
  }

  // Interpolate an element via dynamic index by extending interpolant to each element
  //
  // Regardless of where we do the interpolation, the alloca for the temporary must be inserted in the function entry
  // block for efficient code generation, so we don't use the builder for it.
  auto interpPtr = m_builder->CreateAllocaAtFuncEntry(inputTy);
  // Load all possibly accessed values
  auto loadValue = loadDynamicIndexedMembers(inputTy, SPIRAS_Input, ArrayRef(indexOperands).drop_front(), inputMeta,
                                             nullptr, interpLoc, auxInterpValue, false);

  m_builder->CreateStore(loadValue, interpPtr);

  auto interpElemPtr = m_builder->CreateGEP(inputTy, interpPtr, indexOperands);
  auto interpElemTy = GetElementPtrInst::getIndexedType(inputTy, indexOperands);

  // Only get the value that the original getElemPtr points to
  return m_builder->CreateLoad(interpElemTy, interpElemPtr);
}

// =====================================================================================================================
// Fill the XFB info map from the Vkgc::ApiXfbOutData if XFB is specified by API interface
void SpirvLowerGlobal::buildApiXfbMap() {
  auto pipelineBuildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  for (unsigned idx = 0; idx < pipelineBuildInfo->getGlState().apiXfbOutData.numXfbOutInfo; ++idx) {
    const auto &xfbInfo = pipelineBuildInfo->getGlState().apiXfbOutData.pXfbOutInfos[idx];
    unsigned location = xfbInfo.location;
    if (xfbInfo.isBuiltIn) {
      if (m_builtInXfbMap.find(location) == m_builtInXfbMap.end()) {
        // For built-in array type, we only need add one the first item.
        m_builtInXfbMap[location] = xfbInfo;
      }
    } else {
      m_genericXfbMap[location] = xfbInfo;
    }
  }
}

// =====================================================================================================================
// Check if the given output should be written to an XFB buffer and inserts LLVM call instruction to XFB output.
//
// @param outputMeta: the metadata of output
// @param outputValue : Value to write
// @param xfbBufferAdjust : Adjustment of transform feedback buffer ID for XFB qualifier (for array type, default is 0)
// @param xfbOffsetAdjust : Adjustment of transform feedback offset (for array type)
// @param locOffset : Relative location offset, passed from aggregate type
// @param outputInfo : Extra output info (GS stream ID)
void SpirvLowerGlobal::addCallInstForXfbOutput(const ShaderInOutMetadata &outputMeta, Value *outputValue,
                                               unsigned xfbBufferAdjust, unsigned xfbOffsetAdjust, unsigned locOffset,
                                               lgc::InOutInfo outputInfo) {
  assert(m_shaderStage == m_lastVertexProcessingStage);
  DenseMap<unsigned, Vkgc::XfbOutInfo> *locXfbMapPtr = outputMeta.IsBuiltIn ? &m_builtInXfbMap : &m_genericXfbMap;
  bool hasXfbMetadata = m_entryPoint->getMetadata(lgc::XfbStateMetadataName);
  bool hasXfbOut = hasXfbMetadata && (!locXfbMapPtr->empty() || outputMeta.IsXfb);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 70
  auto pipelineBuildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  hasXfbOut &= !pipelineBuildInfo->apiXfbOutData.forceDisableStreamOut;
#endif
  if (!hasXfbOut)
    return;

  // If the XFB info is specified from API interface so we try to retrieve the info from m_locXfbMap. Otherwise, the XFB
  // info is obtained from the output metadata.
  unsigned xfbBuffer = InvalidValue;
  unsigned xfbStride = 0;
  unsigned xfbOffset = 0;
  unsigned location = outputMeta.Value; // builtInId
  assert(!outputMeta.IsBuiltIn || locOffset == 0);
  if (!outputMeta.IsBuiltIn)
    location += locOffset;

  if (locXfbMapPtr->size() > 0) {
    auto iter = locXfbMapPtr->find(location);
    if (iter == locXfbMapPtr->end())
      return;
    // Use API interface
    xfbBuffer = iter->second.xfbBuffer;
    xfbStride = iter->second.xfbStride;
    xfbOffset = iter->second.xfbOffset;
    if (outputMeta.IsBuiltIn)
      xfbOffset += xfbOffsetAdjust;
    else
      outputInfo.setComponent(iter->second.component);
  } else {
    assert(outputMeta.IsXfb);
    // Use XFB qualifier
    assert(xfbOffsetAdjust != InvalidValue && (!outputMeta.IsBuiltIn || xfbBufferAdjust == 0));
    xfbBuffer = outputMeta.XfbBuffer + xfbBufferAdjust;
    xfbStride = outputMeta.XfbStride;
    xfbOffset = outputMeta.XfbOffset + outputMeta.XfbExtraOffset + xfbOffsetAdjust;
  }

  m_builder->CreateWriteXfbOutput(outputValue, outputMeta.IsBuiltIn, location, xfbBuffer, xfbStride,
                                  m_builder->getInt32(xfbOffset), outputInfo);

  if (!m_printedXfbInfo) {
    LLPC_OUTS("\n===============================================================================\n");
    LLPC_OUTS("// LLPC transform feedback export info (" << getShaderStageName(m_shaderStage) << " shader)\n\n");

    m_printedXfbInfo = true;
  }
  LLPC_OUTS(*outputValue->getType());
  if (outputMeta.IsBuiltIn) {
    auto builtInId = static_cast<BuiltIn>(outputMeta.Value);
    auto builtInName = getNameMap(builtInId).map(builtInId);
    LLPC_OUTS(" (builtin = " << builtInName.substr(strlen("BuiltIn")) << "), ");
  } else {
    LLPC_OUTS(" (loc = " << location << ", comp = " << outputMeta.Component << "), ");
  }
  LLPC_OUTS("xfbBuffer = " << xfbBuffer << ", "
                           << "xfbStride = " << xfbStride << ", "
                           << "xfbOffset = " << xfbOffset << ", "
                           << "streamID = " << outputMeta.StreamId << "\n");
}

// =====================================================================================================================
// Lowers shader record buffer.
void SpirvLowerGlobal::lowerShaderRecordBuffer() {
  // Note: Only ray tracing pipeline has shader record buffer
  if (m_context->getPipelineType() != PipelineType::RayTracing)
    return;

  static const char *ShaderRecordBuffer = "ShaderRecordBuffer";
  for (GlobalVariable &global : m_module->globals()) {
    if (!global.getName().starts_with(ShaderRecordBuffer))
      continue;

    convertUsersOfConstantsToInstructions(&global);

    m_builder->SetInsertPointPastAllocas(m_entryPoint);
    auto shaderRecordBufferPtr = m_builder->create<ShaderRecordBufferOp>(m_builder->create<ShaderIndexOp>());

    global.mutateType(shaderRecordBufferPtr->getType()); // To clear address space for pointer to make replacement valid
    global.replaceAllUsesWith(shaderRecordBufferPtr);
    global.dropAllReferences();
    global.eraseFromParent();

    // There should be only one shader record buffer only
    return;
  }
}

// =====================================================================================================================
// Handles an input which is "volatile" (may change during execution).
//
// @param input : Input to be handled
// @param proxy : Proxy of the input
void SpirvLowerGlobal::handleVolatileInput(GlobalVariable *input, Value *proxy) {
  // For now, only check for RayTCurrent (BuiltInRayTmaxKHR, BuiltInHitTNV) in intersection shader.
  // TODO: Maybe also needed for BuiltInSubgroupLocalInvocationId and related.
  if (!input->getValueType()->isFloatTy())
    return;

  if (m_shaderStage != ShaderStageRayTracingIntersect)
    return;

  MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);

  auto meta = mdconst::extract<Constant>(metaNode->getOperand(0));

  ShaderInOutMetadata inOutMeta = {};
  inOutMeta.U64All[0] = cast<ConstantInt>(meta->getOperand(0))->getZExtValue();
  inOutMeta.U64All[1] = cast<ConstantInt>(meta->getOperand(1))->getZExtValue();

  if (!inOutMeta.IsBuiltIn)
    return;

  unsigned builtInId = inOutMeta.Value;

  switch (builtInId) {
  case BuiltInHitTNV:
  case BuiltInRayTmaxKHR: {
    struct Payload {
      lgc::Builder *builder;
      Value *proxy;
    };
    Payload payload = {m_builder, proxy};

    // RayTcurrent may change after OpReportIntersectionKHR, get and store it to proxy again after each Op is called.
    static auto reportHitVisitor = llvm_dialects::VisitorBuilder<Payload>()
                                       .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                       .add<ReportHitOp>([](auto &payload, auto &op) {
                                         payload.builder->SetInsertPoint(op.getNextNonDebugInstruction());
                                         auto newRayTCurrent = payload.builder->template create<RayTcurrentOp>();
                                         payload.builder->CreateStore(newRayTCurrent, payload.proxy);
                                       })
                                       .build();

    reportHitVisitor.visit(payload, *m_module);
    break;
  }
  default: {
    // Do nothing
    break;
  }
  }
}

// =====================================================================================================================
// Changes function signature for RT shaders. Specifically, add payload / hit attribute / callable data pointers and
// metadata to function signature.
void SpirvLowerGlobal::changeRtFunctionSignature() {
  if (!isRayTracingShaderStage(m_shaderStage))
    return;

  // Ray generation shader has no input payload or hit attributes
  if (m_shaderStage == ShaderStageRayTracingRayGen)
    return;

  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());

  ValueToValueMapTy VMap;
  SmallVector<Type *, 2> argTys;
  SmallVector<ReturnInst *, 8> retInsts;
  Type *pointerTy = PointerType::get(*m_context, SPIRAS_Private);
  switch (m_shaderStage) {
  case ShaderStageRayTracingIntersect:
    // We don't have hit attribute in argument for IS in continuations mode.
    if (rayTracingContext->isContinuationsMode())
      break;
    LLVM_FALLTHROUGH; // Fall through: Legacy RT still requires hit attribute in argument
  case ShaderStageRayTracingAnyHit:
  case ShaderStageRayTracingClosestHit:
    // Hit attribute
    argTys.push_back(pointerTy);
    LLVM_FALLTHROUGH; // Fall through: Handle payload
  case ShaderStageRayTracingMiss:
    // Payload
    argTys.push_back(pointerTy);
    setShaderPaq(m_entryPoint, getPaqFromSize(*m_context, rayTracingContext->getPayloadSizeInBytes()));
    break;
  case ShaderStageRayTracingCallable:
    // Callable data
    argTys.push_back(pointerTy);
    setShaderArgSize(m_entryPoint, rayTracingContext->getCallableDataSizeInBytes());
    break;
  default:
    llvm_unreachable("Should never be called");
  }

  assert(m_entryPoint->arg_empty());

  auto newFuncTy = FunctionType::get(m_entryPoint->getReturnType(), argTys, false);
  auto newFunc = Function::Create(newFuncTy, m_entryPoint->getLinkage(), "", m_module);
  newFunc->takeName(m_entryPoint);

  CloneFunctionInto(newFunc, m_entryPoint, VMap, CloneFunctionChangeType::LocalChangesOnly, retInsts);
  assert(m_entryPoint->use_empty());
  m_entryPoint->eraseFromParent();
  m_entryPoint = newFunc;

  GlobalVariable *hitAttributeVar = nullptr;
  GlobalVariable *incomingPayloadVar = nullptr;
  GlobalVariable *incomingCallableDataVar = nullptr;

  // NOTE: There could be multiple definitions of these variables in a SPIR-V file, but there could only have one of
  // each in used in current entry point.
  for (auto &global : m_module->globals()) {
    if (global.getNumUses() == 0)
      continue;
    if (global.getName().starts_with(SPIRVStorageClassNameMap::map(StorageClassHitAttributeKHR))) {
      assert(hitAttributeVar == nullptr);
      hitAttributeVar = &global;
    } else if (global.getName().starts_with(SPIRVStorageClassNameMap::map(StorageClassIncomingRayPayloadKHR))) {
      assert(incomingPayloadVar == nullptr);
      incomingPayloadVar = &global;
    } else if (global.getName().starts_with(SPIRVStorageClassNameMap::map(StorageClassIncomingCallableDataKHR))) {
      assert(incomingCallableDataVar == nullptr);
      incomingCallableDataVar = &global;
    }
  }

  SmallVector<GlobalVariable *> globalsToErase;

  if (hitAttributeVar && m_entryPoint->arg_size() == 2) {
    assert(!rayTracingContext->isContinuationsMode() || m_shaderStage != ShaderStageRayTracingIntersect);
    convertUsersOfConstantsToInstructions(hitAttributeVar);
    hitAttributeVar->replaceAllUsesWith(m_entryPoint->getArg(1));
    globalsToErase.push_back(hitAttributeVar);
  }

  if (incomingPayloadVar) {
    convertUsersOfConstantsToInstructions(incomingPayloadVar);
    incomingPayloadVar->replaceAllUsesWith(m_entryPoint->getArg(0));
    globalsToErase.push_back(incomingPayloadVar);
  } else if (incomingCallableDataVar) {
    convertUsersOfConstantsToInstructions(incomingCallableDataVar);
    incomingCallableDataVar->replaceAllUsesWith(m_entryPoint->getArg(0));
    globalsToErase.push_back(incomingCallableDataVar);
  }

  if (rayTracingContext->isContinuationsMode()) {
    SmallVector<TypedArgTy> contArgTys;

    // We don't have hit attribute in argument for IS in continuations mode.
    if (m_shaderStage != ShaderStageRayTracingIntersect) {
      auto var = m_shaderStage == ShaderStageRayTracingCallable ? incomingCallableDataVar : incomingPayloadVar;
      auto payloadTy = var ? var->getValueType() : StructType::get(*m_context);
      if (!isa<StructType>(payloadTy))
        payloadTy = StructType::get(*m_context, {payloadTy}, false);
      contArgTys.push_back(TypedArgTy(pointerTy, payloadTy));
      if ((m_shaderStage == ShaderStageRayTracingAnyHit) || (m_shaderStage == ShaderStageRayTracingClosestHit)) {
        auto type = ArrayType::get(m_builder->getInt32Ty(), rayTracingContext->getAttributeDataSize());
        contArgTys.push_back(TypedArgTy(pointerTy, type));
      }
    }

    TypedFuncTy contFuncTy(m_builder->getVoidTy(), contArgTys);
    contFuncTy.writeMetadata(newFunc);
  }

  for (auto globalVar : globalsToErase) {
    globalVar->dropAllReferences();
    globalVar->eraseFromParent();
  }
}

} // namespace Llpc
