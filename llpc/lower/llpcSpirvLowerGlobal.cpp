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
 * @file  llpcSpirvLowerGlobal.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerGlobal.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerGlobal.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <unordered_set>

#define DEBUG_TYPE "llpc-spirv-lower-global"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

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
// Initializes static members.
char LegacySpirvLowerGlobal::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for globals
ModulePass *createLegacySpirvLowerGlobal() {
  return new LegacySpirvLowerGlobal();
}

// =====================================================================================================================
SpirvLowerGlobal::SpirvLowerGlobal() : m_retBlock(nullptr), m_lowerInputInPlace(false), m_lowerOutputInPlace(false) {
}

// =====================================================================================================================
LegacySpirvLowerGlobal::LegacySpirvLowerGlobal() : ModulePass(ID) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
bool LegacySpirvLowerGlobal::runOnModule(Module &module) {
  return Impl.runImpl(module);
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerGlobal::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerGlobal::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Global\n");

  SpirvLower::init(&module);

  // Map globals to proxy variables
  for (auto global = m_module->global_begin(), end = m_module->global_end(); global != end; ++global) {
    if (global->getType()->getAddressSpace() == SPIRAS_Private)
      mapGlobalVariableToProxy(&*global);
    else if (global->getType()->getAddressSpace() == SPIRAS_Input)
      mapInputToProxy(&*global);
    else if (global->getType()->getAddressSpace() == SPIRAS_Output)
      mapOutputToProxy(&*global);
  }

  // NOTE: Global variable, include general global variable, input and output is a special constant variable, so if
  // it is referenced by constant expression, we need translate constant expression to normal instruction first,
  // Otherwise, we will hit assert in replaceAllUsesWith() when we replace global variable with proxy variable.
  for (GlobalVariable &global : m_module->globals()) {
    auto addrSpace = global.getType()->getAddressSpace();

    // Remove constant expressions for global variables in these address spaces
    bool isGlobalVar = addrSpace == SPIRAS_Private || addrSpace == SPIRAS_Input || addrSpace == SPIRAS_Output;

    if (!isGlobalVar)
      continue;
    removeConstantExpr(m_context, &global);
  }

  // Do lowering operations
  lowerGlobalVar();

  if (m_lowerInputInPlace && m_lowerOutputInPlace) {
    // Both input and output have to be lowered in-place (without proxy variables)
    lowerInOutInPlace(); // Just one lowering operation is sufficient
  } else {
    // Either input or output has to be lowered in-place, not both
    if (m_lowerInputInPlace)
      lowerInOutInPlace();
    else
      lowerInput();

    if (m_lowerOutputInPlace)
      lowerInOutInPlace();
    else
      lowerOutput();
  }

  lowerBufferBlock();
  lowerPushConsts();
  lowerAliasedVal();

  cleanupReturnBlock();

  return true;
}

// =====================================================================================================================
// Handle "return" instructions.
void SpirvLowerGlobal::handleReturnInst() {
  for (Function &function : m_module->functions()) {
    // We only handle the "return" in entry point
    if (function.getLinkage() == GlobalValue::InternalLinkage)
      continue;
    for (BasicBlock &block : function) {
      Instruction *terminator = block.getTerminator();
      if (!terminator || terminator->getOpcode() != Instruction::Ret)
        continue;
      ReturnInst *returnInst = cast<ReturnInst>(terminator);
      assert(m_retBlock);
      BranchInst::Create(m_retBlock, &block);
      m_retInsts.insert(returnInst);
    }
  }
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
    for (User *user : users) {
      assert(isa<CallInst>(user) && "We should only have CallInst instructions here.");
      CallInst *callInst = cast<CallInst>(user);
      if (checkEmitCall) {
        if (mangledName.startswith(gSPIRVName::EmitVertex) || mangledName.startswith(gSPIRVName::EmitStreamVertex))
          m_emitCalls.insert(callInst);
      } else {
        assert(checkInterpCall);

        if (mangledName.startswith(gSPIRVName::InterpolateAtCentroid) ||
            mangledName.startswith(gSPIRVName::InterpolateAtSample) ||
            mangledName.startswith(gSPIRVName::InterpolateAtOffset) ||
            mangledName.startswith(gSPIRVName::InterpolateAtVertexAMD)) {
          // Translate interpolation functions to LLPC intrinsic calls
          auto loadSrc = callInst->getArgOperand(0);
          unsigned interpLoc = InterpLocUnknown;
          Value *auxInterpValue = nullptr;

          if (mangledName.startswith(gSPIRVName::InterpolateAtCentroid))
            interpLoc = InterpLocCentroid;
          else if (mangledName.startswith(gSPIRVName::InterpolateAtSample)) {
            interpLoc = InterpLocSample;
            auxInterpValue = callInst->getArgOperand(1); // Sample ID
          } else if (mangledName.startswith(gSPIRVName::InterpolateAtOffset)) {
            interpLoc = InterpLocCenter;
            auxInterpValue = callInst->getArgOperand(1); // Offset from pixel center
          } else {
            assert(mangledName.startswith(gSPIRVName::InterpolateAtVertexAMD));
            interpLoc = InterpLocCustom;
            auxInterpValue = callInst->getArgOperand(1); // Vertex no.
          }

          if (isa<GetElementPtrInst>(loadSrc)) {
            // The interpolant is an element of the input
            interpolateInputElement(interpLoc, auxInterpValue, *callInst);
          } else {
            // The interpolant is an input
            assert(isa<GlobalVariable>(loadSrc));

            auto input = cast<GlobalVariable>(loadSrc);
            auto inputTy = input->getValueType();

            MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
            assert(metaNode);
            auto inputMeta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

            m_builder->SetInsertPoint(callInst);
            auto loadValue = addCallInstForInOutImport(inputTy, SPIRAS_Input, inputMeta, nullptr, 0, nullptr, nullptr,
                                                       interpLoc, auxInterpValue, false);

            m_interpCalls.insert(callInst);
            callInst->replaceAllUsesWith(loadValue);
          }
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
// Handle a single "load" instruction directly loading a global.
//
// @param loadInst : Load instruction
// @param addrSpace : Address space
void SpirvLowerGlobal::handleLoadInstGlobal(LoadInst &loadInst, const unsigned addrSpace) {
  Value *loadSrc = loadInst.getOperand(0);
  auto inOut = cast<GlobalVariable>(loadSrc);
  auto inOutTy = inOut->getValueType();

  MDNode *metaNode = inOut->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto inOutMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

  m_builder->SetInsertPoint(&loadInst);

  Value *loadValue = UndefValue::get(inOutTy);

  // If the input/output is arrayed, the outermost index might be used for vertex indexing
  if (inOutTy->isArrayTy() && hasVertexIdx(*inOutMetaVal)) {
    auto elemTy = inOutTy->getArrayElementType();
    auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));

    const unsigned elemCount = inOutTy->getArrayNumElements();
    for (unsigned i = 0; i < elemCount; ++i) {
      Value *vertexIdx = m_builder->getInt32(i);
      auto elemValue = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, nullptr, 0, nullptr, vertexIdx,
                                                 InterpLocUnknown, nullptr, false);
      loadValue = m_builder->CreateInsertValue(loadValue, elemValue, {i});
    }
  } else {
    loadValue = addCallInstForInOutImport(inOutTy, addrSpace, inOutMetaVal, nullptr, 0, nullptr, nullptr,
                                          InterpLocUnknown, nullptr, false);
  }
  m_loadInsts.insert(&loadInst);
  loadInst.replaceAllUsesWith(loadValue);
}

// =====================================================================================================================
// Handle a single "load" instruction loading a global through a GEP instruction
//
// @param getElemPtr : Load source GEP instruction
// @param loadInst : Load instruction
// @param addrSpace : Address space
void SpirvLowerGlobal::handleLoadInstGEP(GetElementPtrInst *const getElemPtr, LoadInst &loadInst,
                                         const unsigned addrSpace) {
  std::vector<Value *> indexOperands;

  assert(cast<ConstantInt>(getElemPtr->getOperand(1))->isZero() && "Non-zero GEP first index\n");

  GlobalVariable *inOut = cast<GlobalVariable>(getElemPtr->getPointerOperand());
  assert(!isa<GetElementPtrInst>(getElemPtr->getPointerOperand()) &&
         "Chained GEPs should have been coalesced by SpirvLowerAccessChain.");

  m_builder->SetInsertPoint(&loadInst);

  for (auto &index : drop_begin(getElemPtr->indices()))
    indexOperands.push_back(m_builder->CreateZExtOrTrunc(index, m_builder->getInt32Ty()));

  Value *vertexIdx = nullptr;
  auto inOutTy = inOut->getValueType();

  MDNode *metaNode = inOut->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto inOutMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

  // If the input/output is arrayed, the outermost index might be used for vertex indexing
  if (inOutTy->isArrayTy() && hasVertexIdx(*inOutMetaVal)) {
    inOutTy = inOutTy->getArrayElementType();
    vertexIdx = indexOperands.front();
    indexOperands.erase(indexOperands.begin());
    inOutMetaVal = cast<Constant>(inOutMetaVal->getOperand(1));
  }

  Value *loadValue = loadInOutMember(inOutTy, addrSpace, indexOperands, 0, inOutMetaVal, nullptr, vertexIdx,
                                     InterpLocUnknown, nullptr, false);
  m_loadInsts.insert(&loadInst);
  loadInst.replaceAllUsesWith(loadValue);
}

// =====================================================================================================================
// Handle "load" instructions.
void SpirvLowerGlobal::handleLoadInst() {
  auto shouldHandle = [&](const unsigned addrSpace) {
    if (addrSpace != SPIRAS_Input && addrSpace != SPIRAS_Output)
      return false;
    // Skip if "load" instructions are not expected to be handled
    const bool isTcsInput = (m_shaderStage == ShaderStageTessControl && addrSpace == SPIRAS_Input);
    const bool isTcsOutput = (m_shaderStage == ShaderStageTessControl && addrSpace == SPIRAS_Output);
    const bool isTesInput = (m_shaderStage == ShaderStageTessEval && addrSpace == SPIRAS_Input);
    return isTcsInput || isTcsOutput || isTesInput;
  };

  for (GlobalVariable &global : m_module->globals()) {
    const unsigned addrSpace = global.getType()->getPointerAddressSpace();
    if (!shouldHandle(addrSpace))
      continue;
    for (User *user : global.users()) {
      if (LoadInst *loadInst = dyn_cast<LoadInst>(user))
        // The user is a load
        handleLoadInstGlobal(*loadInst, addrSpace);
      else if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(user)) {
        // The user is a GEP
        // We look for load instructions in the GEP users
        for (User *gepUser : gep->users()) {
          // We shouldn't have any chained GEPs here, they are coalesced by the LowerAccessChain pass.
          assert(!isa<GetElementPtrInst>(gepUser));
          if (LoadInst *loadInst = dyn_cast<LoadInst>(gepUser))
            handleLoadInstGEP(gep, *loadInst, addrSpace);
        }
      }
    }
  }
}

// =====================================================================================================================
// Handle a single "store" instruction directly storing a global.
//
// @param storeInst : Store instruction
void SpirvLowerGlobal::handleStoreInstGlobal(StoreInst &storeInst) {
  Value *storeDest = storeInst.getOperand(1);
  Value *storeValue = storeInst.getOperand(0);

  auto output = cast<GlobalVariable>(storeDest);
  auto outputy = output->getValueType();

  m_builder->SetInsertPoint(&storeInst);

  MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto outputMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

  // If the output is arrayed, the outermost dimension might for vertex indexing
  if (outputy->isArrayTy() && hasVertexIdx(*outputMetaVal)) {
    auto elemMeta = cast<Constant>(outputMetaVal->getOperand(1));

    const unsigned elemCount = outputy->getArrayNumElements();
    for (unsigned i = 0; i < elemCount; ++i) {
      auto elemValue = ExtractValueInst::Create(storeValue, {i}, "", &storeInst);
      Value *vertexOrPrimitiveIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
      addCallInstForOutputExport(elemValue, elemMeta, nullptr, 0, InvalidValue, 0, nullptr, vertexOrPrimitiveIdx,
                                 InvalidValue);
    }
  } else {
    addCallInstForOutputExport(storeValue, outputMetaVal, nullptr, 0, InvalidValue, 0, nullptr, nullptr, InvalidValue);
  }
  m_storeInsts.insert(&storeInst);
}

// =====================================================================================================================
// Handle a single "store" instruction storing a global through a GEP instruction
//
// @param getElemPtr : Store destination GEP instruction
// @param storeInst : Store instruction
void SpirvLowerGlobal::handleStoreInstGEP(GetElementPtrInst *const getElemPtr, StoreInst &storeInst) {
  Value *storeValue = storeInst.getOperand(0);

  std::vector<Value *> indexOperands;

  assert(cast<ConstantInt>(getElemPtr->getOperand(1))->isZero() && "Non-zero GEP first index\n");

  GlobalVariable *output = cast<GlobalVariable>(getElemPtr->getPointerOperand());
  assert(!isa<GetElementPtrInst>(getElemPtr->getPointerOperand()) &&
         "Chained GEPs should have been coalesced by SpirvLowerAccessChain.");

  m_builder->SetInsertPoint(&storeInst);

  for (auto &index : drop_begin(getElemPtr->indices()))
    indexOperands.push_back(m_builder->CreateZExtOrTrunc(index, m_builder->getInt32Ty()));

  Value *vertexOrPrimitiveIdx = nullptr;
  auto outputTy = output->getValueType();

  MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto outputMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

  // If the output is arrayed, the outermost index might be used for vertex indexing
  if (outputTy->isArrayTy() && hasVertexIdx(*outputMetaVal)) {
    outputTy = outputTy->getArrayElementType();
    vertexOrPrimitiveIdx = indexOperands.front();
    indexOperands.erase(indexOperands.begin());
    outputMetaVal = cast<Constant>(outputMetaVal->getOperand(1));
  }

  storeOutputMember(outputTy, storeValue, indexOperands, 0, outputMetaVal, nullptr, vertexOrPrimitiveIdx);
  m_storeInsts.insert(&storeInst);
}

// =====================================================================================================================
// Visits "store" instructions.
void SpirvLowerGlobal::handleStoreInst() {
  auto shouldHandle = [&](const unsigned addrSpace) {
    const bool isTcsOutput = (m_shaderStage == ShaderStageTessControl && addrSpace == SPIRAS_Output);
    return isTcsOutput;
  };

  for (GlobalVariable &global : m_module->globals()) {
    const unsigned addrSpace = global.getType()->getPointerAddressSpace();
    if (!shouldHandle(addrSpace))
      continue;
    for (User *user : global.users()) {
      if (StoreInst *storeInst = dyn_cast<StoreInst>(user))
        // The user is a store
        handleStoreInstGlobal(*storeInst);
      else if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(user)) {
        // The user is a GEP
        // We look for store instructions in the GEP users
        for (User *gepUser : gep->users()) {
          // We shouldn't have any chained GEPs here, they are coalesced by the LowerAccessChain pass.
          assert(!isa<GetElementPtrInst>(gepUser));
          if (StoreInst *storeInst = dyn_cast<StoreInst>(gepUser))
            handleStoreInstGEP(gep, *storeInst);
        }
      }
    }
  }
}

// =====================================================================================================================
// Maps the specified global variable to proxy variable.
//
// @param globalVar : Global variable to be mapped
void SpirvLowerGlobal::mapGlobalVariableToProxy(GlobalVariable *globalVar) {
  const auto &dataLayout = m_module->getDataLayout();
  Type *globalVarTy = globalVar->getValueType();

  m_builder->SetInsertPointPastAllocas(m_entryPoint);

  auto proxy = m_builder->CreateAlloca(globalVarTy, dataLayout.getAllocaAddrSpace(), nullptr,
                                       Twine(LlpcName::GlobalProxyPrefix) + globalVar->getName());

  if (globalVar->hasInitializer()) {
    auto initializer = globalVar->getInitializer();
    m_builder->CreateStore(initializer, proxy);
  }

  m_globalVarProxyMap[globalVar] = proxy;
}

// =====================================================================================================================
// Maps the specified input to proxy variable.
//
// @param input : Input to be mapped
void SpirvLowerGlobal::mapInputToProxy(GlobalVariable *input) {
  // NOTE: For tessellation shader, we do not map inputs to real proxy variables. Instead, we directly replace
  // "load" instructions with import calls in the lowering operation.
  if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval) {
    m_inputProxyMap[input] = nullptr;
    m_lowerInputInPlace = true;
    return;
  }

  m_builder->SetInsertPointPastAllocas(m_entryPoint);

  const auto &dataLayout = m_module->getDataLayout();
  Type *inputTy = input->getValueType();
  if (inputTy->isPointerTy())
    inputTy = m_builder->getInt64Ty();

  MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);

  auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
  auto proxy = m_builder->CreateAlloca(inputTy, dataLayout.getAllocaAddrSpace(), nullptr,
                                       Twine(LlpcName::InputProxyPrefix) + input->getName());

  // Import input to proxy variable
  auto inputValue = addCallInstForInOutImport(inputTy, SPIRAS_Input, meta, nullptr, 0, nullptr, nullptr,
                                              InterpLocUnknown, nullptr, false);
  m_builder->CreateStore(inputValue, proxy);

  m_inputProxyMap[input] = proxy;
}

// =====================================================================================================================
// Maps the specified output to proxy variable.
//
// @param output : Output to be mapped
void SpirvLowerGlobal::mapOutputToProxy(GlobalVariable *output) {
  m_builder->SetInsertPointPastAllocas(m_entryPoint);

  // NOTE: For tessellation control shader, we do not map outputs to real proxy variables. Instead, we directly
  // replace "store" instructions with export calls in the lowering operation.
  if (m_shaderStage == ShaderStageTessControl) {
    if (output->hasInitializer()) {
      auto initializer = output->getInitializer();
      m_builder->CreateStore(initializer, output);
    }
    m_outputProxyMap.emplace_back(output, nullptr);
    m_lowerOutputInPlace = true;
    return;
  }

  const auto &dataLayout = m_module->getDataLayout();
  Type *outputTy = output->getValueType();
  if (outputTy->isPointerTy())
    outputTy = m_builder->getInt64Ty();

  auto proxy = m_builder->CreateAlloca(outputTy, dataLayout.getAllocaAddrSpace(), nullptr,
                                       Twine(LlpcName::OutputProxyPrefix) + output->getName());

  if (output->hasInitializer()) {
    auto initializer = output->getInitializer();
    m_builder->CreateStore(initializer, proxy);
  }

  m_outputProxyMap.emplace_back(output, proxy);
}

// =====================================================================================================================
// Does lowering operations for SPIR-V global variables, replaces global variables with proxy variables.
void SpirvLowerGlobal::lowerGlobalVar() {
  if (m_globalVarProxyMap.empty()) {
    // Skip lowering if there is no global variable
    return;
  }

  // Replace global variable with proxy variable
  for (auto globalVarMap : m_globalVarProxyMap) {
    auto globalVar = cast<GlobalVariable>(globalVarMap.first);
    auto proxy = globalVarMap.second;
    globalVar->mutateType(proxy->getType()); // To clear address space for pointer to make replacement valid
    globalVar->replaceAllUsesWith(proxy);
    globalVar->dropAllReferences();
    globalVar->eraseFromParent();
  }
}

// =====================================================================================================================
// Does lowering operations for SPIR-V inputs, replaces inputs with proxy variables.
void SpirvLowerGlobal::lowerInput() {
  if (m_inputProxyMap.empty()) {
    // Skip lowering if there is no input
    return;
  }

  // NOTE: For tessellation shader, we invoke handling of "load"/"store" instructions and replace all those
  // instructions with import/export calls in-place.
  assert(m_shaderStage != ShaderStageTessControl && m_shaderStage != ShaderStageTessEval);

  // NOTE: For fragment shader, we have to handle interpolation functions first since input interpolants must be
  // lowered in-place.
  if (m_shaderStage == ShaderStageFragment) {
    // Invoke handling of interpolation calls
    handleCallInst(false, true);

    // Remove interpolation calls, they must have been replaced with LLPC intrinsics
    std::unordered_set<GetElementPtrInst *> getElemInsts;
    for (auto interpCall : m_interpCalls) {
      GetElementPtrInst *getElemPtr = dyn_cast<GetElementPtrInst>(interpCall->getArgOperand(0));
      if (getElemPtr)
        getElemInsts.insert(getElemPtr);

      assert(interpCall->use_empty());
      interpCall->dropAllReferences();
      interpCall->eraseFromParent();
    }

    for (auto getElemPtr : getElemInsts) {
      if (getElemPtr->use_empty()) {
        getElemPtr->dropAllReferences();
        getElemPtr->eraseFromParent();
      }
    }
  }

  for (auto inputMap : m_inputProxyMap) {
    auto input = cast<GlobalVariable>(inputMap.first);

    for (auto user = input->user_begin(), end = input->user_end(); user != end; ++user) {
      // NOTE: "Getelementptr" and "bitcast" will propagate the address space of pointer value (input variable)
      // to the element pointer value (destination). We have to clear the address space of this element pointer
      // value. The original pointer value has been lowered and therefore the address space is invalid now.
      Instruction *inst = dyn_cast<Instruction>(*user);
      if (inst) {
        Type *instTy = inst->getType();
        if (isa<PointerType>(instTy) && instTy->getPointerAddressSpace() == SPIRAS_Input) {
          assert(isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst));
          Type *newInstTy = PointerType::getWithSamePointeeType(cast<PointerType>(instTy), SPIRAS_Private);
          inst->mutateType(newInstTy);
        }
      }
    }

    auto proxy = inputMap.second;
    input->mutateType(proxy->getType()); // To clear address space for pointer to make replacement valid
    input->replaceAllUsesWith(proxy);
    input->eraseFromParent();
  }
}

// =====================================================================================================================
// Does lowering operations for SPIR-V outputs, replaces outputs with proxy variables.
void SpirvLowerGlobal::lowerOutput() {
#if VKI_RAY_TRACING
  // Note: indirect raytracing does not have output to lower and must return payload value
  if (m_context->isRayTracing())
    return;
#endif

  m_retBlock = BasicBlock::Create(*m_context, "", m_entryPoint);
  // Invoke handling of "return" instructions or "emit" calls
  if (m_shaderStage == ShaderStageGeometry)
    handleCallInst(true, false);
  handleReturnInst();

  auto retInst = ReturnInst::Create(*m_context, m_retBlock);

  for (auto retInst : m_retInsts) {
    retInst->dropAllReferences();
    retInst->eraseFromParent();
  }

  if (m_outputProxyMap.empty() && m_shaderStage != ShaderStageGeometry) {
    // Skip lowering if there is no output for non-geometry shader
    return;
  }

  // NOTE: For tessellation control shader, we invoke handling of "load"/"store" instructions and replace all those
  // instructions with import/export calls in-place.
  assert(m_shaderStage != ShaderStageTessControl);

  // Export output from the proxy variable prior to "return" instruction or "emit" calls
  for (auto outputMap : m_outputProxyMap) {
    auto output = cast<GlobalVariable>(outputMap.first);
    auto proxy = outputMap.second;
    auto proxyTy = proxy->getAllocatedType();

    MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);

    auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

    if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
        m_shaderStage == ShaderStageFragment) {
      m_builder->SetInsertPoint(retInst);
      Value *outputValue = m_builder->CreateLoad(proxyTy, proxy);
      addCallInstForOutputExport(outputValue, meta, nullptr, 0, 0, 0, nullptr, nullptr, InvalidValue);
    } else if (m_shaderStage == ShaderStageGeometry) {
      for (auto emitCall : m_emitCalls) {
        unsigned emitStreamId = 0;

        m_builder->SetInsertPoint(emitCall);

        auto mangledName = emitCall->getCalledFunction()->getName();
        if (mangledName.startswith(gSPIRVName::EmitStreamVertex))
          emitStreamId = cast<ConstantInt>(emitCall->getOperand(0))->getZExtValue();
        else
          assert(mangledName.startswith(gSPIRVName::EmitVertex));

        Value *outputValue = m_builder->CreateLoad(proxyTy, proxy);
        addCallInstForOutputExport(outputValue, meta, nullptr, 0, 0, 0, nullptr, nullptr, emitStreamId);
      }
    }
  }

  // Replace the Emit(Stream)Vertex calls with builder code.
  for (auto emitCall : m_emitCalls) {
    unsigned emitStreamId =
        emitCall->arg_size() != 0 ? cast<ConstantInt>(emitCall->getArgOperand(0))->getZExtValue() : 0;
    m_builder->SetInsertPoint(emitCall);
    m_builder->CreateEmitVertex(emitStreamId);
    emitCall->eraseFromParent();
  }

  for (auto outputMap : m_outputProxyMap) {
    auto output = cast<GlobalVariable>(outputMap.first);

    for (auto user = output->user_begin(), end = output->user_end(); user != end; ++user) {
      // NOTE: "Getelementptr" and "bitCast" will propagate the address space of pointer value (output variable)
      // to the element pointer value (destination). We have to clear the address space of this element pointer
      // value. The original pointer value has been lowered and therefore the address space is invalid now.
      Instruction *inst = dyn_cast<Instruction>(*user);
      if (inst) {
        Type *instTy = inst->getType();
        if (isa<PointerType>(instTy) && instTy->getPointerAddressSpace() == SPIRAS_Output) {
          assert(isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst));
          Type *newInstTy = PointerType::getWithSamePointeeType(cast<PointerType>(instTy), SPIRAS_Private);
          inst->mutateType(newInstTy);
        }
      }
    }

    auto proxy = outputMap.second;
    output->mutateType(proxy->getType()); // To clear address space for pointer to make replacement valid
    output->replaceAllUsesWith(proxy);
    output->eraseFromParent();
  }
}

// =====================================================================================================================
// Does inplace lowering operations for SPIR-V inputs/outputs, replaces "load" instructions with import calls and
// "store" instructions with export calls.
void SpirvLowerGlobal::lowerInOutInPlace() {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval);

  // Invoke handling of "load" and "store" instruction
  handleLoadInst();
  if (m_shaderStage == ShaderStageTessControl)
    handleStoreInst();

  DenseSet<GetElementPtrInst *> getElemInsts;

  // Remove unnecessary "load" instructions
  for (auto loadInst : m_loadInsts) {
    GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(loadInst->getPointerOperand());
    if (getElemPtr)
      getElemInsts.insert(getElemPtr);

    assert(loadInst->use_empty());
    loadInst->dropAllReferences();
    loadInst->eraseFromParent();
  }

  m_loadInsts.clear();

  // Remove unnecessary "store" instructions
  for (auto storeInst : m_storeInsts) {
    GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(storeInst->getPointerOperand());
    if (getElemPtr)
      getElemInsts.insert(getElemPtr);

    assert(storeInst->use_empty());
    storeInst->dropAllReferences();
    storeInst->eraseFromParent();
  }

  m_storeInsts.clear();

  // Remove unnecessary "getelementptr" instructions
  while (!getElemInsts.empty()) {
    GetElementPtrInst *const getElemPtr = *getElemInsts.begin();
    getElemInsts.erase(getElemPtr);

    // If the GEP still has any uses, skip processing it.
    if (!getElemPtr->use_empty())
      continue;

    // If the GEP is GEPing into another GEP, record that GEP as something we need to visit too.
    if (GetElementPtrInst *const otherGetElemInst = dyn_cast<GetElementPtrInst>(getElemPtr->getPointerOperand()))
      getElemInsts.insert(otherGetElemInst);

    getElemPtr->dropAllReferences();
    getElemPtr->eraseFromParent();
  }

  // Remove inputs if they are lowered in-place
  if (m_lowerInputInPlace) {
    for (auto inputMap : m_inputProxyMap) {
      auto input = cast<GlobalVariable>(inputMap.first);
      assert(input->use_empty());
      input->eraseFromParent();
    }
  }

  // Remove outputs if they are lowered in-place
  if (m_lowerOutputInPlace) {
    for (auto outputMap : m_outputProxyMap) {
      auto output = cast<GlobalVariable>(outputMap.first);
      assert(output->use_empty());
      output->eraseFromParent();
    }
  }
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

  Value *inOutValue = UndefValue::get(inOutTy);

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
          auto elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, nullptr, maxLocOffset, nullptr, vertexIdx,
                                                interpLoc, auxInterpValue, false);
          inOutValue = m_builder->CreateInsertValue(inOutValue, elem, {idx});
        }
      } else {
        // Array built-in without vertex indexing (ClipDistance/CullDistance).
        lgc::InOutInfo inOutInfo;
        inOutInfo.setArraySize(inOutTy->getArrayNumElements());
        // For Barycentric interplotation
        inOutInfo.setInterpLoc(interpLoc);
        if (addrSpace == SPIRAS_Input) {
          inOutValue = m_builder->CreateReadBuiltInInput(static_cast<lgc::BuiltInKind>(inOutMeta.Value), inOutInfo,
                                                         vertexIdx, nullptr);
        } else {
          inOutValue = m_builder->CreateReadBuiltInOutput(static_cast<lgc::BuiltInKind>(inOutMeta.Value), inOutInfo,
                                                          vertexIdx, nullptr);
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
          // clang-format on
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
      auto builtIn = static_cast<lgc::BuiltInKind>(inOutMeta.Value);
      elemIdx = elemIdx == m_builder->getInt32(InvalidValue) ? nullptr : elemIdx;
      vertexIdx = vertexIdx == m_builder->getInt32(InvalidValue) ? nullptr : vertexIdx;

      lgc::InOutInfo inOutInfo;
      inOutInfo.setArraySize(maxLocOffset);
      inOutInfo.setInterpLoc(interpLoc);

      if (builtIn == lgc::BuiltInBaryCoord || builtIn == lgc::BuiltInBaryCoordNoPerspKHR) {
        if (inOutInfo.getInterpLoc() == InterpLocUnknown)
          inOutInfo.setInterpLoc(inOutMeta.InterpLoc);
        return m_builder->CreateReadBaryCoord(builtIn, inOutInfo, auxInterpValue);
      }

      if (addrSpace == SPIRAS_Input)
        inOutValue = m_builder->CreateReadBuiltInInput(builtIn, inOutInfo, vertexIdx, elemIdx);
      else
        inOutValue = m_builder->CreateReadBuiltInOutput(builtIn, inOutInfo, vertexIdx, elemIdx);

      if ((builtIn == lgc::BuiltInSubgroupEqMask || builtIn == lgc::BuiltInSubgroupGeMask ||
           builtIn == lgc::BuiltInSubgroupGtMask || builtIn == lgc::BuiltInSubgroupLeMask ||
           builtIn == lgc::BuiltInSubgroupLtMask) &&
          inOutTy->isIntegerTy(64)) {
        // NOTE: Glslang has a bug. For gl_SubGroupXXXMaskARB, they are implemented as "uint64_t" while
        // for gl_subgroupXXXMask they are "uvec4". And the SPIR-V enumerants "BuiltInSubgroupXXXMaskKHR"
        // and "BuiltInSubgroupXXXMask" share the same numeric values.
        inOutValue = m_builder->CreateBitCast(inOutValue, FixedVectorType::get(inOutTy, 2));
        inOutValue = m_builder->CreateExtractElement(inOutValue, uint64_t(0));
      }
      if (inOutValue->getType()->isIntegerTy(1)) {
        // Convert i1 to i32.
        inOutValue = m_builder->CreateZExt(inOutValue, m_builder->getInt32Ty());
      }
    } else {
      unsigned idx = inOutMeta.Component;
      assert(inOutMeta.Component <= 3);
      if (inOutTy->getScalarSizeInBits() == 64) {
        assert(inOutMeta.Component % 2 == 0); // Must be even for 64-bit type
        idx = inOutMeta.Component / 2;
      }
      elemIdx = !elemIdx ? m_builder->getInt32(idx) : m_builder->CreateAdd(elemIdx, m_builder->getInt32(idx));

      lgc::InOutInfo inOutInfo;
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

  // NOTE: This special flag is just to check if we need output header of transform feedback info.
  static unsigned EnableXfb = false;

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
      m_builder->CreateWriteBuiltInOutput(outputValue, builtInId, outputInfo, vertexOrPrimitiveIdx, nullptr);

      if (outputMeta.IsXfb) {
        // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
        assert(xfbOffsetAdjust == 0 && xfbBufferAdjust == 0); // Unused for built-ins

        auto elemTy = outputTy->getArrayElementType();
        assert(elemTy->isFloatingPointTy() || elemTy->isIntegerTy()); // Must be scalar

        const uint64_t elemCount = outputTy->getArrayNumElements();
        const uint64_t byteSize = elemTy->getScalarSizeInBits() / 8;

        for (unsigned idx = 0; idx < elemCount; ++idx) {
          // Handle array elements recursively
          auto elem = m_builder->CreateExtractValue(outputValue, {idx}, "");

          auto xfbOffset = m_builder->getInt32(outputMeta.XfbOffset + outputMeta.XfbExtraOffset + byteSize * idx);
          m_builder->CreateWriteXfbOutput(elem,
                                          /*isBuiltIn=*/true, builtInId, outputMeta.XfbBuffer, outputMeta.XfbStride,
                                          xfbOffset, outputInfo);

          if (!static_cast<bool>(EnableXfb)) {
            LLPC_OUTS("\n===============================================================================\n");
            LLPC_OUTS("// LLPC transform feedback export info (" << getShaderStageName(m_shaderStage)
                                                                 << " shader)\n\n");

            EnableXfb = true;
          }

          auto builtInName = getNameMap(static_cast<BuiltIn>(builtInId)).map(static_cast<BuiltIn>(builtInId));
          LLPC_OUTS(*outputValue->getType() << " (builtin = " << builtInName.substr(strlen("BuiltIn")) << "), "
                                            << "xfbBuffer = " << outputMeta.XfbBuffer << ", "
                                            << "xfbStride = " << outputMeta.XfbStride << ", "
                                            << "xfbOffset = " << cast<ConstantInt>(xfbOffset)->getZExtValue() << "\n");
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

    if (outputMeta.IsBuiltIn) {
      auto builtInId = static_cast<lgc::BuiltInKind>(outputMeta.Value);
      outputInfo.setArraySize(maxLocOffset);
      if (outputMeta.IsXfb) {
        // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
        assert(xfbOffsetAdjust == 0 && xfbBufferAdjust == 0); // Unused for built-ins
        auto xfbOffset = m_builder->getInt32(outputMeta.XfbOffset + outputMeta.XfbExtraOffset);
        m_builder->CreateWriteXfbOutput(outputValue,
                                        /*isBuiltIn=*/true, builtInId, outputMeta.XfbBuffer, outputMeta.XfbStride,
                                        xfbOffset, outputInfo);

        if (!static_cast<bool>(EnableXfb)) {
          LLPC_OUTS("\n===============================================================================\n");
          LLPC_OUTS("// LLPC transform feedback export info (" << getShaderStageName(m_shaderStage) << " shader)\n\n");

          EnableXfb = true;
        }

        auto builtInName = getNameMap(static_cast<BuiltIn>(builtInId)).map(static_cast<BuiltIn>(builtInId));
        LLPC_OUTS(*outputValue->getType() << " (builtin = " << builtInName.substr(strlen("BuiltIn")) << "), "
                                          << "xfbBuffer = " << outputMeta.XfbBuffer << ", "
                                          << "xfbStride = " << outputMeta.XfbStride << ", "
                                          << "xfbOffset = " << cast<ConstantInt>(xfbOffset)->getZExtValue() << "\n");
      }

      m_builder->CreateWriteBuiltInOutput(outputValue, builtInId, outputInfo, vertexOrPrimitiveIdx, elemIdx);
      return;
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

    if (outputMeta.IsXfb) {
      // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
      assert(xfbOffsetAdjust != InvalidValue);
      Value *xfbOffset = m_builder->getInt32(outputMeta.XfbOffset + outputMeta.XfbExtraOffset + xfbOffsetAdjust);
      m_builder->CreateWriteXfbOutput(outputValue,
                                      /*isBuiltIn=*/false, location + cast<ConstantInt>(locOffset)->getZExtValue(),
                                      outputMeta.XfbBuffer + xfbBufferAdjust, outputMeta.XfbStride, xfbOffset,
                                      outputInfo);

      if (!static_cast<bool>(EnableXfb)) {
        LLPC_OUTS("\n===============================================================================\n");
        LLPC_OUTS("// LLPC transform feedback export info (" << getShaderStageName(m_shaderStage) << " shader)\n\n");

        EnableXfb = true;
      }

      LLPC_OUTS(*outputValue->getType() << " (loc = " << location + cast<ConstantInt>(locOffset)->getZExtValue()
                                        << "), "
                                        << "xfbBuffer = " << outputMeta.XfbBuffer + xfbBufferAdjust << ", "
                                        << "xfbStride = " << outputMeta.XfbStride << ", "
                                        << "xfbOffset = " << cast<ConstantInt>(xfbOffset)->getZExtValue() << "\n");
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
  Value *inOutValue = UndefValue::get(inOutTy);
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
// @param addrSpace : Address space
// @param indexOperands : Index operands to process (if empty, all indices have been processed)
// @param maxLocOffset : Max+1 location offset if variable index has been encountered
// @param inOutMetaVal : Metadata of this input/output member
// @param locOffset : Relative location offset of this input/output member
// @param vertexIdx : Input/output array outermost index used for vertex indexing
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader): - Sample ID for
// "InterpLocSample" - Offset from the center of the pixel for "InterpLocCenter" - Vertex no. (0 ~ 2) for
// "InterpLocCustom"
Value *SpirvLowerGlobal::loadInOutMember(Type *inOutTy, unsigned addrSpace, ArrayRef<Value *> indexOperands,
                                         unsigned maxLocOffset, Constant *inOutMetaVal, Value *locOffset,
                                         Value *vertexIdx, unsigned interpLoc, Value *auxInterpValue,
                                         bool isPerVertexDimension) {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageFragment);

  if (indexOperands.empty()) {
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
      assert(indexOperands.size() == 1);
      auto elemIdx = indexOperands.front();
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
      auxInterpValue = indexOperands.front();
      elemLocOffset = m_builder->getInt32(0);
    } else {
      // elemLocOffset = locOffset + stride * elemIdx
      unsigned stride = cast<ConstantInt>(inOutMetaVal->getOperand(0))->getZExtValue();
      auto elemIdx = indexOperands.front();
      elemLocOffset = m_builder->CreateMul(m_builder->getInt32(stride), elemIdx);
      elemLocOffset = m_builder->CreateAdd(locOffset, elemLocOffset);

      // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
      // so it knows how many locations to mark as used by this access.
      if (maxLocOffset == 0 && !isa<ConstantInt>(elemIdx)) {
        maxLocOffset = cast<ConstantInt>(locOffset)->getZExtValue() + stride * inOutTy->getArrayNumElements();
      }
    }

    return loadInOutMember(elemTy, addrSpace, indexOperands.drop_front(), maxLocOffset, elemMeta, elemLocOffset,
                           vertexIdx, interpLoc, auxInterpValue, inOutMeta.PerVertexDimension);
  }

  if (inOutTy->isStructTy()) {
    // Struct type
    unsigned memberIdx = cast<ConstantInt>(indexOperands.front())->getZExtValue();

    auto memberTy = inOutTy->getStructElementType(memberIdx);
    auto memberMeta = cast<Constant>(inOutMetaVal->getOperand(memberIdx));

    return loadInOutMember(memberTy, addrSpace, indexOperands.drop_front(), maxLocOffset, memberMeta, locOffset,
                           vertexIdx, interpLoc, auxInterpValue, isPerVertexDimension);
  }

  if (inOutTy->isVectorTy()) {
    // Vector type
    assert(indexOperands.size() == 1);

    Type *loadTy = cast<VectorType>(inOutTy)->getElementType();
    Value *compIdx = indexOperands.front();

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
// @param storeValue : Value stored to output member
// @param indexOperands : Index operands to process (if empty, all indices have been processed)
// @param maxLocOffset : Max+1 location offset if variable index has been encountered
// @param outputMetaVal : Metadata of this output member
// @param locOffset : Relative location offset of this output member
// @param vertexOrPrimitiveIdx : Input array outermost index used for vertex indexing
void SpirvLowerGlobal::storeOutputMember(Type *outputTy, Value *storeValue, ArrayRef<Value *> indexOperands,
                                         unsigned maxLocOffset, Constant *outputMetaVal, Value *locOffset,
                                         Value *vertexOrPrimitiveIdx) {
  assert(m_shaderStage == ShaderStageTessControl);

  if (indexOperands.empty()) {
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
      assert(indexOperands.size() == 1);

      auto elemIdx = indexOperands.front();
      return addCallInstForOutputExport(storeValue, elemMeta, nullptr, outputTy->getArrayNumElements(), InvalidValue, 0,
                                        elemIdx, vertexOrPrimitiveIdx, InvalidValue);
    }

    // NOTE: If the relative location offset is not specified, initialize it.
    if (!locOffset)
      locOffset = m_builder->getInt32(0);

    // elemLocOffset = locOffset + stride * elemIdx
    unsigned stride = cast<ConstantInt>(outputMetaVal->getOperand(0))->getZExtValue();
    auto elemIdx = indexOperands.front();
    Value *elemLocOffset = m_builder->CreateMul(m_builder->getInt32(stride), elemIdx);
    elemLocOffset = m_builder->CreateAdd(locOffset, elemLocOffset);

    // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
    // so it knows how many locations to mark as used by this access.
    if (maxLocOffset == 0 && !isa<ConstantInt>(elemIdx)) {
      maxLocOffset = cast<ConstantInt>(locOffset)->getZExtValue() + stride * outputTy->getArrayNumElements();
    }

    return storeOutputMember(elemTy, storeValue, indexOperands.drop_front(), maxLocOffset, elemMeta, elemLocOffset,
                             vertexOrPrimitiveIdx);
  }

  if (outputTy->isStructTy()) {
    // Structure type
    unsigned memberIdx = cast<ConstantInt>(indexOperands.front())->getZExtValue();

    auto memberTy = outputTy->getStructElementType(memberIdx);
    auto memberMeta = cast<Constant>(outputMetaVal->getOperand(memberIdx));

    return storeOutputMember(memberTy, storeValue, indexOperands.drop_front(), maxLocOffset, memberMeta, locOffset,
                             vertexOrPrimitiveIdx);
  }

  if (outputTy->isVectorTy()) {
    // Vector type
    assert(indexOperands.size() == 1);
    auto compIdx = indexOperands.front();

    return addCallInstForOutputExport(storeValue, outputMetaVal, locOffset, maxLocOffset, InvalidValue, 0, compIdx,
                                      vertexOrPrimitiveIdx, InvalidValue);
  }

  llvm_unreachable("Should never be called!");
}

// =====================================================================================================================
// Lowers buffer blocks.
void SpirvLowerGlobal::lowerBufferBlock() {
  SmallVector<GlobalVariable *, 8> globalsToRemove;

  // Represent the users of the global variables, expect a bitCast, a GEP or a select used by GEPs
  struct ReplaceInstsInfo {
    BitCastInst *bitCastInst;                         // The user is a bitCast
    SelectInst *selectInst;                           // The user is a select
    SmallVector<GetElementPtrInst *> getElemPtrInsts; // The user is a GEP. If the user is a select, we store its users.
  };

  // Skip the globals that are handled with previous global.
  SmallSet<GlobalVariable *, 4> skipGlobals;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a block.
    if (global.getAddressSpace() != SPIRAS_Uniform)
      continue;
    if (skipGlobals.count(&global) > 0) {
      globalsToRemove.push_back(&global);
      continue;
    }

    MDNode *const resMetaNode = global.getMetadata(gSPIRVMD::Resource);
    assert(resMetaNode);

    const unsigned descSet = mdconst::dyn_extract<ConstantInt>(resMetaNode->getOperand(0))->getZExtValue();
    const unsigned binding = mdconst::dyn_extract<ConstantInt>(resMetaNode->getOperand(1))->getZExtValue();

    SmallVector<Constant *, 8> constantUsers;

    for (User *const user : global.users()) {
      if (Constant *const constVal = dyn_cast<Constant>(user))
        constantUsers.push_back(constVal);
    }

    for (Constant *const constVal : constantUsers)
      replaceConstWithInsts(m_context, constVal);

    // Record of all the functions that our global is used within.
    SmallSet<Function *, 4> funcsUsedIn;

    for (User *const user : global.users()) {
      if (Instruction *const inst = dyn_cast<Instruction>(user))
        funcsUsedIn.insert(inst->getFunction());
    }

    // Collect the instructions to be replaced per-global
    SmallVector<ReplaceInstsInfo> instructionsToReplace;
    for (Function *const func : funcsUsedIn) {
      // Check if our block is an array of blocks.
      if (global.getValueType()->isArrayTy()) {
        Type *const elementType = global.getValueType()->getArrayElementType();
        Type *const blockType = elementType->getPointerTo(global.getAddressSpace());

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
            } else if (auto *bitCast = dyn_cast<BitCastInst>(inst)) {
              // We need to modify the bitcast if we did not find a GEP.
              assert(bitCast->getOperand(0) == &global);
              replaceInstsInfo.bitCastInst = bitCast;
            } else {
              // The users of the select must be a GEP.
              SelectInst *selectInst = cast<SelectInst>(inst);
              assert(selectInst->getTrueValue() == &global || selectInst->getFalseValue() == &global);
              replaceInstsInfo.selectInst = selectInst;
              for (User *selectUser : selectInst->users()) {
                if (auto *userInst = dyn_cast<Instruction>(selectUser)) {
                  assert(userInst->getFunction() == func);
                  if (auto *getElemPtr = dyn_cast<GetElementPtrInst>(userInst))
                    replaceInstsInfo.getElemPtrInsts.push_back(getElemPtr);
                }
              }
            }
            instructionsToReplace.push_back(replaceInstsInfo);
          }
        }

        for (const auto &replaceInstsInfo : instructionsToReplace) {
          if (replaceInstsInfo.bitCastInst) {
            // All bitcasts recorded here are for GEPs that indexed by 0, 0 into the arrayed resource, and LLVM
            // has been clever enough to realise that doing a GEP of 0, 0 is actually a no-op (because the pointer
            // does not change!), and has removed it.
            m_builder->SetInsertPoint(replaceInstsInfo.bitCastInst);
            unsigned bufferFlags = global.isConstant() ? 0 : lgc::Builder::BufferFlagWritten;
            Value *const bufferDesc = m_builder->CreateLoadBufferDesc(descSet, binding, m_builder->getInt32(0),
                                                                      bufferFlags, m_builder->getInt8Ty());

            // If the global variable is a constant, the data it points to is invariant.
            if (global.isConstant())
              m_builder->CreateInvariantStart(bufferDesc);

            replaceInstsInfo.bitCastInst->replaceUsesOfWith(&global, m_builder->CreateBitCast(bufferDesc, blockType));
          } else {
            assert(!replaceInstsInfo.getElemPtrInsts.empty());

            for (GetElementPtrInst *getElemPtr : replaceInstsInfo.getElemPtrInsts) {
              // The second index is the block offset, so we need at least two indices!
              assert(getElemPtr->getNumIndices() >= 2);
              SmallVector<Value *, 8> indices;

              for (Value *const index : getElemPtr->indices())
                indices.push_back(index);

              // The first index should always be zero.
              assert(isa<ConstantInt>(indices[0]) && cast<ConstantInt>(indices[0])->getZExtValue() == 0);

              // The second index is the block index.
              Value *const blockIndex = indices[1];

              bool isNonUniform = false;

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
                isNonUniform = callee->getName().startswith(gSPIRVName::NonUniform);
                break;
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
                  if (callee && callee->getName().startswith(gSPIRVName::NonUniform)) {
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
              Value *bitCasts[2] = {nullptr};
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
                descSets[1] = mdconst::dyn_extract<ConstantInt>(resMetaNode1->getOperand(0))->getZExtValue();
                bindings[1] = mdconst::dyn_extract<ConstantInt>(resMetaNode1->getOperand(1))->getZExtValue();

                if (!nextGlobalIdx) {
                  std::swap(descSets[0], descSets[1]);
                  std::swap(bindings[0], bindings[1]);
                }
                skipGlobals.insert(globals[nextGlobalIdx]);
              }
              for (unsigned idx = 0; idx < descCount; ++idx) {
                bufferDescs[idx] = m_builder->CreateLoadBufferDesc(descSets[idx], bindings[idx], blockIndex,
                                                                   bufferFlags, m_builder->getInt8Ty());
                // If the global variable is a constant, the data it points to is invariant.
                if (global.isConstant())
                  m_builder->CreateInvariantStart(bufferDescs[idx]);

                bitCasts[idx] = m_builder->CreateBitCast(bufferDescs[idx], blockType);
              }

              Value *newSelect = nullptr;
              if (select)
                newSelect = m_builder->CreateSelect(select->getCondition(), bitCasts[0], bitCasts[1]);

              Value *base = newSelect ? newSelect : bitCasts[0];
              // We need to remove the block index from the original GEP indices so that we can use them.
              indices[1] = indices[0];

              ArrayRef<Value *> newIndices(indices);
              newIndices = newIndices.drop_front(1);

              Value *newGetElemPtr = nullptr;

              if (getElemPtr->isInBounds())
                newGetElemPtr = m_builder->CreateInBoundsGEP(elementType, base, newIndices);
              else
                newGetElemPtr = m_builder->CreateGEP(elementType, base, newIndices);

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
        Value *const bufferDesc = m_builder->CreateLoadBufferDesc(descSet, binding, m_builder->getInt32(0), bufferFlags,
                                                                  m_builder->getInt8Ty());

        // If the global variable is a constant, the data it points to is invariant.
        if (global.isConstant())
          m_builder->CreateInvariantStart(bufferDesc);

        Value *const bitCast = m_builder->CreateBitCast(bufferDesc, global.getType());

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

        for (Instruction *const use : usesToReplace)
          use->replaceUsesOfWith(&global, bitCast);
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
      const unsigned aliased = mdconst::dyn_extract<ConstantInt>(meta->getOperand(0))->getZExtValue();
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
// Lowers push constants.
void SpirvLowerGlobal::lowerPushConsts() {
  SmallVector<GlobalVariable *, 1> globalsToRemove;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a push constant.
    if (global.getAddressSpace() != SPIRAS_Constant || !global.hasMetadata(gSPIRVMD::PushConst))
      continue;

    // There should only be a single push constant variable!
    assert(globalsToRemove.empty());

    SmallVector<Constant *, 8> constantUsers;

    for (User *const user : global.users()) {
      if (Constant *const constVal = dyn_cast<Constant>(user))
        constantUsers.push_back(constVal);
    }

    for (Constant *const constVal : constantUsers)
      replaceConstWithInsts(m_context, constVal);

    // Record of all the functions that our global is used within.
    SmallSet<Function *, 4> funcsUsedIn;

    for (User *const user : global.users()) {
      if (Instruction *const inst = dyn_cast<Instruction>(user))
        funcsUsedIn.insert(inst->getFunction());
    }

    for (Function *const func : funcsUsedIn) {
      m_builder->SetInsertPointPastAllocas(func);

      MDNode *metaNode = global.getMetadata(gSPIRVMD::PushConst);
      auto pushConstSize = mdconst::dyn_extract<ConstantInt>(metaNode->getOperand(0))->getZExtValue();
      Type *const pushConstantsType = ArrayType::get(m_builder->getInt8Ty(), pushConstSize);
      Value *pushConstants =
          m_builder->CreateLoadPushConstantsPtr(pushConstantsType->getPointerTo(m_builder->getAddrSpaceConst()));

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
// Removes the created return block if it has a single predecessor. This is to avoid
// scheduling future heavy-weight cleanup passes if we can trivially simplify the CFG here.
void SpirvLowerGlobal::cleanupReturnBlock() {
  if (!m_retBlock)
    return;

  if (MergeBlockIntoPredecessor(m_retBlock))
    m_retBlock = nullptr;
}

// =====================================================================================================================
// Interpolates an element of the input.
//
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader): - Sample ID for
// "InterpLocSample" - Offset from the center of the pixel for "InterpLocCenter" - Vertex no. (0 ~ 2) for
// "InterpLocCustom"
// @param callInst : "Call" instruction
void SpirvLowerGlobal::interpolateInputElement(unsigned interpLoc, Value *auxInterpValue, CallInst &callInst) {
  GetElementPtrInst *getElemPtr = cast<GetElementPtrInst>(callInst.getArgOperand(0));

  assert(cast<ConstantInt>(getElemPtr->getOperand(1))->isZero() && "Non-zero GEP first index\n");

  m_builder->SetInsertPoint(&callInst);

  std::vector<Value *> indexOperands;
  for (auto &index : getElemPtr->indices())
    indexOperands.push_back(m_builder->CreateZExtOrTrunc(index, m_builder->getInt32Ty()));

  auto input = cast<GlobalVariable>(getElemPtr->getPointerOperand());
  auto inputTy = input->getValueType();

  MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto inputMeta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

  if (getElemPtr->hasAllConstantIndices()) {
    auto loadValue = loadInOutMember(inputTy, SPIRAS_Input, makeArrayRef(indexOperands).drop_front(), 0, inputMeta,
                                     nullptr, nullptr, interpLoc, auxInterpValue, false);

    m_interpCalls.insert(&callInst);
    callInst.replaceAllUsesWith(loadValue);
  } else {
    // Interpolant an element via dynamic index by extending interpolant to each element
    //
    // Regardless of where we do the interpolation, the alloca for the temporary must be inserted in the function entry
    // block for efficient code generation, so we don't use the builder for it.
    auto interpPtr = new AllocaInst(inputTy, m_module->getDataLayout().getAllocaAddrSpace(), Twine(),
                                    &*(m_entryPoint->begin()->getFirstInsertionPt()));
    // Load all possibly accessed values
    auto loadValue = loadDynamicIndexedMembers(inputTy, SPIRAS_Input, makeArrayRef(indexOperands).drop_front(),
                                               inputMeta, nullptr, interpLoc, auxInterpValue, false);

    m_builder->CreateStore(loadValue, interpPtr);

    auto interpElemPtr = m_builder->CreateGEP(inputTy, interpPtr, indexOperands);
    auto interpElemTy = GetElementPtrInst::getIndexedType(inputTy, indexOperands);

    // Only get the value that the original getElemPtr points to
    auto interpElemValue = m_builder->CreateLoad(interpElemTy, interpElemPtr);
    callInst.replaceAllUsesWith(interpElemValue);

    if (callInst.user_empty()) {
      callInst.dropAllReferences();
      callInst.eraseFromParent();
    }
  }
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering operations for globals.
INITIALIZE_PASS(LegacySpirvLowerGlobal, DEBUG_TYPE, "Lower SPIR-V globals (global variables, inputs, and outputs)",
                false, false)
