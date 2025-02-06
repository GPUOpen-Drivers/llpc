/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ScalarReplacementOfBuiltins.cpp
 * @brief LLPC source file: split and replace global variables that are structures containing built-in values
 ***********************************************************************************************************************
 */
#include "ScalarReplacementOfBuiltins.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDialect.h"
#include "vkgcDefs.h"
#include "spirv/spirv.hpp"
#include "lgc/Builder.h"
#include "llvm/ADT/ADL.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include <cassert>

#define DEBUG_TYPE "scalar-replacement-of-builtins"

using namespace llvm;
using namespace lgc;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses ScalarReplacementOfBuiltins::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(
      dbgs() << "Run the pass refactor and replace global variables that are structures containing built-in values\n");

  bool changed = false;
  SpirvLower::init(&module);
  SmallVector<GlobalVariable *> originalGlobals(make_pointer_range(m_module->globals()));
  for (auto &global : originalGlobals) {
    if (!needsSplit(global))
      continue;

    if (global->getValueType()->isStructTy()) {
      splitBuiltinStructure(global);
      changed = true;
    } else if (global->getValueType()->isArrayTy()) {
      splitBuiltinArray(global);
      changed = true;
    }
  }
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Retrieves metadata for shader input/output elements based on their type.
//
// @param elementType : Type of the shader input/output element
// @param elementMetadata : Metadata values for initializing the metadata structure
ShaderInOutMetadata ScalarReplacementOfBuiltins::getShaderInOutMetadata(Type *elementType, Constant *elementMetadata) {
  ShaderInOutMetadata inOutMeta = {};
  if (elementType->isArrayTy()) {
    assert(elementMetadata->getNumOperands() == 4);
    inOutMeta.U64All[0] = cast<ConstantInt>(elementMetadata->getOperand(2))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(elementMetadata->getOperand(3))->getZExtValue();
  } else {
    assert(elementMetadata->getNumOperands() == 2);
    inOutMeta.U64All[0] = cast<ConstantInt>(elementMetadata->getOperand(0))->getZExtValue();
    inOutMeta.U64All[1] = cast<ConstantInt>(elementMetadata->getOperand(1))->getZExtValue();
  }
  return inOutMeta;
}

// =====================================================================================================================
// Determine whether the structure needs to be split.
//
// @param globalBuiltinVar : Global variable containing built-in type
bool ScalarReplacementOfBuiltins::needsSplit(GlobalVariable *globalBuiltinVar) {
  auto addressSpace = globalBuiltinVar->getType()->getAddressSpace();
  if (addressSpace != SPIRV::SPIRAS_Output && addressSpace != SPIRV::SPIRAS_Input)
    return false;

  Type *valueType = globalBuiltinVar->getValueType();
  // NOTE: If the global value type to be split is a structure or array.
  if (!valueType->isStructTy() && !valueType->isArrayTy())
    return false;

  MDNode *globalVarMetaNode = globalBuiltinVar->getMetadata(gSPIRVMD::InOut);
  Constant *inOutMetaConst = mdconst::dyn_extract<Constant>(globalVarMetaNode->getOperand(0));
  Constant *firstMemberMeta = nullptr;
  Type *firstMemberTy = nullptr;

  if (valueType->isArrayTy()) {
    Type *arrayElemmentTy = valueType->getArrayElementType();
    // Note: If the global value type to be split is an array, the member type must be a structure type.
    // This is because, according to OpenGL specifications, members of gl_in, gl_out, and gl_MeshVerticesEXT must be of
    // structure type.
    if (!arrayElemmentTy->isStructTy())
      return false;

    Constant *structureMds = dyn_cast<Constant>(inOutMetaConst->getOperand(1));

    firstMemberTy = arrayElemmentTy->getStructElementType(0);
    firstMemberMeta = dyn_cast<Constant>(structureMds->getOperand(0));
  } else if (globalBuiltinVar->getValueType()->isStructTy()) {
    // NOTE: If the global value type to be split is a structure, the first member of the structure must be a built-in
    // value or a location type for compatibility variables. Only such structures can be split.
    Type *globalBuiltinVarTy = globalBuiltinVar->getValueType();
    assert(globalBuiltinVarTy->isStructTy());

    firstMemberTy = globalBuiltinVarTy->getStructElementType(0);
    firstMemberMeta = cast<Constant>(inOutMetaConst->getOperand(0));
  }

  // NOTE: If the first member is of structure type, we do not need to split it because gl_in, gl_out, or gl_PerVertex
  // do not have any members that are of structure type.
  if (firstMemberTy->isStructTy())
    return false;
  ShaderInOutMetadata firstMeta = getShaderInOutMetadata(firstMemberTy, firstMemberMeta);
  // Note: This condition handles only built-in and location value types.
  assert(firstMeta.IsBuiltIn || firstMeta.IsLoc);
  unsigned builtInId = firstMeta.Value;
  if (firstMeta.IsBuiltIn) {
    switch (builtInId) {
    case spv::BuiltInPosition:
    case spv::BuiltInPointSize:
    case spv::BuiltInClipDistance:
    case spv::BuiltInCullDistance:
      return true;
    default:
      return false;
    }
  } else {
    switch (builtInId) {
    case Vkgc::GlCompatibilityInOutLocation::ClipVertex:
    case Vkgc::GlCompatibilityInOutLocation::FrontColor:
    case Vkgc::GlCompatibilityInOutLocation::BackColor:
    case Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor:
    case Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor:
    case Vkgc::GlCompatibilityInOutLocation::TexCoord:
    case Vkgc::GlCompatibilityInOutLocation::FogFragCoord:
      return true;
    default:
      return false;
    }
  }

  return false;
}

// =====================================================================================================================
// Resolves the name of a built-in shader element based on its metadata.
//
// @param inOutMeta : Reference to the metadata structure describing the shader element
// @returns : The resolved name of the built-in shader element as a StringRef
StringRef ScalarReplacementOfBuiltins::getBuiltinElementName(ShaderInOutMetadata &inOutMeta) {
  StringRef builtinElementName;
  unsigned builtInId = inOutMeta.Value;
  if (inOutMeta.IsBuiltIn) {
    switch (builtInId) {
    case spv::BuiltInPosition:
      builtinElementName = "_gl_Position";
      break;
    case spv::BuiltInPointSize:
      builtinElementName = "_gl_PointSize";
      break;
    case spv::BuiltInClipDistance:
      builtinElementName = "_gl_ClipDistance";
      break;
    case spv::BuiltInCullDistance:
      builtinElementName = "_gl_CullDistance";
      break;
    default:
      llvm_unreachable("Not implemented");
      break;
    }
  } else {
    switch (builtInId) {
    case Vkgc::GlCompatibilityInOutLocation::ClipVertex:
      builtinElementName = "_gl_ClipVertex";
      break;
    case Vkgc::GlCompatibilityInOutLocation::FrontColor:
      builtinElementName = "_gl_FrontColor";
      break;
    case Vkgc::GlCompatibilityInOutLocation::BackColor:
      builtinElementName = "_gl_BackColor";
      break;
    case Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor:
      builtinElementName = "_gl_FrontSecondaryColor";
      break;
    case Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor:
      builtinElementName = "_gl_BackSecondaryColor";
      break;
    case Vkgc::GlCompatibilityInOutLocation::TexCoord:
      builtinElementName = "_gl_TexCoord";
      break;
    case Vkgc::GlCompatibilityInOutLocation::FogFragCoord:
      builtinElementName = "_gl_FogFragCoord";
      break;
    default:
      llvm_unreachable("Not implemented");
      break;
    }
  }
  return builtinElementName;
}

// =====================================================================================================================
// Removes unused newly created built-in global variables.
//
// @param elements : Vector of users associated with newly created global variables
void ScalarReplacementOfBuiltins::cleanUpUnusedGlobals(SmallVector<User *> &elements) {
  for (User *user : make_early_inc_range(elements)) {
    GlobalVariable *globalValueReplace = cast<GlobalVariable>(user);
    if (globalValueReplace->users().empty()) {
      globalValueReplace->dropAllReferences();
      globalValueReplace->eraseFromParent();
    }
  }
  return;
}

// =====================================================================================================================
// Replaces users of a global variable with newly created global variables.
//
// @param globalBuiltinVar : Global variable containing built-in type
// @param elements : Vector of users associated with newly created global variables
void ScalarReplacementOfBuiltins::replaceGlobalBuiltinVar(GlobalVariable *globalBuiltinVar,
                                                          SmallVector<User *> &elements) {
  convertUsersOfConstantsToInstructions(globalBuiltinVar);
  for (User *user : make_early_inc_range(globalBuiltinVar->users())) {
    if (StoreInst *storeInst = dyn_cast<StoreInst>(user)) {
      [[maybe_unused]] const DataLayout &dataLayout = storeInst->getModule()->getDataLayout();
      GlobalVariable *globalVar = cast<GlobalVariable>(elements[0]);
      assert(dataLayout.getTypeStoreSize(storeInst->getValueOperand()->getType()) <=
             dataLayout.getTypeStoreSize(globalVar->getValueType()));
      storeInst->replaceUsesOfWith(globalBuiltinVar, globalVar);
    } else if (LoadInst *loadInst = dyn_cast<LoadInst>(user)) {
      GlobalVariable *LoadValue = cast<GlobalVariable>(elements[0]);
      loadInst->replaceUsesOfWith(globalBuiltinVar, LoadValue);
    } else if (auto *gepInst = dyn_cast<StructuralGepOp>(user)) {
      SmallVector<Value *, 8> indices;
      // NOTE: The newly generated global variables are created based on the elements of the original global structure
      // variable or global array variable. Therefore, when encountering a GetElementPtr (GEP) instruction, we utilize
      // the second operand to determine which of the newly generated global variables corresponds to a specific element
      // in the original type.
      // For example:
      //   structure built-in: getelementptr { <4 x float>, float, ... }, ptr addrspace(65) @0, i32 0, i32 1
      //   array built-in: getelementptr [3 x { <4 x float>, ... }], ptr addrspace(65) @1, i32 0, i32 %5, i32 0, i32 2
      //  ===>
      //   scalarized structure built-in: getelementptr float, ptr addrspace(65) @gl_out_0, i32 0
      //   scalarized array built-in: getelementptr [3 x <4 x float>], ptr addrspace(65) @gl_out_1, i32 0, i32 %5, i32 2
      //
      // The first one index is always 0 dereference the pointer value. The element idx (1 if original global variable
      // is a structure, or 2 if the original global variable is an array) indicates which built-in variable is used.
      assert(globalBuiltinVar->getValueType()->isStructTy() || globalBuiltinVar->getValueType()->isArrayTy());
      const auto indexRange = gepInst->getIndices();
      const auto elementIdxIt =
          std::next(llvm::adl_begin(indexRange), globalBuiltinVar->getValueType()->isStructTy() ? 1 : 2);
      indices.append(llvm::adl_begin(indexRange), elementIdxIt);
      // Remove the element index from the indices.
      const unsigned index = cast<ConstantInt>(*elementIdxIt)->getZExtValue();
      const auto indicesAfterElementIdx = llvm::make_range(std::next(elementIdxIt), llvm::adl_end(indexRange));
      indices.append(llvm::adl_begin(indicesAfterElementIdx), llvm::adl_end(indicesAfterElementIdx));
      assert(cast<ConstantInt>(indices[0])->isZero() && "Non-zero GEP first index\n");
      Type *globalValueReplaceTy = cast<GlobalVariable>(elements[index])->getValueType();
      m_builder->SetInsertPoint(gepInst);
      Value *gepElement = m_builder->create<StructuralGepOp>(elements[index], globalValueReplaceTy, false, indices);
      gepInst->replaceAllUsesWith(gepElement);
      gepInst->eraseFromParent();
    } else {
      llvm_unreachable("Not implemented");
    }
  }
  return;
}

// =====================================================================================================================
// Splits a global variable of structure type containing built-in elements into individual components.
//
// @param globalBuiltinVar : Global variable containing built-in type
void ScalarReplacementOfBuiltins::splitBuiltinStructure(GlobalVariable *globalBuiltinVar) {
  SmallVector<User *> elements;
  StringRef prefixName = globalBuiltinVar->getName();
  MDNode *metaNode = globalBuiltinVar->getMetadata(gSPIRVMD::InOut);
  assert(metaNode);
  Constant *inOutMetaConst = mdconst::extract<Constant>(metaNode->getOperand(0));
  Type *globalBuiltinVarTy = globalBuiltinVar->getValueType();
  assert(globalBuiltinVarTy->isStructTy());
  auto structElementCount = globalBuiltinVarTy->getStructNumElements();
  assert(structElementCount == inOutMetaConst->getType()->getStructNumElements());

  for (unsigned idx = 0; idx < structElementCount; ++idx) {
    Type *elementType = globalBuiltinVarTy->getStructElementType(idx);
    Constant *elementMetadata = cast<Constant>(inOutMetaConst->getOperand(idx));
    ShaderInOutMetadata inOutMeta = getShaderInOutMetadata(elementType, elementMetadata);

    // Note: This condition handles only built-in and location value types.
    assert(inOutMeta.IsBuiltIn || inOutMeta.IsLoc);
    StringRef builtinElementName = getBuiltinElementName(inOutMeta);
    GlobalVariable *replacementBuiltinVar = new GlobalVariable(
        *m_module, elementType, false, GlobalValue::ExternalLinkage, nullptr, prefixName + builtinElementName, nullptr,
        GlobalVariable::NotThreadLocal, globalBuiltinVar->getType()->getAddressSpace());

    replacementBuiltinVar->addMetadata(gSPIRVMD::InOut,
                                       *MDNode::get(*m_context, {ConstantAsMetadata::get(elementMetadata)}));
    elements.push_back(replacementBuiltinVar);
  }

  // NOTE: Replace global variable users.
  replaceGlobalBuiltinVar(globalBuiltinVar, elements);

  // Cleans up unused newly created built-in global variables.
  cleanUpUnusedGlobals(elements);

  globalBuiltinVar->dropAllReferences();
  globalBuiltinVar->eraseFromParent();
  return;
}

// =====================================================================================================================
// Splits a global variable of array type containing built-in elements into individual components.
//
// @param globalBuiltinVar : Global variable containing built-in type
void ScalarReplacementOfBuiltins::splitBuiltinArray(GlobalVariable *globalBuiltinVar) {
  assert(globalBuiltinVar->getValueType()->getArrayElementType()->isStructTy());
  Type *arrayElemmentTy = globalBuiltinVar->getValueType()->getArrayElementType();
  auto structureElementNum = arrayElemmentTy->getStructNumElements();
  StringRef prefixName = globalBuiltinVar->getName();
  auto arrayElementNum = globalBuiltinVar->getValueType()->getArrayNumElements();
  SmallVector<User *> elements;
  MDNode *globalVarMetaNode = globalBuiltinVar->getMetadata(gSPIRVMD::InOut);
  assert(globalVarMetaNode);
  Constant *inOutMetaConst = mdconst::dyn_extract<Constant>(globalVarMetaNode->getOperand(0));
  Constant *structureMds = dyn_cast<Constant>(inOutMetaConst->getOperand(1));
  auto int32Type = m_builder->getInt32Ty();
  auto int64Type = m_builder->getInt64Ty();

  for (int idx = 0; idx < structureElementNum; ++idx) {
    Constant *memberMeta = dyn_cast<Constant>(structureMds->getOperand(idx));
    assert(memberMeta && "memberMeta should not be null");

    Type *memberElementTy = arrayElemmentTy->getStructElementType(idx);
    ShaderInOutMetadata inOutMeta = getShaderInOutMetadata(memberElementTy, memberMeta);
    auto builtInId = inOutMeta.Value;
    ArrayType *replaceElementTy = ArrayType::get(memberElementTy, arrayElementNum);
    // Note: This condition handles only built-in and location value types.
    assert((inOutMeta.IsBuiltIn || inOutMeta.IsLoc) && "Expected built-in or location metadata");
    StringRef builtinElementName = getBuiltinElementName(inOutMeta);

    GlobalVariable *replaceBuiltinElement =
        new GlobalVariable(*m_module, replaceElementTy, globalBuiltinVar->isConstant(), globalBuiltinVar->getLinkage(),
                           nullptr, prefixName + builtinElementName, nullptr, globalBuiltinVar->getThreadLocalMode(),
                           globalBuiltinVar->getType()->getAddressSpace());

    ShaderInOutMetadata memberInOutMd = {};
    memberInOutMd.IsBuiltIn = inOutMeta.IsBuiltIn;
    memberInOutMd.IsLoc = inOutMeta.IsLoc;
    memberInOutMd.Value = builtInId;

    Type *elmdTy = memberMeta->getType();
    StructType *mdTy = StructType::get(*m_context, {int32Type, elmdTy, int64Type, int64Type});
    SmallVector<Constant *, 4> mdValues;
    mdValues.push_back(ConstantInt::get(int32Type, 1));
    mdValues.push_back(memberMeta);
    mdValues.push_back(ConstantInt::get(int64Type, memberInOutMd.U64All[0]));
    mdValues.push_back(ConstantInt::get(int64Type, memberInOutMd.U64All[1]));

    Constant *mdVariable = ConstantStruct::get(mdTy, mdValues);
    replaceBuiltinElement->addMetadata(gSPIRVMD::InOut,
                                       *MDNode::get(*m_context, {ConstantAsMetadata::get(mdVariable)}));
    elements.push_back(replaceBuiltinElement);
  }

  replaceGlobalBuiltinVar(globalBuiltinVar, elements);

  // Cleans up unused newly created built-in global variables.
  cleanUpUnusedGlobals(elements);
  globalBuiltinVar->dropAllReferences();
  globalBuiltinVar->eraseFromParent();
  return;
}

} // namespace Llpc
