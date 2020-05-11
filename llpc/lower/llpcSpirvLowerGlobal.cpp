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

// =====================================================================================================================
// Initializes static members.
char SpirvLowerGlobal::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for globals
ModulePass *createSpirvLowerGlobal() {
  return new SpirvLowerGlobal();
}

// =====================================================================================================================
SpirvLowerGlobal::SpirvLowerGlobal()
    : SpirvLower(ID), m_retBlock(nullptr), m_lowerInputInPlace(false), m_lowerOutputInPlace(false) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerGlobal::runOnModule(Module &module) {
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

  // NOTE: Global variable, inlcude general global variable, input and output is a special constant variable, so if
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

  cleanupReturnBlock();

  return true;
}

// =====================================================================================================================
// Visits "return" instruction.
//
// @param retInst : "Ret" instruction
void SpirvLowerGlobal::visitReturnInst(ReturnInst &retInst) {
  // Skip if "return" instructions are not expected to be handled.
  if (!static_cast<bool>(m_instVisitFlags.checkReturn))
    return;

  // We only handle the "return" in entry point
  if (retInst.getParent()->getParent()->getLinkage() == GlobalValue::InternalLinkage)
    return;

  assert(m_retBlock); // Must have been created
  BranchInst::Create(m_retBlock, retInst.getParent());
  m_retInsts.insert(&retInst);
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void SpirvLowerGlobal::visitCallInst(CallInst &callInst) {
  // Skip if "emit" and interpolaton calls are not expected to be handled
  if (!static_cast<bool>(m_instVisitFlags.checkEmitCall) && !static_cast<bool>(m_instVisitFlags.checkInterpCall))
    return;

  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  auto mangledName = callee->getName();

  if (m_instVisitFlags.checkEmitCall) {
    if (mangledName.startswith(gSPIRVName::EmitVertex) || mangledName.startswith(gSPIRVName::EmitStreamVertex))
      m_emitCalls.insert(&callInst);
  } else {
    assert(m_instVisitFlags.checkInterpCall);

    if (mangledName.startswith(gSPIRVName::InterpolateAtCentroid) ||
        mangledName.startswith(gSPIRVName::InterpolateAtSample) ||
        mangledName.startswith(gSPIRVName::InterpolateAtOffset) ||
        mangledName.startswith(gSPIRVName::InterpolateAtVertexAMD)) {
      // Translate interpolation functions to LLPC intrinsic calls
      auto loadSrc = callInst.getArgOperand(0);
      unsigned interpLoc = InterpLocUnknown;
      Value *auxInterpValue = nullptr;

      if (mangledName.startswith(gSPIRVName::InterpolateAtCentroid))
        interpLoc = InterpLocCentroid;
      else if (mangledName.startswith(gSPIRVName::InterpolateAtSample)) {
        interpLoc = InterpLocSample;
        auxInterpValue = callInst.getArgOperand(1); // Sample ID
      } else if (mangledName.startswith(gSPIRVName::InterpolateAtOffset)) {
        interpLoc = InterpLocCenter;
        auxInterpValue = callInst.getArgOperand(1); // Offset from pixel center
      } else {
        assert(mangledName.startswith(gSPIRVName::InterpolateAtVertexAMD));
        interpLoc = InterpLocCustom;
        auxInterpValue = callInst.getArgOperand(1); // Vertex no.
      }

      if (isa<GetElementPtrInst>(loadSrc)) {
        // The interpolant is an element of the input
        interpolateInputElement(interpLoc, auxInterpValue, callInst);
      } else {
        // The interpolant is an input
        assert(isa<GlobalVariable>(loadSrc));

        auto input = cast<GlobalVariable>(loadSrc);
        auto inputTy = input->getType()->getContainedType(0);

        MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
        assert(metaNode);
        auto inputMeta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

        auto loadValue = addCallInstForInOutImport(inputTy, SPIRAS_Input, inputMeta, nullptr, 0, nullptr, nullptr,
                                                   interpLoc, auxInterpValue, &callInst);

        m_interpCalls.insert(&callInst);
        callInst.replaceAllUsesWith(loadValue);
      }
    }
  }
}

// =====================================================================================================================
// Visits "load" instruction.
//
// @param loadInst : "Load" instruction
void SpirvLowerGlobal::visitLoadInst(LoadInst &loadInst) {
  Value *loadSrc = loadInst.getOperand(0);
  const unsigned addrSpace = loadSrc->getType()->getPointerAddressSpace();

  if (addrSpace != SPIRAS_Input && addrSpace != SPIRAS_Output)
    return;

  // Skip if "load" instructions are not expected to be handled
  const bool isTcsInput = (m_shaderStage == ShaderStageTessControl && addrSpace == SPIRAS_Input);
  const bool isTcsOutput = (m_shaderStage == ShaderStageTessControl && addrSpace == SPIRAS_Output);
  const bool isTesInput = (m_shaderStage == ShaderStageTessEval && addrSpace == SPIRAS_Input);

  if (!static_cast<bool>(m_instVisitFlags.checkLoad) || (!isTcsInput && !isTcsOutput && !isTesInput))
    return;

  if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(loadSrc)) {
    std::vector<Value *> indexOperands;

    GlobalVariable *inOut = nullptr;

    // Loop back through the get element pointer to find the global variable.
    for (GetElementPtrInst *currGetElemPtr = getElemPtr; currGetElemPtr;
         currGetElemPtr = dyn_cast<GetElementPtrInst>(currGetElemPtr->getPointerOperand())) {
      assert(currGetElemPtr);

      // If we have previous index operands, we need to remove the first operand (a zero index into the pointer)
      // when concatenating two GEP indices together.
      if (!indexOperands.empty())
        indexOperands.erase(indexOperands.begin());

      SmallVector<Value *, 8> indices;

      for (Value *const index : currGetElemPtr->indices())
        indices.push_back(toInt32Value(index, &loadInst));

      indexOperands.insert(indexOperands.begin(), indices.begin(), indices.end());

      inOut = dyn_cast<GlobalVariable>(currGetElemPtr->getPointerOperand());
    }

    // The root of the GEP should always be the global variable.
    assert(inOut);

    unsigned operandIdx = 0;

    auto inOutTy = inOut->getType()->getContainedType(0);

    MDNode *metaNode = inOut->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);
    auto inOutMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

    Value *vertexIdx = nullptr;

    // If the input/output is arrayed, the outermost index might be used for vertex indexing
    if (inOutTy->isArrayTy()) {
      bool isVertexIdx = false;

      assert(inOutMetaVal->getNumOperands() == 4);
      ShaderInOutMetadata inOutMeta = {};

      inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaVal->getOperand(2))->getZExtValue();
      inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaVal->getOperand(3))->getZExtValue();

      if (inOutMeta.IsBuiltIn) {
        unsigned builtInId = inOutMeta.Value;
        isVertexIdx = (builtInId == spv::BuiltInPerVertex || // GLSL style per-vertex data
                       builtInId == spv::BuiltInPosition ||  // HLSL style per-vertex data
                       builtInId == spv::BuiltInPointSize || builtInId == spv::BuiltInClipDistance ||
                       builtInId == spv::BuiltInCullDistance);
      } else
        isVertexIdx = !static_cast<bool>(inOutMeta.PerPatch);

      if (isVertexIdx) {
        inOutTy = inOutTy->getArrayElementType();
        vertexIdx = indexOperands[1];
        ++operandIdx;

        inOutMetaVal = cast<Constant>(inOutMetaVal->getOperand(1));
      }
    }

    auto loadValue = loadInOutMember(inOutTy, addrSpace, indexOperands, operandIdx, 0, inOutMetaVal, nullptr, vertexIdx,
                                     InterpLocUnknown, nullptr, &loadInst);

    m_loadInsts.insert(&loadInst);
    loadInst.replaceAllUsesWith(loadValue);
  } else {
    assert(isa<GlobalVariable>(loadSrc));

    auto inOut = cast<GlobalVariable>(loadSrc);
    auto inOutTy = inOut->getType()->getContainedType(0);

    MDNode *metaNode = inOut->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);
    auto inOutMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

    Value *loadValue = UndefValue::get(inOutTy);
    bool hasVertexIdx = false;

    if (inOutTy->isArrayTy()) {
      // Arrayed input/output
      assert(inOutMetaVal->getNumOperands() == 4);
      ShaderInOutMetadata inOutMeta = {};
      inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaVal->getOperand(2))->getZExtValue();
      inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaVal->getOperand(3))->getZExtValue();

      // If the input/output is arrayed, the outermost dimension might for vertex indexing
      if (inOutMeta.IsBuiltIn) {
        unsigned builtInId = inOutMeta.Value;
        hasVertexIdx = (builtInId == spv::BuiltInPerVertex || // GLSL style per-vertex data
                        builtInId == spv::BuiltInPosition ||  // HLSL style per-vertex data
                        builtInId == spv::BuiltInPointSize || builtInId == spv::BuiltInClipDistance ||
                        builtInId == spv::BuiltInCullDistance);
      } else
        hasVertexIdx = !static_cast<bool>(inOutMeta.PerPatch);
    }

    if (hasVertexIdx) {
      assert(inOutTy->isArrayTy());

      auto elemTy = inOutTy->getArrayElementType();
      auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));

      const unsigned elemCount = inOutTy->getArrayNumElements();
      for (unsigned i = 0; i < elemCount; ++i) {
        Value *vertexIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        auto elemValue = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, nullptr, 0, nullptr, vertexIdx,
                                                   InterpLocUnknown, nullptr, &loadInst);
        loadValue = InsertValueInst::Create(loadValue, elemValue, {i}, "", &loadInst);
      }
    } else {
      loadValue = addCallInstForInOutImport(inOutTy, addrSpace, inOutMetaVal, nullptr, 0, nullptr, nullptr,
                                            InterpLocUnknown, nullptr, &loadInst);
    }

    m_loadInsts.insert(&loadInst);
    loadInst.replaceAllUsesWith(loadValue);
  }
}

// =====================================================================================================================
// Visits "store" instruction.
//
// @param storeInst : "Store" instruction
void SpirvLowerGlobal::visitStoreInst(StoreInst &storeInst) {
  Value *storeValue = storeInst.getOperand(0);
  Value *storeDest = storeInst.getOperand(1);

  const unsigned addrSpace = storeDest->getType()->getPointerAddressSpace();

  if (addrSpace != SPIRAS_Input && addrSpace != SPIRAS_Output)
    return;

  // Skip if "store" instructions are not expected to be handled
  const bool isTcsOutput = (m_shaderStage == ShaderStageTessControl && addrSpace == SPIRAS_Output);
  if (!static_cast<bool>(m_instVisitFlags.checkStore) || !isTcsOutput)
    return;

  if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(storeDest)) {
    std::vector<Value *> indexOperands;

    GlobalVariable *output = nullptr;

    // Loop back through the get element pointer to find the global variable.
    for (GetElementPtrInst *currGetElemPtr = getElemPtr; currGetElemPtr;
         currGetElemPtr = dyn_cast<GetElementPtrInst>(currGetElemPtr->getPointerOperand())) {
      assert(currGetElemPtr);

      // If we have previous index operands, we need to remove the first operand (a zero index into the pointer)
      // when concatenating two GEP indices together.
      if (!indexOperands.empty())
        indexOperands.erase(indexOperands.begin());

      SmallVector<Value *, 8> indices;

      for (Value *const index : currGetElemPtr->indices())
        indices.push_back(toInt32Value(index, &storeInst));

      indexOperands.insert(indexOperands.begin(), indices.begin(), indices.end());

      output = dyn_cast<GlobalVariable>(currGetElemPtr->getPointerOperand());
    }

    unsigned operandIdx = 0;

    auto outputTy = output->getType()->getContainedType(0);

    MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);
    auto outputMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

    Value *vertexIdx = nullptr;

    // If the output is arrayed, the outermost index might be used for vertex indexing
    if (outputTy->isArrayTy()) {
      bool isVertexIdx = false;

      assert(outputMetaVal->getNumOperands() == 4);
      ShaderInOutMetadata outputMeta = {};
      outputMeta.U64All[0] = cast<ConstantInt>(outputMetaVal->getOperand(2))->getZExtValue();
      outputMeta.U64All[1] = cast<ConstantInt>(outputMetaVal->getOperand(3))->getZExtValue();

      if (outputMeta.IsBuiltIn) {
        unsigned builtInId = outputMeta.Value;
        isVertexIdx = (builtInId == spv::BuiltInPerVertex || // GLSL style per-vertex data
                       builtInId == spv::BuiltInPosition ||  // HLSL style per-vertex data
                       builtInId == spv::BuiltInPointSize || builtInId == spv::BuiltInClipDistance ||
                       builtInId == spv::BuiltInCullDistance);
      } else
        isVertexIdx = !static_cast<bool>(outputMeta.PerPatch);

      if (isVertexIdx) {
        outputTy = outputTy->getArrayElementType();
        vertexIdx = indexOperands[1];
        ++operandIdx;

        outputMetaVal = cast<Constant>(outputMetaVal->getOperand(1));
      }
    }

    storeOutputMember(outputTy, storeValue, indexOperands, operandIdx, 0, outputMetaVal, nullptr, vertexIdx,
                      &storeInst);

    m_storeInsts.insert(&storeInst);
  } else {
    assert(isa<GlobalVariable>(storeDest));

    auto output = cast<GlobalVariable>(storeDest);
    auto outputy = output->getType()->getContainedType(0);

    MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);
    auto outputMetaVal = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

    bool hasVertexIdx = false;

    // If the input/output is arrayed, the outermost dimension might for vertex indexing
    if (outputy->isArrayTy()) {
      assert(outputMetaVal->getNumOperands() == 4);
      ShaderInOutMetadata outputMeta = {};
      outputMeta.U64All[0] = cast<ConstantInt>(outputMetaVal->getOperand(2))->getZExtValue();
      outputMeta.U64All[1] = cast<ConstantInt>(outputMetaVal->getOperand(3))->getZExtValue();

      if (outputMeta.IsBuiltIn) {
        unsigned builtInId = outputMeta.Value;
        hasVertexIdx = (builtInId == spv::BuiltInPerVertex || // GLSL style per-vertex data
                        builtInId == spv::BuiltInPosition ||  // HLSL style per-vertex data
                        builtInId == spv::BuiltInPointSize || builtInId == spv::BuiltInClipDistance ||
                        builtInId == spv::BuiltInCullDistance);
      } else
        hasVertexIdx = !static_cast<bool>(outputMeta.PerPatch);
    }

    if (hasVertexIdx) {
      assert(outputy->isArrayTy());
      auto elemMeta = cast<Constant>(outputMetaVal->getOperand(1));

      const unsigned elemCount = outputy->getArrayNumElements();
      for (unsigned i = 0; i < elemCount; ++i) {
        auto elemValue = ExtractValueInst::Create(storeValue, {i}, "", &storeInst);
        Value *vertexIdx = ConstantInt::get(Type::getInt32Ty(*m_context), i);
        addCallInstForOutputExport(elemValue, elemMeta, nullptr, 0, InvalidValue, 0, nullptr, vertexIdx, InvalidValue,
                                   &storeInst);
      }
    } else {
      addCallInstForOutputExport(storeValue, outputMetaVal, nullptr, 0, InvalidValue, 0, nullptr, nullptr, InvalidValue,
                                 &storeInst);
    }

    m_storeInsts.insert(&storeInst);
  }
}

// =====================================================================================================================
// Maps the specified global variable to proxy variable.
//
// @param globalVar : Global variable to be mapped
void SpirvLowerGlobal::mapGlobalVariableToProxy(GlobalVariable *globalVar) {
  const auto &dataLayout = m_module->getDataLayout();
  Type *globalVarTy = globalVar->getType()->getContainedType(0);
  Twine prefix = LlpcName::GlobalProxyPrefix;
  auto insertPos = m_entryPoint->begin()->getFirstInsertionPt();

  auto proxy = new AllocaInst(globalVarTy, dataLayout.getAllocaAddrSpace(), prefix + globalVar->getName(), &*insertPos);

  if (globalVar->hasInitializer()) {
    auto initializer = globalVar->getInitializer();
    new StoreInst(initializer, proxy, &*insertPos);
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

  const auto &dataLayout = m_module->getDataLayout();
  Type *inputTy = input->getType()->getContainedType(0);
  Twine prefix = LlpcName::InputProxyPrefix;
  auto insertPos = m_entryPoint->begin()->getFirstInsertionPt();

  MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);

  auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
  auto proxy = new AllocaInst(inputTy, dataLayout.getAllocaAddrSpace(), prefix + input->getName(), &*insertPos);

  // Import input to proxy variable
  auto inputValue = addCallInstForInOutImport(inputTy, SPIRAS_Input, meta, nullptr, 0, nullptr, nullptr,
                                              InterpLocUnknown, nullptr, &*insertPos);
  new StoreInst(inputValue, proxy, &*insertPos);

  m_inputProxyMap[input] = proxy;
}

// =====================================================================================================================
// Maps the specified output to proxy variable.
//
// @param output : Output to be mapped
void SpirvLowerGlobal::mapOutputToProxy(GlobalVariable *output) {
  auto insertPos = m_entryPoint->begin()->getFirstInsertionPt();

  // NOTE: For tessellation control shader, we do not map outputs to real proxy variables. Instead, we directly
  // replace "store" instructions with export calls in the lowering operation.
  if (m_shaderStage == ShaderStageTessControl) {
    if (output->hasInitializer()) {
      auto initializer = output->getInitializer();
      new StoreInst(initializer, output, &*insertPos);
    }
    m_outputProxyMap.push_back(std::pair<Value *, Value *>(output, nullptr));
    m_lowerOutputInPlace = true;
    return;
  }

  const auto &dataLayout = m_module->getDataLayout();
  Type *outputTy = output->getType()->getContainedType(0);
  Twine prefix = LlpcName::OutputProxyPrefix;

  auto proxy = new AllocaInst(outputTy, dataLayout.getAllocaAddrSpace(), prefix + output->getName(), &*insertPos);

  if (output->hasInitializer()) {
    auto initializer = output->getInitializer();
    new StoreInst(initializer, proxy, &*insertPos);
  }

  m_outputProxyMap.push_back(std::pair<Value *, Value *>(output, proxy));
}

// =====================================================================================================================
// Does lowering opertions for SPIR-V global variables, replaces global variables with proxy variables.
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
// Does lowering opertions for SPIR-V inputs, replaces inputs with proxy variables.
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
    m_instVisitFlags.u32All = 0;
    m_instVisitFlags.checkInterpCall = true;
    visit(m_module);

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
      // NOTE: "Getelementptr" and "bitcast" will propogate the address space of pointer value (input variable)
      // to the element pointer value (destination). We have to clear the address space of this element pointer
      // value. The original pointer value has been lowered and therefore the address space is invalid now.
      Instruction *inst = dyn_cast<Instruction>(*user);
      if (inst) {
        Type *instTy = inst->getType();
        if (isa<PointerType>(instTy) && instTy->getPointerAddressSpace() == SPIRAS_Input) {
          assert(isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst));
          Type *newInstTy = PointerType::get(instTy->getContainedType(0), SPIRAS_Private);
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
// Does lowering opertions for SPIR-V outputs, replaces outputs with proxy variables.
void SpirvLowerGlobal::lowerOutput() {
  m_retBlock = BasicBlock::Create(*m_context, "", m_entryPoint);

  // Invoke handling of "return" instructions or "emit" calls
  m_instVisitFlags.u32All = 0;
  if (m_shaderStage == ShaderStageGeometry) {
    m_instVisitFlags.checkEmitCall = true;
    m_instVisitFlags.checkReturn = true;
  } else
    m_instVisitFlags.checkReturn = true;
  visit(m_module);

  auto retInst = ReturnInst::Create(*m_context, m_retBlock);

  for (auto retInst : m_retInsts) {
    retInst->dropAllReferences();
    retInst->eraseFromParent();
  }

  if (m_outputProxyMap.empty()) {
    // Skip lowering if there is no output
    return;
  }

  // NOTE: For tessellation control shader, we invoke handling of "load"/"store" instructions and replace all those
  // instructions with import/export calls in-place.
  assert(m_shaderStage != ShaderStageTessControl);

  // Export output from the proxy variable prior to "return" instruction or "emit" calls
  for (auto outputMap : m_outputProxyMap) {
    auto output = cast<GlobalVariable>(outputMap.first);
    auto proxy = outputMap.second;
    auto proxyTy = proxy->getType()->getPointerElementType();

    MDNode *metaNode = output->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);

    auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

    if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
        m_shaderStage == ShaderStageFragment) {
      Value *outputValue = new LoadInst(proxyTy, proxy, "", retInst);
      addCallInstForOutputExport(outputValue, meta, nullptr, 0, 0, 0, nullptr, nullptr, InvalidValue, retInst);
    } else if (m_shaderStage == ShaderStageGeometry) {
      for (auto emitCall : m_emitCalls) {
        unsigned emitStreamId = 0;

        auto mangledName = emitCall->getCalledFunction()->getName();
        if (mangledName.startswith(gSPIRVName::EmitStreamVertex))
          emitStreamId = cast<ConstantInt>(emitCall->getOperand(0))->getZExtValue();
        else
          assert(mangledName.startswith(gSPIRVName::EmitVertex));

        Value *outputValue = new LoadInst(proxyTy, proxy, "", emitCall);
        addCallInstForOutputExport(outputValue, meta, nullptr, 0, 0, 0, nullptr, nullptr, emitStreamId, emitCall);
      }
    }
  }

  // Replace the Emit(Stream)Vertex calls with builder code.
  for (auto emitCall : m_emitCalls) {
    unsigned emitStreamId =
        emitCall->getNumArgOperands() != 0 ? cast<ConstantInt>(emitCall->getArgOperand(0))->getZExtValue() : 0;
    m_builder->SetInsertPoint(emitCall);
    m_builder->CreateEmitVertex(emitStreamId);
    emitCall->eraseFromParent();
  }

  for (auto outputMap : m_outputProxyMap) {
    auto output = cast<GlobalVariable>(outputMap.first);

    for (auto user = output->user_begin(), end = output->user_end(); user != end; ++user) {
      // NOTE: "Getelementptr" and "bitCast" will propogate the address space of pointer value (output variable)
      // to the element pointer value (destination). We have to clear the address space of this element pointer
      // value. The original pointer value has been lowered and therefore the address space is invalid now.
      Instruction *inst = dyn_cast<Instruction>(*user);
      if (inst) {
        Type *instTy = inst->getType();
        if (isa<PointerType>(instTy) && instTy->getPointerAddressSpace() == SPIRAS_Output) {
          assert(isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst));
          Type *newInstTy = PointerType::get(instTy->getContainedType(0), SPIRAS_Private);
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
// Does inplace lowering opertions for SPIR-V inputs/outputs, replaces "load" instructions with import calls and
// "store" instructions with export calls.
void SpirvLowerGlobal::lowerInOutInPlace() {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval);

  // Invoke handling of "load" and "store" instruction
  m_instVisitFlags.u32All = 0;
  m_instVisitFlags.checkLoad = true;
  if (m_shaderStage == ShaderStageTessControl)
    m_instVisitFlags.checkStore = true;
  visit(m_module);

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
// @param insertPos : Where to insert this call
Value *SpirvLowerGlobal::addCallInstForInOutImport(Type *inOutTy, unsigned addrSpace, Constant *inOutMetaVal,
                                                   Value *locOffset, unsigned maxLocOffset, Value *elemIdx,
                                                   Value *vertexIdx, unsigned interpLoc, Value *auxInterpValue,
                                                   Instruction *insertPos) {
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
          vertexIdx = ConstantInt::get(Type::getInt32Ty(*m_context), idx);
          auto elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, nullptr, maxLocOffset, nullptr, vertexIdx,
                                                interpLoc, auxInterpValue, insertPos);
          inOutValue = InsertValueInst::Create(inOutValue, elem, {idx}, "", insertPos);
        }
      } else {
        // Array built-in without vertex indexing (ClipDistance/CullDistance).
        lgc::InOutInfo inOutInfo;
        inOutInfo.setArraySize(inOutTy->getArrayNumElements());
        m_builder->SetInsertPoint(insertPos);
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
          vertexIdx = ConstantInt::get(Type::getInt32Ty(*m_context), idx);
          auto elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, locOffset, maxLocOffset, nullptr,
                                                vertexIdx, InterpLocUnknown, nullptr, insertPos);
          inOutValue = InsertValueInst::Create(inOutValue, elem, {idx}, "", insertPos);
        }
      } else {
        // NOTE: If the relative location offset is not specified, initialize it to 0.
        if (!locOffset)
          locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);

        for (unsigned idx = 0; idx < elemCount; ++idx) {
          // Handle array elements recursively

          // elemLocOffset = locOffset + stride * idx
          Value *elemLocOffset =
              BinaryOperator::CreateMul(ConstantInt::get(Type::getInt32Ty(*m_context), stride),
                                        ConstantInt::get(Type::getInt32Ty(*m_context), idx), "", insertPos);
          elemLocOffset = BinaryOperator::CreateAdd(locOffset, elemLocOffset, "", insertPos);

          auto elem = addCallInstForInOutImport(elemTy, addrSpace, elemMeta, elemLocOffset, maxLocOffset, elemIdx,
                                                vertexIdx, InterpLocUnknown, nullptr, insertPos);
          inOutValue = InsertValueInst::Create(inOutValue, elem, {idx}, "", insertPos);
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
                                              vertexIdx, InterpLocUnknown, nullptr, insertPos);
      inOutValue = InsertValueInst::Create(inOutValue, member, {memberIdx}, "", insertPos);
    }
  } else {
    Constant *inOutMetaValConst = cast<Constant>(inOutMetaVal);
    inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaValConst->getOperand(0))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaValConst->getOperand(1))->getZExtValue();

    assert(inOutMeta.IsLoc || inOutMeta.IsBuiltIn);

    m_builder->SetInsertPoint(insertPos);
    if (inOutMeta.IsBuiltIn) {
      auto builtIn = static_cast<lgc::BuiltInKind>(inOutMeta.Value);
      elemIdx = elemIdx == m_builder->getInt32(InvalidValue) ? nullptr : elemIdx;
      vertexIdx = vertexIdx == m_builder->getInt32(InvalidValue) ? nullptr : vertexIdx;

      lgc::InOutInfo inOutInfo;
      inOutInfo.setArraySize(maxLocOffset);
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
        inOutValue = m_builder->CreateBitCast(inOutValue, VectorType::get(inOutTy, 2));
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

      if (addrSpace == SPIRAS_Input) {
        if (m_shaderStage == ShaderStageFragment) {
          if (interpLoc != InterpLocUnknown) {
            // Use auxiliary value of interpolation (calcuated I/J or vertex no.) for
            // interpolant inputs of fragment shader.
            vertexIdx = auxInterpValue;
            inOutInfo.setHasInterpAux();
          } else
            interpLoc = inOutMeta.InterpLoc;
          inOutInfo.setInterpLoc(interpLoc);
          inOutInfo.setInterpMode(inOutMeta.InterpMode);
        }
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
// @param vertexIdx : Output array outermost index used for vertex indexing, valid for tessellation control shader
// @param emitStreamId : ID of emitted vertex stream, valid for geometry shader (0xFFFFFFFF for others)
// @param insertPos : Where to insert this call
void SpirvLowerGlobal::addCallInstForOutputExport(Value *outputValue, Constant *outputMetaVal, Value *locOffset,
                                                  unsigned maxLocOffset, unsigned xfbOffsetAdjust,
                                                  unsigned xfbBufferAdjust, Value *elemIdx, Value *vertexIdx,
                                                  unsigned emitStreamId, Instruction *insertPos) {
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
      m_builder->SetInsertPoint(insertPos);
      m_builder->CreateWriteBuiltInOutput(outputValue, builtInId, outputInfo, vertexIdx, nullptr);

      if (outputMeta.IsXfb) {
        // NOTE: For transform feedback outputs, additional stream-out export call will be generated.
        assert(xfbOffsetAdjust == 0 && xfbBufferAdjust == 0); // Unused for built-ins

        auto elemTy = outputTy->getArrayElementType();
        assert(elemTy->isFloatingPointTy() || elemTy->isIntegerTy()); // Must be scalar

        const uint64_t elemCount = outputTy->getArrayNumElements();
        const uint64_t byteSize = elemTy->getScalarSizeInBits() / 8;

        for (unsigned idx = 0; idx < elemCount; ++idx) {
          // Handle array elements recursively
          auto elem = ExtractValueInst::Create(outputValue, {idx}, "", insertPos);

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
        Value *elem = ExtractValueInst::Create(outputValue, {idx}, "", insertPos);

        Value *elemLocOffset = nullptr;
        ConstantInt *locOffsetConst = dyn_cast<ConstantInt>(locOffset);

        if (locOffsetConst) {
          unsigned locOffset = locOffsetConst->getZExtValue();
          elemLocOffset = ConstantInt::get(Type::getInt32Ty(*m_context), locOffset + stride * idx);
        } else {
          // elemLocOffset = locOffset + stride * idx
          elemLocOffset = BinaryOperator::CreateMul(ConstantInt::get(Type::getInt32Ty(*m_context), stride),
                                                    ConstantInt::get(Type::getInt32Ty(*m_context), idx), "", insertPos);
          elemLocOffset = BinaryOperator::CreateAdd(locOffset, elemLocOffset, "", insertPos);
        }

        // NOTE: GLSL spec says: an array of size N of blocks is captured by N consecutive buffers,
        // with all members of block array-element E captured by buffer B, where B equals the declared or
        // inherited xfb_buffer plus E.
        bool blockArray = outputMeta.IsBlockArray;
        addCallInstForOutputExport(elem, elemMeta, elemLocOffset, maxLocOffset,
                                   xfbOffsetAdjust + (blockArray ? 0 : outputMeta.XfbArrayStride * idx),
                                   xfbBufferAdjust + (blockArray ? outputMeta.XfbArrayStride * idx : 0), nullptr,
                                   vertexIdx, emitStreamId, insertPos);
      }
    }
  } else if (outputTy->isStructTy()) {
    // Structure type
    assert(!elemIdx);

    const uint64_t memberCount = outputTy->getStructNumElements();
    for (unsigned memberIdx = 0; memberIdx < memberCount; ++memberIdx) {
      // Handle structure member recursively
      auto memberMeta = cast<Constant>(outputMetaVal->getOperand(memberIdx));
      Value *member = ExtractValueInst::Create(outputValue, {memberIdx}, "", insertPos);
      addCallInstForOutputExport(member, memberMeta, locOffset, maxLocOffset, xfbOffsetAdjust, xfbBufferAdjust, nullptr,
                                 vertexIdx, emitStreamId, insertPos);
    }
  } else {
    // Normal scalar or vector type
    m_builder->SetInsertPoint(insertPos);
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

      m_builder->CreateWriteBuiltInOutput(outputValue, builtInId, outputInfo, vertexIdx, elemIdx);
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

    m_builder->CreateWriteGenericOutput(outputValue, location, locOffset, elemIdx, maxLocOffset, outputInfo, vertexIdx);
  }
}

// =====================================================================================================================
// Inserts instructions to load value from input/ouput member.
//
// @param inOutTy : Type of this input/output member
// @param addrSpace : Address space
// @param indexOperands : Index operands
// @param operandIdx : Index of the index operand in processing
// @param maxLocOffset : Max+1 location offset if variable index has been encountered
// @param inOutMetaVal : Metadata of this input/output member
// @param locOffset : Relative location offset of this input/output member
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param interpLoc : Interpolation location, valid for fragment shader (use "InterpLocUnknown" as don't-care value)
// @param auxInterpValue : Auxiliary value of interpolation (valid for fragment shader): - Sample ID for
// "InterpLocSample" - Offset from the center of the pixel for "InterpLocCenter" - Vertex no. (0 ~ 2) for
// "InterpLocCustom"
// @param insertPos : Where to insert calculation instructions
Value *SpirvLowerGlobal::loadInOutMember(Type *inOutTy, unsigned addrSpace, const std::vector<Value *> &indexOperands,
                                         unsigned operandIdx, unsigned maxLocOffset, Constant *inOutMetaVal,
                                         Value *locOffset, Value *vertexIdx, unsigned interpLoc, Value *auxInterpValue,
                                         Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageFragment);

  if (operandIdx < indexOperands.size() - 1) {
    if (inOutTy->isArrayTy()) {
      // Array type
      assert(inOutMetaVal->getNumOperands() == 4);
      ShaderInOutMetadata inOutMeta = {};

      inOutMeta.U64All[0] = cast<ConstantInt>(inOutMetaVal->getOperand(2))->getZExtValue();
      inOutMeta.U64All[1] = cast<ConstantInt>(inOutMetaVal->getOperand(3))->getZExtValue();

      auto elemMeta = cast<Constant>(inOutMetaVal->getOperand(1));
      auto elemTy = inOutTy->getArrayElementType();

      if (inOutMeta.IsBuiltIn) {
        assert(operandIdx + 1 == indexOperands.size() - 1);
        auto elemIdx = indexOperands[operandIdx + 1];
        return addCallInstForInOutImport(elemTy, addrSpace, elemMeta, locOffset, inOutTy->getArrayNumElements(),
                                         elemIdx, vertexIdx, interpLoc, auxInterpValue, insertPos);
      } else {
        // NOTE: If the relative location offset is not specified, initialize it to 0.
        if (!locOffset)
          locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);

        // elemLocOffset = locOffset + stride * elemIdx
        unsigned stride = cast<ConstantInt>(inOutMetaVal->getOperand(0))->getZExtValue();
        auto elemIdx = indexOperands[operandIdx + 1];
        Value *elemLocOffset =
            BinaryOperator::CreateMul(ConstantInt::get(Type::getInt32Ty(*m_context), stride), elemIdx, "", insertPos);
        elemLocOffset = BinaryOperator::CreateAdd(locOffset, elemLocOffset, "", insertPos);

        // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
        // so it knows how many locations to mark as used by this access.
        if (maxLocOffset == 0 && !isa<ConstantInt>(elemIdx)) {
          maxLocOffset = cast<ConstantInt>(locOffset)->getZExtValue() + stride * inOutTy->getArrayNumElements();
        }

        return loadInOutMember(elemTy, addrSpace, indexOperands, operandIdx + 1, maxLocOffset, elemMeta, elemLocOffset,
                               vertexIdx, interpLoc, auxInterpValue, insertPos);
      }
    } else if (inOutTy->isStructTy()) {
      // Structure type
      unsigned memberIdx = cast<ConstantInt>(indexOperands[operandIdx + 1])->getZExtValue();

      auto memberTy = inOutTy->getStructElementType(memberIdx);
      auto memberMeta = cast<Constant>(inOutMetaVal->getOperand(memberIdx));

      return loadInOutMember(memberTy, addrSpace, indexOperands, operandIdx + 1, maxLocOffset, memberMeta, locOffset,
                             vertexIdx, interpLoc, auxInterpValue, insertPos);
    } else if (inOutTy->isVectorTy()) {
      // Vector type
      auto compTy = cast<VectorType>(inOutTy)->getElementType();

      assert(operandIdx + 1 == indexOperands.size() - 1);
      auto compIdx = indexOperands[operandIdx + 1];

      return addCallInstForInOutImport(compTy, addrSpace, inOutMetaVal, locOffset, maxLocOffset, compIdx, vertexIdx,
                                       interpLoc, auxInterpValue, insertPos);
    }
  } else {
    // Last index operand
    assert(operandIdx == indexOperands.size() - 1);
    return addCallInstForInOutImport(inOutTy, addrSpace, inOutMetaVal, locOffset, maxLocOffset, nullptr, vertexIdx,
                                     interpLoc, auxInterpValue, insertPos);
  }

  llvm_unreachable("Should never be called!");
  return nullptr;
}

// =====================================================================================================================
// Inserts instructions to store value to ouput member.
//
// @param outputTy : Type of this output member
// @param storeValue : Value stored to output member
// @param indexOperands : Index operands
// @param operandIdx : Index of the index operand in processing
// @param maxLocOffset : Max+1 location offset if variable index has been encountered
// @param outputMetaVal : Metadata of this output member
// @param locOffset : Relative location offset of this output member
// @param vertexIdx : Input array outermost index used for vertex indexing
// @param insertPos : Where to insert store instructions
void SpirvLowerGlobal::storeOutputMember(Type *outputTy, Value *storeValue, const std::vector<Value *> &indexOperands,
                                         unsigned operandIdx, unsigned maxLocOffset, Constant *outputMetaVal,
                                         Value *locOffset, Value *vertexIdx, Instruction *insertPos) {
  assert(m_shaderStage == ShaderStageTessControl);

  if (operandIdx < indexOperands.size() - 1) {
    if (outputTy->isArrayTy()) {
      assert(outputMetaVal->getNumOperands() == 4);
      ShaderInOutMetadata outputMeta = {};

      outputMeta.U64All[0] = cast<ConstantInt>(outputMetaVal->getOperand(2))->getZExtValue();
      outputMeta.U64All[1] = cast<ConstantInt>(outputMetaVal->getOperand(3))->getZExtValue();

      auto elemMeta = cast<Constant>(outputMetaVal->getOperand(1));
      auto elemTy = outputTy->getArrayElementType();

      if (outputMeta.IsBuiltIn) {
        assert(!locOffset);
        assert(operandIdx + 1 == indexOperands.size() - 1);

        auto elemIdx = indexOperands[operandIdx + 1];
        return addCallInstForOutputExport(storeValue, elemMeta, nullptr, outputTy->getArrayNumElements(), InvalidValue,
                                          0, elemIdx, vertexIdx, InvalidValue, insertPos);
      } else {
        // NOTE: If the relative location offset is not specified, initialize it.
        if (!locOffset)
          locOffset = ConstantInt::get(Type::getInt32Ty(*m_context), 0);

        // elemLocOffset = locOffset + stride * elemIdx
        unsigned stride = cast<ConstantInt>(outputMetaVal->getOperand(0))->getZExtValue();
        auto elemIdx = indexOperands[operandIdx + 1];
        Value *elemLocOffset =
            BinaryOperator::CreateMul(ConstantInt::get(Type::getInt32Ty(*m_context), stride), elemIdx, "", insertPos);
        elemLocOffset = BinaryOperator::CreateAdd(locOffset, elemLocOffset, "", insertPos);

        // Mark the end+1 possible location offset if the index is variable. The Builder call needs it
        // so it knows how many locations to mark as used by this access.
        if (maxLocOffset == 0 && !isa<ConstantInt>(elemIdx)) {
          maxLocOffset = cast<ConstantInt>(locOffset)->getZExtValue() + stride * outputTy->getArrayNumElements();
        }

        return storeOutputMember(elemTy, storeValue, indexOperands, operandIdx + 1, maxLocOffset, elemMeta,
                                 elemLocOffset, vertexIdx, insertPos);
      }
    } else if (outputTy->isStructTy()) {
      // Structure type
      unsigned memberIdx = cast<ConstantInt>(indexOperands[operandIdx + 1])->getZExtValue();

      auto memberTy = outputTy->getStructElementType(memberIdx);
      auto memberMeta = cast<Constant>(outputMetaVal->getOperand(memberIdx));

      return storeOutputMember(memberTy, storeValue, indexOperands, operandIdx + 1, maxLocOffset, memberMeta, locOffset,
                               vertexIdx, insertPos);
    } else if (outputTy->isVectorTy()) {
      // Vector type
      assert(operandIdx + 1 == indexOperands.size() - 1);
      auto compIdx = indexOperands[operandIdx + 1];

      return addCallInstForOutputExport(storeValue, outputMetaVal, locOffset, maxLocOffset, InvalidValue, 0, compIdx,
                                        vertexIdx, InvalidValue, insertPos);
    }
  } else {
    // Last index operand
    assert(operandIdx == indexOperands.size() - 1);

    return addCallInstForOutputExport(storeValue, outputMetaVal, locOffset, maxLocOffset, InvalidValue, 0, nullptr,
                                      vertexIdx, InvalidValue, insertPos);
  }

  llvm_unreachable("Should never be called!");
}

// =====================================================================================================================
// Lowers buffer blocks.
void SpirvLowerGlobal::lowerBufferBlock() {
  SmallVector<GlobalVariable *, 8> globalsToRemove;

  for (GlobalVariable &global : m_module->globals()) {
    // Skip anything that is not a block.
    if (global.getAddressSpace() != SPIRAS_Uniform)
      continue;

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

    for (Function *const func : funcsUsedIn) {
      // Check if our block is an array of blocks.
      if (global.getType()->getPointerElementType()->isArrayTy()) {
        Type *const elementType = global.getType()->getPointerElementType()->getArrayElementType();
        Type *const blockType = elementType->getPointerTo(global.getAddressSpace());

        SmallVector<BitCastInst *, 8> bitCastsToModify;
        SmallVector<GetElementPtrInst *, 8> getElemPtrsToReplace;

        // We need to run over the users of the global, find the GEPs, and add a load for each.
        for (User *const user : global.users()) {
          // Skip over non-instructions.
          if (!isa<Instruction>(user))
            continue;

          GetElementPtrInst *getElemPtr = dyn_cast<GetElementPtrInst>(user);

          if (!getElemPtr) {
            // Skip all bitcasts, looking for a GEP.
            for (BitCastInst *bitCast = dyn_cast<BitCastInst>(user); bitCast;
                 bitCast = dyn_cast<BitCastInst>(bitCast->getOperand(0)))
              getElemPtr = dyn_cast<GetElementPtrInst>(bitCast);

            // If even after we've stripped away all the bitcasts we did not find a GEP, we need to modify
            // the bitcast instead.
            if (!getElemPtr) {
              BitCastInst *const bitCast = dyn_cast<BitCastInst>(user);
              assert(bitCast);

              bitCastsToModify.push_back(bitCast);
              continue;
            }
          }

          // Skip instructions in other functions.
          if (getElemPtr->getFunction() != func)
            continue;

          getElemPtrsToReplace.push_back(getElemPtr);
        }

        // All bitcasts recorded here are for GEPs that indexed by 0, 0 into the arrayed resource, and LLVM
        // has been clever enough to realise that doing a GEP of 0, 0 is actually a no-op (because the pointer
        // does not change!), and has removed it.
        for (BitCastInst *const bitCast : bitCastsToModify) {
          m_builder->SetInsertPoint(bitCast);

          Value *const bufferDesc =
              m_builder->CreateLoadBufferDesc(descSet, binding, m_builder->getInt32(0),
                                              /*isNonUniform=*/false, !global.isConstant(), m_builder->getInt8Ty());

          // If the global variable is a constant, the data it points to is invariant.
          if (global.isConstant())
            m_builder->CreateInvariantStart(bufferDesc);

          bitCast->replaceUsesOfWith(&global, m_builder->CreateBitCast(bufferDesc, blockType));
        }

        for (GetElementPtrInst *const getElemPtr : getElemPtrsToReplace) {
          // The second index is the block offset, so we need at least two indices!
          assert(getElemPtr->getNumIndices() >= 2);

          m_builder->SetInsertPoint(getElemPtr);

          SmallVector<Value *, 8> indices;

          for (Value *const index : getElemPtr->indices())
            indices.push_back(index);

          // The first index should always be zero.
          assert(isa<ConstantInt>(indices[0]) && cast<ConstantInt>(indices[0])->getZExtValue() == 0);

          // The second index is the block index.
          Value *const blockIndex = indices[1];

          bool isNonUniform = false;

          // Run the users of the block index to check for any nonuniform calls.
          for (User *const user : blockIndex->users()) {
            CallInst *const call = dyn_cast<CallInst>(user);

            // If the user is not a call, bail.
            if (!call)
              continue;

            // If the call is our non uniform decoration, record we are non uniform.
            if (call->getCalledFunction()->getName().startswith(gSPIRVName::NonUniform)) {
              isNonUniform = true;
              break;
            }
          }

          Value *const bufferDesc = m_builder->CreateLoadBufferDesc(descSet, binding, blockIndex, isNonUniform,
                                                                    !global.isConstant(), m_builder->getInt8Ty());

          // If the global variable is a constant, the data it points to is invariant.
          if (global.isConstant())
            m_builder->CreateInvariantStart(bufferDesc);

          Value *const bitCast = m_builder->CreateBitCast(bufferDesc, blockType);

          // We need to remove the block index from the original GEP indices so that we can use them.
          indices[1] = indices[0];

          ArrayRef<Value *> newIndices(indices);
          newIndices = newIndices.drop_front(1);

          Value *newGetElemPtr = nullptr;

          if (getElemPtr->isInBounds())
            newGetElemPtr = m_builder->CreateInBoundsGEP(bitCast, newIndices);
          else
            newGetElemPtr = m_builder->CreateGEP(bitCast, newIndices);

          getElemPtr->replaceAllUsesWith(newGetElemPtr);
          getElemPtr->eraseFromParent();
        }
      } else {
        m_builder->SetInsertPoint(&func->getEntryBlock(), func->getEntryBlock().getFirstInsertionPt());

        Value *const bufferDesc =
            m_builder->CreateLoadBufferDesc(descSet, binding, m_builder->getInt32(0),
                                            /*isNonUniform=*/false, !global.isConstant(), m_builder->getInt8Ty());

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
      m_builder->SetInsertPoint(&func->getEntryBlock(), func->getEntryBlock().getFirstInsertionPt());

      MDNode *metaNode = global.getMetadata(gSPIRVMD::PushConst);
      auto pushConstSize = mdconst::dyn_extract<ConstantInt>(metaNode->getOperand(0))->getZExtValue();
      Type *const pushConstantsType = ArrayType::get(m_builder->getInt8Ty(), pushConstSize);
      Value *pushConstants = m_builder->CreateLoadPushConstantsPtr(pushConstantsType);

      auto addrSpace = pushConstants->getType()->getPointerAddressSpace();
      Type *const castType = global.getType()->getPointerElementType()->getPointerTo(addrSpace);
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

  std::vector<Value *> indexOperands;
  for (unsigned i = 0, indexOperandCount = getElemPtr->getNumIndices(); i < indexOperandCount; ++i)
    indexOperands.push_back(toInt32Value(getElemPtr->getOperand(1 + i), &callInst));
  unsigned operandIdx = 0;

  auto input = cast<GlobalVariable>(getElemPtr->getPointerOperand());
  auto inputTy = input->getType()->getContainedType(0);

  MDNode *metaNode = input->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  auto inputMeta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

  if (getElemPtr->hasAllConstantIndices()) {
    auto loadValue = loadInOutMember(inputTy, SPIRAS_Input, indexOperands, operandIdx, 0, inputMeta, nullptr, nullptr,
                                     interpLoc, auxInterpValue, &callInst);

    m_interpCalls.insert(&callInst);
    callInst.replaceAllUsesWith(loadValue);
  } else // Interpolant an element via dynamic index by extending interpolant to each element
  {
    auto interpValueTy = inputTy;
    auto interpPtr = new AllocaInst(interpValueTy, m_module->getDataLayout().getAllocaAddrSpace(), "",
                                    &*(m_entryPoint->begin()->getFirstInsertionPt()));

    std::vector<unsigned> arraySizes;
    std::vector<unsigned> indexOperandIdxs;
    unsigned flattenElemCount = 1;
    auto elemTy = inputTy;
    for (unsigned i = 1, indexOperandCount = indexOperands.size(); i < indexOperandCount; ++i) {
      if (isa<ConstantInt>(indexOperands[i])) {
        unsigned index = (cast<ConstantInt>(indexOperands[i]))->getZExtValue();
        elemTy = elemTy->getContainedType(index);
      } else {
        arraySizes.push_back(cast<ArrayType>(elemTy)->getNumElements());
        elemTy = elemTy->getArrayElementType();
        flattenElemCount *= arraySizes.back();
        indexOperandIdxs.push_back(i);
      }
    }

    const unsigned arraySizeCount = arraySizes.size();
    SmallVector<unsigned, 4> elemStrides;
    elemStrides.resize(arraySizeCount, 1);
    for (unsigned i = arraySizeCount - 1; i > 0; --i)
      elemStrides[i - 1] = arraySizes[i] * elemStrides[i];

    std::vector<Value *> newIndexOperands = indexOperands;
    Value *interpValue = UndefValue::get(interpValueTy);

    for (unsigned elemIdx = 0; elemIdx < flattenElemCount; ++elemIdx) {
      unsigned flattenElemIdx = elemIdx;
      for (unsigned arraySizeIdx = 0; arraySizeIdx < arraySizeCount; ++arraySizeIdx) {
        unsigned index = flattenElemIdx / elemStrides[arraySizeIdx];
        flattenElemIdx = flattenElemIdx - index * elemStrides[arraySizeIdx];
        newIndexOperands[indexOperandIdxs[arraySizeIdx]] = ConstantInt::get(Type::getInt32Ty(*m_context), index, true);
      }

      auto loadValue = loadInOutMember(inputTy, SPIRAS_Input, newIndexOperands, operandIdx, 0, inputMeta, nullptr,
                                       nullptr, interpLoc, auxInterpValue, &callInst);

      std::vector<unsigned> idxs;
      for (auto indexIt = newIndexOperands.begin() + 1; indexIt != newIndexOperands.end(); ++indexIt)
        idxs.push_back((cast<ConstantInt>(*indexIt))->getZExtValue());
      interpValue = InsertValueInst::Create(interpValue, loadValue, idxs, "", &callInst);
    }
    new StoreInst(interpValue, interpPtr, &callInst);

    auto interpElemPtr = GetElementPtrInst::Create(nullptr, interpPtr, indexOperands, "", &callInst);
    auto interpElemTy = interpElemPtr->getType()->getPointerElementType();

    auto interpElemValue = new LoadInst(interpElemTy, interpElemPtr, "", &callInst);
    callInst.replaceAllUsesWith(interpElemValue);

    if (callInst.user_empty()) {
      callInst.dropAllReferences();
      callInst.eraseFromParent();
    }
  }
}

// =====================================================================================================================
// Translates an integer to 32-bit integer regardless of its initial bit width.
//
// @param value : Value to be translated
// @param insertPos : Where to insert the translation instructions
Value *SpirvLowerGlobal::toInt32Value(Value *value, Instruction *insertPos) {
  assert(isa<IntegerType>(value->getType()));
  auto valueTy = cast<IntegerType>(value->getType());

  const unsigned bitWidth = valueTy->getBitWidth();
  if (bitWidth > 32) {
    // Truncated to i32 type
    value = CastInst::CreateTruncOrBitCast(value, Type::getInt32Ty(*m_context), "", insertPos);
  } else if (bitWidth < 32) {
    // Extended to i32 type
    value = CastInst::CreateZExtOrBitCast(value, Type::getInt32Ty(*m_context), "", insertPos);
  }

  return value;
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for globals.
INITIALIZE_PASS(SpirvLowerGlobal, DEBUG_TYPE, "Lower SPIR-V globals (global variables, inputs, and outputs)", false,
                false)
