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
 * @file  LowerGLCompatibility.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerGLCompatibility.
 ***********************************************************************************************************************
 */
#include "LowerGLCompatibility.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcGraphicsContext.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/Pipeline.h"
#include "llvm/IR/DerivedTypes.h"

#define DEBUG_TYPE "llpc-spirv-lower-gl-compatibility"

using namespace llvm;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
LowerGLCompatibility::LowerGLCompatibility()
    : m_retInst(nullptr), m_entryPointEnd(nullptr), m_originalEntryBlock(nullptr), m_out(nullptr),
      m_clipVertex(nullptr), m_clipDistance(nullptr), m_clipPlane(nullptr), m_frontColor(nullptr), m_backColor(nullptr),
      m_frontSecondaryColor(nullptr), m_backSecondaryColor(nullptr), m_color(nullptr), m_secondaryColor(nullptr),
      m_frontFacing(nullptr), m_patchTexCoord(nullptr), m_fragColor(nullptr), m_fragDepth(), m_fragStencilRef() {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerGLCompatibility::run(Module &module, ModuleAnalysisManager &analysisManager) {
  SpirvLower::init(&module);
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-gl-compatibility\n");

  if (!needRun())
    return PreservedAnalyses::all();

  collectEmulationResource();

  if (!needLowerClipVertex() && !needLowerFrontColor() && !needLowerBackColor() && !needLowerFrontSecondaryColor() &&
      !needLowerBackSecondaryColor() && !needEmulateDrawPixels() && !needEmulateTwoSideLighting() &&
      !needEmulateBitmap() && !needLowerFragColor())
    return PreservedAnalyses::all();

  buildPatchPositionInfo();

  if (needLowerClipVertex())
    lowerClipVertex();

  if (needLowerFrontColor())
    lowerFrontColor();

  if (needLowerBackColor())
    lowerBackColor();

  if (needLowerFrontSecondaryColor())
    lowerFrontSecondaryColor();

  if (needLowerBackSecondaryColor())
    lowerBackSecondaryColor();

  if (needLowerFragColor())
    lowerFragColor();

  if (needEmulateDrawPixels())
    emulateDrawPixels();

  // Two side lighting patch should place just before bitmap patch.
  if (needEmulateTwoSideLighting())
    emulateTwoSideLighting();

  // Bit map patch should be the last patch in the pass.
  if (needEmulateBitmap())
    emulateBitmap();

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Use to check whether need run the pass.
bool LowerGLCompatibility::needRun() {
  bool result = false;
  if (m_context->getPipelineType() == PipelineType::Graphics) {
    auto moduleData =
        static_cast<const ShaderModuleData *>(static_cast<GraphicsContext *>(m_context->getPipelineContext())
                                                  ->getPipelineShaderInfo(m_shaderStage)
                                                  ->pModuleData);
    auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
    result |= moduleData->usage.useClipVertex;
    result |= moduleData->usage.useFrontColor;
    result |= moduleData->usage.useBackColor;
    result |= moduleData->usage.useFrontSecondaryColor;
    result |= moduleData->usage.useBackSecondaryColor;
    result |= buildInfo->glState.drawPixelsType != Vkgc::DrawPixelsTypeNone;
    result |= buildInfo->glState.enableTwoSideLighting;
    result |= buildInfo->glState.enableBitmap;
    result |= buildInfo->glState.enableBitmapLsb;
    result |= buildInfo->glState.enableColorClampFs;
  }
  return result;
}

// =====================================================================================================================
// Get location in meta data, if the global variable is UniformConstant.
//
// @param [in] var : Global variable to get uniform constant location
unsigned LowerGLCompatibility::getUniformLocation(llvm::GlobalVariable *var) {
  assert(var->getType()->getAddressSpace() == SPIRAS_Uniform && var->hasMetadata(gSPIRVMD::UniformConstant));
  MDNode *metaNode = var->getMetadata(gSPIRVMD::UniformConstant);
  return mdconst::dyn_extract<ConstantInt>(metaNode->getOperand(3))->getZExtValue();
}

// =====================================================================================================================
// Get in/out meta data by indices from from aggregate type.
//
// @param [in]  valueTy : The metadata's embellish type.
// @param [in]  mds     : The metadata constant of InOut Global variable to be decode.
// @param [in]  index   : The the index of the metadata in the embellish type.
// @param [out] out     : Use to output the element's metadatas of the InOut Global variable.
void LowerGLCompatibility::decodeInOutMetaRecursivelyByIndex(llvm::Type *valueTy, llvm::Constant *mds,
                                                             ArrayRef<Value *> index,
                                                             llvm::SmallVector<ShaderInOutMetadata> &out) {
  auto currentType = valueTy;
  auto currentMds = mds;
  if (!index.empty()) {
    if (valueTy->isSingleValueType()) {
      // Single type's metadata:{uint64, uint64}
      assert(mds->getType() == StructType::get(*m_context, {m_builder->getInt64Ty(), m_builder->getInt64Ty()}));
      ShaderInOutMetadata md = {};
      md.U64All[0] = cast<ConstantInt>(mds->getOperand(0))->getZExtValue();
      md.U64All[1] = cast<ConstantInt>(mds->getOperand(1))->getZExtValue();
      out.push_back(md);
    } else if (valueTy->isArrayTy()) {
      assert(mds->getType()->getStructNumElements() == 4);
      currentType = valueTy->getArrayElementType();
      currentMds = cast<Constant>(mds->getOperand(1));
      index = index.drop_front();
      if (index.empty())
        decodeInOutMetaRecursively(currentType, currentMds, out);
      else {
        decodeInOutMetaRecursivelyByIndex(currentType, currentMds, index, out);
      }
    } else if (valueTy->isStructTy()) {
      // Structure type's metadata:[{element metadata type}, ...]
      assert(valueTy->getStructNumElements() == mds->getType()->getStructNumElements());
      auto opIdx = cast<ConstantInt>(index[0])->getZExtValue();
      currentType = valueTy->getStructElementType(opIdx);
      currentMds = cast<Constant>(mds->getOperand(opIdx));
      index = index.drop_front();
      if (index.empty())
        decodeInOutMetaRecursively(currentType, currentMds, out);
      else {
        decodeInOutMetaRecursivelyByIndex(currentType, currentMds, index, out);
      }
    }
  }
}

// =====================================================================================================================
// Get in/out meta data recursively.
//
// @param [in]  valueTy : The metadata's embellish type.
// @param [in]  mds     : The metadata constant of InOut Global variable to be decode.
// @param [out] out     : Use to output the element's metadatas of the InOut Global variable.
void LowerGLCompatibility::decodeInOutMetaRecursively(llvm::Type *valueTy, llvm::Constant *mds,
                                                      llvm::SmallVector<ShaderInOutMetadata> &out) {
  ShaderInOutMetadata md = {};
  if (valueTy->isSingleValueType()) {
    // Single type's metadata:{uint64, uint64}
    assert(mds->getType() == StructType::get(*m_context, {m_builder->getInt64Ty(), m_builder->getInt64Ty()}));
    md.U64All[0] = cast<ConstantInt>(mds->getOperand(0))->getZExtValue();
    md.U64All[1] = cast<ConstantInt>(mds->getOperand(1))->getZExtValue();
    out.push_back(md);
  } else if (valueTy->isArrayTy()) {
    // Array type's metadata:{uint32, {element metadata type}, uint64, uint64}
    assert(mds->getType()->getStructNumElements() == 4);
    decodeInOutMetaRecursively(valueTy->getArrayElementType(), cast<Constant>(mds->getOperand(1)), out);
    md.U64All[0] = cast<ConstantInt>(mds->getOperand(2))->getZExtValue();
    md.U64All[1] = cast<ConstantInt>(mds->getOperand(3))->getZExtValue();
    out.push_back(md);
  } else if (valueTy->isStructTy()) {
    // Structure type's metadata:[{element metadata type}, ...]
    auto elementCount = valueTy->getStructNumElements();
    assert(elementCount == mds->getType()->getStructNumElements());
    for (signed opIdx = 0; opIdx < elementCount; opIdx++) {
      decodeInOutMetaRecursively(valueTy->getStructElementType(opIdx), cast<Constant>(mds->getOperand(opIdx)), out);
    }
  } else {
    llvm_unreachable("The Type can't be handle in decodeInOutMetaRecursively.");
  }
}

// =====================================================================================================================
// Collect "Return" instructions and replace those instructions with a branch instruction point to "ReturnBlock".
//
// @param [in]  func : The entry function of the shader module.
void LowerGLCompatibility::unifyFunctionReturn(Function *func) {
  SmallVector<ReturnInst *> retInsts;
  for (BasicBlock &block : *func) {
    Instruction *terminator = block.getTerminator();
    if (terminator != nullptr) {
      if (ReturnInst *retInst = dyn_cast<ReturnInst>(terminator)) {
        retInsts.push_back(retInst);
      }
    }
  }

  if (retInsts.size() > 1) {
    // Only create unify return block when the function's return instruction more then one.
    auto retBlock = BasicBlock::Create(*m_context, ".gl.compatibility.ret", m_entryPoint);
    m_retInst = ReturnInst::Create(*m_context, retBlock);
    for (auto inst : retInsts) {
      BranchInst::Create(retBlock, inst->getParent());
      inst->eraseFromParent();
    }
  } else {
    assert(retInsts.size() == 1);
    m_retInst = retInsts.back();
  }
}

// =====================================================================================================================
// Collect "EmitCall" instructions in the shader module.
void LowerGLCompatibility::collectEmitInst() {
  for (Function &function : m_module->functions()) {
    auto mangledName = function.getName();
    // We get all users before iterating because the iterator can be invalidated
    // by interpolateInputElement
    if (mangledName.starts_with(gSPIRVName::EmitVertex) || mangledName.starts_with(gSPIRVName::EmitStreamVertex)) {
      SmallVector<User *> users(function.users());
      for (User *user : users) {
        assert(isa<CallInst>(user) && "We should only have CallInst instructions here.");
        CallInst *callInst = cast<CallInst>(user);
        m_emitCalls.push_back(callInst);
      }
    }
  }
}

// =====================================================================================================================
// Build resource may used in compatibility emulation.
void LowerGLCompatibility::collectEmulationResource() {
  // Collect emulation information.
  for (auto &global : m_module->globals()) {
    if (global.getType()->getAddressSpace() == SPIRAS_Uniform && global.hasMetadata(gSPIRVMD::UniformConstant)) {
      if (getUniformLocation(&global) == Vkgc::GlCompatibilityUniformLocation::ClipPlane) {
        assert(m_clipPlane == nullptr);
        m_clipPlane = &global;
      }
    } else if (global.getType()->getAddressSpace() == SPIRAS_Input) {
      llvm::SmallVector<ShaderInOutMetadata> mds;
      MDNode *metaNode = global.getMetadata(gSPIRVMD::InOut);
      assert(metaNode);
      auto inOutMetaConst = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
      auto valueType = global.getValueType();
      bool isStructureOrArrayOfStructure =
          (valueType->isStructTy() || (valueType->isArrayTy() && valueType->getArrayElementType()->isStructTy()));
      decodeInOutMetaRecursively(valueType, inOutMetaConst, mds);
      if (m_shaderStage == ShaderStageFragment) {
        // In fragment shader, gl_Color have same location with gl_FrontColor in pre-stage outputs.
        // gl_SecondaryColor have same location with gl_FrontSecondaryColor in pre-stage outputs.
        // So we can use location of gl_FrontColor and gl_FrontSecondaryColor to find gl_Color and gl_FrontColor
        for (auto md : mds) {
          if (md.IsLoc) {
            if (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontColor) {
              if (isStructureOrArrayOfStructure)
                m_out = &global;
              else
                m_color = &global;
            }
            if (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor) {
              if (isStructureOrArrayOfStructure)
                m_out = &global;
              else
                m_secondaryColor = &global;
            }
          }
        }
      }
    } else if (global.getType()->getAddressSpace() == SPIRAS_Output) {
      llvm::SmallVector<ShaderInOutMetadata> mds;
      MDNode *metaNode = global.getMetadata(gSPIRVMD::InOut);
      assert(metaNode);
      auto inOutMetaConst = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
      auto valueType = global.getValueType();
      bool isStructureOrArrayOfStructure =
          (valueType->isStructTy() || (valueType->isArrayTy() && valueType->getArrayElementType()->isStructTy()));
      decodeInOutMetaRecursively(valueType, inOutMetaConst, mds);
      if (m_shaderStage == ShaderStageFragment) {
        for (auto md : mds) {
          if (md.IsBuiltIn) {
            if (md.Value == spv::BuiltInFragDepth) {
              m_fragDepth = &global;
            }
            if (md.Value == spv::BuiltInFragStencilRefEXT) {
              m_fragStencilRef = &global;
            }
          } else {
            assert(m_fragColor == nullptr);
            m_fragColor = &global;
          }
        }
      }
      for (auto md : mds) {
        if (md.IsLoc) {
          if (md.Value == Vkgc::GlCompatibilityInOutLocation::ClipVertex) {
            if (isStructureOrArrayOfStructure)
              m_out = &global;
            else
              m_clipVertex = &global;
          }
          if (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontColor) {
            if (isStructureOrArrayOfStructure)
              m_out = &global;
            else
              m_frontColor = &global;
          }
          if (md.Value == Vkgc::GlCompatibilityInOutLocation::BackColor) {
            if (isStructureOrArrayOfStructure)
              m_out = &global;
            else
              m_backColor = &global;
          }
          if (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor) {
            if (isStructureOrArrayOfStructure)
              m_out = &global;
            else
              m_frontSecondaryColor = &global;
          }
          if (md.Value == Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor) {
            if (isStructureOrArrayOfStructure)
              m_out = &global;
            else
              m_backSecondaryColor = &global;
          }
        } else if (md.IsBuiltIn) {
          if (md.Value == spv::BuiltInClipDistance) {
            if (isStructureOrArrayOfStructure)
              m_out = &global;
            else
              m_clipDistance = &global;
          }
          if (md.Value == spv::BuiltInFrontFacing)
            m_frontFacing = &global;
        }
      }
    }
  }

  // If gl_in/gl_out used in shader, then the Gl deprecated builtin variable will be pack in the structure:
  // gl_PerVertex. We need traversal the user of m_out to get the usage information Gl deprecated builtin variable.
  if (m_out != nullptr) {
    assert((m_clipVertex == nullptr) && (m_clipDistance == nullptr));
    llvm::SmallVector<ShaderInOutMetadata> mds;
    auto glOut = cast<GlobalVariable>(m_out);
    MDNode *metaNode = glOut->getMetadata(gSPIRVMD::InOut);
    assert(metaNode);
    auto inOutMetaConst = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
    for (User *user : m_out->users()) {
      SmallVector<Value *> indexOperands;
      // The user is a GEP
      // Check to see if the value has been stored.
      bool beenModified = false;
      User *gep = nullptr;
      if (auto *gepConst = dyn_cast<ConstantExpr>(user)) {
        auto operandsCount = gepConst->getNumOperands();
        // Skip the first indices, and the access chain target.
        for (size_t index = 2; index < operandsCount; index++) {
          auto *pIndex = dyn_cast<ConstantInt>(gepConst->getOperand(index));
          if (pIndex) {
            indexOperands.push_back(pIndex);
          }
        }
        gep = gepConst;
      } else if (auto *gepInst = dyn_cast<GetElementPtrInst>(user)) {
        // We shouldn't have any chained GEPs here, they are coalesced by the LowerAccessChain pass.
        for (auto index = gepInst->idx_begin(); index != gepInst->idx_end(); index++) {
          // Skip the first indices, it should be 0 in most of time.
          if (index == gepInst->idx_begin()) {
            assert(cast<ConstantInt>(gepInst->idx_begin())->isZero() && "Non-zero GEP first index\n");
            continue;
          }
          indexOperands.push_back(m_builder->CreateZExtOrTrunc(index->get(), m_builder->getInt32Ty()));
        }
        gep = gepInst;
      }
      if (gep != nullptr) {
        for (User *gepUser : gep->users()) {
          assert(!isa<GetElementPtrInst>(gepUser));
          beenModified |= isa<StoreInst>(gepUser);
        }
        decodeInOutMetaRecursivelyByIndex(glOut->getValueType(), inOutMetaConst, indexOperands, mds);
        for (auto md : mds) {
          if (md.IsLoc) {
            if (beenModified && (md.Value == Vkgc::GlCompatibilityInOutLocation::ClipVertex))
              m_clipVertex = gep;
            if (beenModified && (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontColor))
              m_frontColor = gep;
            if (beenModified && (md.Value == Vkgc::GlCompatibilityInOutLocation::BackColor))
              m_backColor = gep;
            if (beenModified && (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor))
              m_frontSecondaryColor = gep;
            if (beenModified && (md.Value == Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor))
              m_backSecondaryColor = gep;
          } else if (md.IsBuiltIn && md.Value == spv::BuiltInClipDistance) {
            m_clipDistance = gep;
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Acquire the patch pointer for do lower, function unifyFunctionReturn may cause IR change.
void LowerGLCompatibility::buildPatchPositionInfo() {
  if (m_shaderStage == ShaderStageGeometry)
    collectEmitInst();
  else
    unifyFunctionReturn(m_entryPoint);

  // Create early kill block for bitmap, bitmap require a early return in masked thread.
  if (needEmulateBitmap()) {
    m_originalEntryBlock = &(m_entryPoint->getEntryBlock());
    m_originalEntryBlock->splitBasicBlockBefore(m_originalEntryBlock->getFirstInsertionPt(), ".gl.compatibility.entry");
    m_entryPointEnd = m_originalEntryBlock->splitBasicBlockBefore(m_originalEntryBlock->getFirstInsertionPt(),
                                                                  ".gl.compatibility.kill");
    m_builder->SetInsertPoint(m_entryPointEnd->begin());
    m_builder->CreateKill();
    ReturnInst::Create(*m_context, m_entryPointEnd);
    m_entryPointEnd->back().eraseFromParent();
  }
}

// =====================================================================================================================
// Check whether need do lower for ClipVertex.
bool LowerGLCompatibility::needLowerClipVertex() {
  return (m_clipVertex != nullptr && !m_clipVertex->user_empty());
}

// =====================================================================================================================
// Check whether need do lower for FrontColor.
bool LowerGLCompatibility::needLowerFrontColor() {
  return (m_frontColor != nullptr && !m_frontColor->user_empty());
}

// =====================================================================================================================
// Check whether need do lower for BackColor.
bool LowerGLCompatibility::needLowerBackColor() {
  return (m_backColor != nullptr && !m_backColor->user_empty());
}

// =====================================================================================================================
// Check whether need do lower for FrontSecondaryColor.
bool LowerGLCompatibility::needLowerFrontSecondaryColor() {
  return (m_frontSecondaryColor != nullptr && !m_frontSecondaryColor->user_empty());
}

// =====================================================================================================================
// Check whether need do lower for BackSecondaryColor.
bool LowerGLCompatibility::needLowerBackSecondaryColor() {
  return (m_backSecondaryColor != nullptr && !m_backSecondaryColor->user_empty());
}

// =====================================================================================================================
// Check whether need do emulate for draw pixels.
bool LowerGLCompatibility::needEmulateDrawPixels() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) && (buildInfo->glState.drawPixelsType != Vkgc::DrawPixelsTypeNone);
}

// =====================================================================================================================
// Check whether need do emulate for two-side lighting.
bool LowerGLCompatibility::needEmulateTwoSideLighting() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) && buildInfo->glState.enableTwoSideLighting &&
         (m_color != nullptr || m_secondaryColor != nullptr);
}

// =====================================================================================================================
// Check whether need do emulate for bitmap.
bool LowerGLCompatibility::needEmulateBitmap() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) &&
         (buildInfo->glState.enableBitmap || buildInfo->glState.enableBitmapLsb);
}

// =====================================================================================================================
// Check whether need do clamp fs
bool LowerGLCompatibility::needLowerFragColor() {
  auto buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return m_fragColor && (m_shaderStage == ShaderStageFragment) && (buildInfo->glState.enableColorClampFs);
}

// =====================================================================================================================
// Create InOut global variable Metadata.
//
// @param [in] md : The base information of the in/out meta date.
MDTuple *LowerGLCompatibility::createInOutMd(const ShaderInOutMetadata &md) {
  auto int64Type = m_builder->getInt64Ty();
  // Built metadata for the array element
  std::vector<Constant *> mdValues;
  // int64Type : Content of "ShaderInOutMetadata.U64All[0]"
  // int64Type : Content of "ShaderInOutMetadata.U64All[1]"
  auto elmdTy = StructType::get(*m_context, {int64Type, int64Type});
  assert(elmdTy != nullptr);
  mdValues.push_back(ConstantInt::get(int64Type, md.U64All[0]));
  mdValues.push_back(ConstantInt::get(int64Type, md.U64All[1]));
  auto mdVariable = ConstantStruct::get(elmdTy, mdValues);

  // Setup input/output metadata
  std::vector<Metadata *> mDs;
  mDs.push_back(ConstantAsMetadata::get(mdVariable));
  return MDNode::get(*m_context, mDs);
}

// =====================================================================================================================
// Create builtin InOut global variable Metadata.
//
// @param [in] builtIn : The built-in kind of the in/out meta date.
MDTuple *LowerGLCompatibility::createBuiltInInOutMd(lgc::BuiltInKind builtIn) {
  ShaderInOutMetadata inOutMd = {};
  inOutMd.IsBuiltIn = true;
  inOutMd.Value = builtIn;
  return createInOutMd(inOutMd);
}

// =====================================================================================================================
// Create the SPIR-V output builtin variable "gl_ClipDistance".
void LowerGLCompatibility::createClipDistance() {
  assert(m_clipDistance == nullptr);
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  uint32_t indexOfLastClipPlane = 0;
  Util::BitMaskScanReverse(&indexOfLastClipPlane, buildInfo->rsState.usrClipPlaneMask);

  auto floatType = m_builder->getFloatTy();
  auto int32Type = m_builder->getInt32Ty();
  auto int64Type = m_builder->getInt64Ty();

  auto clipDistanceType = ArrayType::get(floatType, indexOfLastClipPlane + 1);
  auto clipDistance =
      new GlobalVariable(*m_module, clipDistanceType, false, GlobalValue::ExternalLinkage, nullptr, "gl_ClipDistance",
                         nullptr, GlobalVariable::NotThreadLocal, SPIRV::SPIRAS_Output);

  ShaderInOutMetadata inOutMd = {};
  inOutMd.IsBuiltIn = true;
  inOutMd.IsLoc = false;
  inOutMd.Value = spv::BuiltInClipDistance;

  // Built metadata for the array element
  std::vector<Constant *> mdValues;
  // int64Type : Content of "ShaderInOutMetadata.U64All[0]"
  // int64Type : Content of "ShaderInOutMetadata.U64All[1]"
  auto elmdTy = StructType::get(*m_context, {int64Type, int64Type});
  assert(elmdTy != nullptr);
  mdValues.push_back(ConstantInt::get(int64Type, inOutMd.U64All[0]));
  mdValues.push_back(ConstantInt::get(int64Type, inOutMd.U64All[1]));
  auto mdElement = ConstantStruct::get(elmdTy, mdValues);

  // Build metadata for the array.
  // int32Type : Stride
  // elmdTy    : Element MD type
  // int64Type : Content of "ShaderInOutMetadata.U64All[0]"
  // int64Type : Content of "ShaderInOutMetadata.U64All[1]"
  auto mdTy = StructType::get(*m_context, {int32Type, elmdTy, int64Type, int64Type});
  assert(mdTy != nullptr);
  mdValues.clear();
  mdValues.push_back(ConstantInt::get(int32Type, 1));
  mdValues.push_back(mdElement);
  mdValues.push_back(ConstantInt::get(int64Type, inOutMd.U64All[0]));
  mdValues.push_back(ConstantInt::get(int64Type, inOutMd.U64All[1]));
  auto *mdVariable = ConstantStruct::get(static_cast<StructType *>(mdTy), mdValues);

  // Setup input/output metadata
  std::vector<Metadata *> mDs;
  mDs.push_back(ConstantAsMetadata::get(mdVariable));
  auto mdNode = MDNode::get(*m_context, mDs);
  clipDistance->addMetadata(gSPIRVMD::InOut, *mdNode);
  m_clipDistance = clipDistance;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_ClipPlane".
void LowerGLCompatibility::createClipPlane() {
  auto floatType = m_builder->getFloatTy();
  auto vec4Type = FixedVectorType::get(floatType, 4);
  auto clipPlaneType = ArrayType::get(vec4Type, 8);
  auto clipPlane =
      new GlobalVariable(*m_module, clipPlaneType, false, GlobalValue::ExternalLinkage, nullptr, "gl_ClipPlaneInternal",
                         nullptr, GlobalVariable::NotThreadLocal, SPIRV::SPIRAS_Uniform);
  auto locationFound =
      getUniformConstantEntryByLocation(m_context, m_shaderStage, Vkgc::GlCompatibilityUniformLocation::ClipPlane);
  auto clipPlaneBaseOffset = locationFound != nullptr ? locationFound->offset : 0;
  assert(m_shaderStage != ShaderStageTask && m_shaderStage != ShaderStageMesh);
  unsigned constBufferBinding =
      Vkgc::ConstantBuffer0Binding + static_cast<GraphicsContext *>(m_context->getPipelineContext())
                                         ->getPipelineShaderInfo(m_shaderStage)
                                         ->options.constantBufferBindingOffset;

  std::vector<Metadata *> mDs;
  auto int32Ty = Type::getInt32Ty(*m_context);
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, Vkgc::InternalDescriptorSetId)));
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, constBufferBinding)));
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, clipPlaneBaseOffset)));
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, Vkgc::GlCompatibilityUniformLocation::ClipPlane)));
  auto mdNode = MDNode::get(*m_context, mDs);
  clipPlane->addMetadata(gSPIRVMD::UniformConstant, *mdNode);
  m_clipPlane = clipPlane;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_BackColor".
void LowerGLCompatibility::createBackColor() {
  auto vec4Type = FixedVectorType::get(m_builder->getFloatTy(), 4);
  auto backColor = new GlobalVariable(*m_module, vec4Type, false, GlobalValue::ExternalLinkage, nullptr, "gl_BackColor",
                                      nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Input);
  ShaderInOutMetadata inOutMd = {};
  inOutMd.IsLoc = true;
  inOutMd.Value = Vkgc::GlCompatibilityInOutLocation::BackColor;
  inOutMd.InterpMode = InterpModeSmooth;
  inOutMd.InterpLoc = InterpLocCenter;
  backColor->addMetadata(gSPIRVMD::InOut, *createInOutMd(inOutMd));
  m_backColor = backColor;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_BackSecondaryColor".
void LowerGLCompatibility::createBackSecondaryColor() {
  auto vec4Type = FixedVectorType::get(m_builder->getFloatTy(), 4);
  auto backSecondaryColor =
      new GlobalVariable(*m_module, vec4Type, false, GlobalValue::ExternalLinkage, nullptr, "gl_BackSecondaryColor",
                         nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Input);
  ShaderInOutMetadata inOutMd = {};
  inOutMd.IsLoc = true;
  inOutMd.Value = Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor;
  inOutMd.InterpMode = InterpModeSmooth;
  inOutMd.InterpLoc = InterpLocCenter;
  backSecondaryColor->addMetadata(gSPIRVMD::InOut, *createInOutMd(inOutMd));
  m_backSecondaryColor = backSecondaryColor;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_FrontFacing".
void LowerGLCompatibility::createFrontFacing() {
  assert(m_frontFacing == nullptr);
  auto frontFacing =
      new GlobalVariable(*m_module, m_builder->getInt1Ty(), false, GlobalValue::ExternalLinkage, nullptr,
                         "gl_FrontFacing", nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Input);
  frontFacing->addMetadata(gSPIRVMD::InOut, *createBuiltInInOutMd(lgc::BuiltInKind::BuiltInFrontFacing));
  m_frontFacing = frontFacing;
}

// =====================================================================================================================
// Create the ARB builtin variable "patchTexCoord".
void LowerGLCompatibility::createPatchTexCoord() {
  auto vec2Type = FixedVectorType::get(m_builder->getFloatTy(), 2);
  auto patchTexCoord =
      new GlobalVariable(*m_module, vec2Type, false, GlobalValue::ExternalLinkage, nullptr, "patchTexCoord", nullptr,
                         GlobalVariable::NotThreadLocal, SPIRV::SPIRAS_Input);
  ShaderInOutMetadata inOutMd = {};
  inOutMd.IsLoc = true;
  inOutMd.Value = Vkgc::GlCompatibilityInOutLocation::PatchTexCoord;
  inOutMd.InterpMode = InterpModeSmooth;
  inOutMd.InterpLoc = InterpLocCenter;
  patchTexCoord->addMetadata(gSPIRVMD::InOut, *createInOutMd(inOutMd));
  m_patchTexCoord = patchTexCoord;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_FragDepth".
void LowerGLCompatibility::createFragDepth() {
  assert(m_fragDepth == nullptr);
  auto fragDepth =
      new GlobalVariable(*m_module, m_builder->getFloatTy(), false, GlobalValue::ExternalLinkage, nullptr,
                         "gl_FragDepth", nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Output);
  fragDepth->addMetadata(gSPIRVMD::InOut, *createBuiltInInOutMd(lgc::BuiltInKind::BuiltInFragDepth));
  m_fragDepth = fragDepth;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_fragStencilRef".
void LowerGLCompatibility::createFragStencilRef() {
  assert(m_fragStencilRef == nullptr);
  auto fragStencilRef =
      new GlobalVariable(*m_module, m_builder->getInt32Ty(), false, GlobalValue::ExternalLinkage, nullptr,
                         "gl_FragStencilRef", nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Output);
  fragStencilRef->addMetadata(gSPIRVMD::InOut, *createBuiltInInOutMd(lgc::BuiltInKind::BuiltInFragStencilRef));
  m_fragStencilRef = fragStencilRef;
}

// =====================================================================================================================
// Inline the emulation instruction of clip vertex.
void LowerGLCompatibility::emulateStoreClipVertex() {
  auto floatType = m_builder->getFloatTy();
  Type *vec4Type = VectorType::get(floatType, 4, false);
  // Load clipVertex
  Value *clipVertex = m_builder->CreateLoad(vec4Type, m_clipVertex);
  // Create a new intermediate result variable
  assert(m_context->getPipelineType() == PipelineType::Graphics);
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  auto clipPlaneMask = buildInfo->rsState.usrClipPlaneMask;
  for (uint32_t clipPlaneIdx = 0; clipPlaneIdx < Vkgc::GlCompatibilityLimits::MaxClipPlanes; ++clipPlaneIdx) {
    if (clipPlaneMask & (1 << clipPlaneIdx)) {
      // gl_ClipPlane are Emulate by uniform constant, so the resource descriptor are same with uniform constant.
      auto clipPlaneElement = m_builder->CreateConstInBoundsGEP1_32(vec4Type, m_clipPlane, clipPlaneIdx);
      auto clipPlaneLoad = m_builder->CreateLoad(vec4Type, clipPlaneElement);

      // Dot ClipPlane and ClipVertex
      auto dot = m_builder->CreateDotProduct(clipVertex, clipPlaneLoad);

      // Store result to ClipDistance
      auto clipDistanceElement = m_builder->CreateConstInBoundsGEP1_32(floatType, m_clipDistance, clipPlaneIdx);
      m_builder->CreateStore(dot, clipDistanceElement);
    }
  }
}

// =====================================================================================================================
// Inline the emulation instruction of front/back/front secondary/back secondary color.
//
// @param [in] color : One of front/back/front secondary/back secondary color.
void LowerGLCompatibility::emulationOutputColor(llvm::User *color) {
  auto floatType = m_builder->getFloatTy();
  Type *vec4Type = VectorType::get(floatType, 4, false);
  // Load frontColor
  auto info = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  if ((m_shaderStage == ShaderStageVertex && info->glState.enableColorClampVs) ||
      (m_shaderStage == ShaderStageFragment && info->glState.enableColorClampFs)) {
    Value *colorOperand = m_builder->CreateLoad(vec4Type, color);
    Value *clampedColor =
        m_builder->CreateFClamp(colorOperand, ConstantFP::get(vec4Type, 0.0), ConstantFP::get(vec4Type, 1.0));
    // Store color
    m_builder->CreateStore(clampedColor, color);
  }
}

// =====================================================================================================================
// Emulate for draw pixels emulation.
void LowerGLCompatibility::emulateDrawPixels() {
  m_builder->SetInsertPoint(m_entryPoint->getEntryBlock().begin());
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  auto floatType = m_builder->getFloatTy();
  auto int32Type = m_builder->getInt32Ty();
  auto vec2Type = FixedVectorType::get(floatType, 2);
  auto vec4Type = FixedVectorType::get(floatType, 4);
  auto ivec2Type = FixedVectorType::get(int32Type, 2);
  if (m_patchTexCoord == nullptr) {
    createPatchTexCoord();
  }
  Value *patchTexcoord = m_builder->CreateLoad(vec2Type, m_patchTexCoord);
  Value *texcoord = m_builder->CreateFPToUI(patchTexcoord, ivec2Type);
  auto imageDescPtr = m_builder->CreateGetDescPtr(
      lgc::ResourceNodeType::DescriptorResource, lgc::ResourceNodeType::DescriptorResource,
      PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorResource),
      Vkgc::InternalBinding::PixelOpInternalBinding);
  Value *texel = m_builder->CreateImageLoad(vec4Type, Dim2D, 0, imageDescPtr, texcoord, nullptr);

  // Write Color
  if (buildInfo->glState.drawPixelsType == Vkgc::DrawPixelsTypeColor) {
    if (m_color != nullptr) {
      // replace scale and bias constant with real value
      std::vector<Constant *> vals;
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferScale[0]));
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferScale[1]));
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferScale[2]));
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferScale[3]));
      auto scale = ConstantVector::get(vals);

      vals.clear();
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferBias[0]));
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferBias[1]));
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferBias[2]));
      vals.push_back(ConstantFP::get(floatType, buildInfo->glState.pixelTransferBias[3]));
      auto bias = ConstantVector::get(vals);
      auto color = m_builder->CreateFma(texel, scale, bias);
      m_builder->CreateStore(color, m_color);
    }
  }

  // Write Depth
  if (buildInfo->glState.drawPixelsType == Vkgc::DrawPixelsTypeDepth) {
    if (m_fragDepth == nullptr)
      createFragDepth();
    auto depth = m_builder->CreateExtractElement(texel, ConstantInt::get(int32Type, 0));
    m_builder->CreateStore(depth, m_fragDepth);
  }

  // Write Stencil
  if (buildInfo->glState.drawPixelsType == Vkgc::DrawPixelsTypeStencil) {
    if (m_fragStencilRef == nullptr)
      createFragStencilRef();
    auto stencil = m_builder->CreateExtractElement(texel, ConstantInt::get(int32Type, 0));
    auto stencilInt = m_builder->CreateBitCast(stencil, int32Type);
    m_builder->CreateStore(stencilInt, m_fragStencilRef);
  }
}

// =====================================================================================================================
// Emulate for two-side lighting.
void LowerGLCompatibility::emulateTwoSideLighting() {
  auto vec4Type = FixedVectorType::get(m_builder->getFloatTy(), 4);
  if (m_shaderStage == ShaderStageFragment) {
    m_builder->SetInsertPoint(m_entryPoint->getEntryBlock().begin());
    if (m_color != nullptr || m_secondaryColor != nullptr) {
      if (m_frontFacing == nullptr) {
        createFrontFacing();
      }
      if (m_color != nullptr) {
        assert(m_backColor == nullptr);
        createBackColor();
        auto frontColorLoad = m_builder->CreateLoad(vec4Type, m_color);
        auto backColorLoad = m_builder->CreateLoad(vec4Type, m_backColor);
        auto frontFacingLoad = m_builder->CreateLoad(m_builder->getInt1Ty(), m_frontFacing);
        auto color = m_builder->CreateSelect(frontFacingLoad, frontColorLoad, backColorLoad);
        m_builder->CreateStore(color, m_color);
      }
      if (m_secondaryColor != nullptr) {
        assert(m_backSecondaryColor == nullptr);
        createBackSecondaryColor();
        auto frontSecondaryColorLoad = m_builder->CreateLoad(vec4Type, m_secondaryColor);
        auto backSecondaryColorLoad = m_builder->CreateLoad(vec4Type, m_backSecondaryColor);
        auto frontFacingLoad = m_builder->CreateLoad(m_builder->getInt1Ty(), m_frontFacing);
        auto secondaryColor = m_builder->CreateSelect(frontFacingLoad, frontSecondaryColorLoad, backSecondaryColorLoad);
        m_builder->CreateStore(secondaryColor, m_secondaryColor);
      }
    }
  }
}

// =====================================================================================================================
// Emulate for bitmap emulation.
void LowerGLCompatibility::emulateBitmap() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  m_builder->SetInsertPoint(m_entryPoint->getEntryBlock().begin());
  auto floatType = m_builder->getFloatTy();
  auto int32Type = m_builder->getInt32Ty();
  auto vec2Type = FixedVectorType::get(floatType, 2);
  auto ivec2Type = FixedVectorType::get(int32Type, 2);
  if (!m_patchTexCoord) {
    createPatchTexCoord();
  }
  Value *constInt0x7 = ConstantInt::get(ivec2Type, 0x7);
  Value *constInt0x3 = ConstantInt::get(ivec2Type, 0x3);
  Value *patchTexcoord = m_builder->CreateLoad(vec2Type, m_patchTexCoord);
  Value *texcoord = m_builder->CreateFPToUI(patchTexcoord, ivec2Type);
  Value *mask = m_builder->CreateAnd(texcoord, constInt0x7);
  if (buildInfo->glState.enableBitmapLsb) {
    mask = m_builder->CreateSub(mask, constInt0x7);
  }
  mask = m_builder->CreateShl(ConstantInt::get(ivec2Type, 1), mask);
  Value *texCoordSrc = m_builder->CreateLShr(constInt0x3, texcoord);
  auto imageDescPtr = m_builder->CreateGetDescPtr(
      lgc::ResourceNodeType::DescriptorResource, lgc::ResourceNodeType::DescriptorResource,
      PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorResource),
      Vkgc::InternalBinding::PixelOpInternalBinding);
  Value *texel = m_builder->CreateImageLoad(ivec2Type, Dim2D, 0, imageDescPtr, texCoordSrc, nullptr);
  Value *val = m_builder->CreateAnd(mask, texel);
  val = m_builder->CreateExtractElement(val, ConstantInt::get(int32Type, 0));
  auto cmp = m_builder->CreateICmpEQ(val, ConstantInt::get(int32Type, 0));
  m_builder->CreateCondBr(cmp, m_entryPointEnd, m_originalEntryBlock);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_ClipVertex".
void LowerGLCompatibility::lowerClipVertex() {
  if (m_clipPlane == nullptr)
    createClipPlane();
  if (m_clipDistance == nullptr)
    createClipDistance();

  if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessControl ||
      m_shaderStage == ShaderStageTessEval) {
    assert(m_retInst != nullptr);
    m_builder->SetInsertPoint(m_retInst);
    emulateStoreClipVertex();
  } else if (m_shaderStage == ShaderStageGeometry) {
    for (auto emitCall : m_emitCalls) {
      m_builder->SetInsertPoint(emitCall);
      emulateStoreClipVertex();
    }
  }
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_FrontColor" or "gl_BackColor" or "gl_FrontSecondaryColor" or
// "gl_BackSecondaryColor".
//
// @param [in] color : One of gl_FrontColor/gl_BackColor/gl_FrontSecondaryColor/gl_BackSecondaryColor.
void LowerGLCompatibility::lowerColor(llvm::User *color) {
  if (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessControl ||
      m_shaderStage == ShaderStageTessEval || m_shaderStage == ShaderStageFragment) {
    assert(m_retInst != nullptr);
    m_builder->SetInsertPoint(m_retInst);
    emulationOutputColor(color);
  } else if (m_shaderStage == ShaderStageGeometry) {
    for (auto emitCall : m_emitCalls) {
      m_builder->SetInsertPoint(emitCall);
      emulationOutputColor(color);
    }
  }
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_FrontColor".
void LowerGLCompatibility::lowerFrontColor() {
  lowerColor(m_frontColor);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_BackColor".
void LowerGLCompatibility::lowerBackColor() {
  lowerColor(m_backColor);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_FrontSecondaryColor".
void LowerGLCompatibility::lowerFrontSecondaryColor() {
  lowerColor(m_frontSecondaryColor);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_BackSecondaryColor".
void LowerGLCompatibility::lowerBackSecondaryColor() {
  lowerColor(m_backSecondaryColor);
}

// =====================================================================================================================
// Does clamp fragment color
void LowerGLCompatibility::lowerFragColor() {
  lowerColor(m_fragColor);
}

} // namespace Llpc
