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
 * @file  LowerGlCompatibility.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerGlCompatibility.
 ***********************************************************************************************************************
 */
#include "LowerGlCompatibility.h"
#include "LoweringUtil.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDialect.h"
#include "llpcGraphicsContext.h"
#include "lgc/Builder.h"
#include "lgc/Pipeline.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"

#define DEBUG_TYPE "lower-gl-compatibility"

using namespace llvm;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
LowerGlCompatibility::LowerGlCompatibility()
    : m_retInst(nullptr), m_entryPointEnd(nullptr), m_originalEntryBlock(nullptr), m_clipVertex(nullptr),
      m_clipDistance(nullptr), m_clipPlane(nullptr), m_frontColor(nullptr), m_backColor(nullptr),
      m_frontSecondaryColor(nullptr), m_backSecondaryColor(nullptr), m_color(nullptr), m_secondaryColor(nullptr),
      m_frontFacing(nullptr), m_patchTexCoord(nullptr), m_fragColor(nullptr), m_fragDepth(), m_fragStencilRef() {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerGlCompatibility::run(Module &module, ModuleAnalysisManager &analysisManager) {
  SpirvLower::init(&module);
  LLVM_DEBUG(dbgs() << "Run the pass Lower-gl-compatibility\n");

  collectEmulationResource();

  if (!needRun())
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

  if (needLowerAlphaTest())
    lowerAlphaTest();

  if (needEmulateDrawPixels())
    emulateDrawPixels();

  if (needEmulateSmoothStipple())
    emulateSmoothStipple();

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
bool LowerGlCompatibility::needRun() {
  bool result = false;
  if (m_context->getPipelineType() == PipelineType::Graphics) {
    auto moduleData =
        static_cast<const ShaderModuleData *>(static_cast<GraphicsContext *>(m_context->getPipelineContext())
                                                  ->getPipelineShaderInfo(m_shaderStage)
                                                  ->pModuleData);
    auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
    auto options = m_context->getPipelineContext()->getPipelineOptions();
    bool enableAlphaTest =
        (m_shaderStage == Vkgc::ShaderStageFragment && buildInfo->glState.alphaTestFunc != Vkgc::AlphaTestFunc::Always);
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
    result |= options->getGlState().enablePolygonStipple;
    result |= options->getGlState().enableLineSmooth;
    result |= options->getGlState().enablePointSmooth;
    result |= enableAlphaTest;
  }
  return result;
}

// =====================================================================================================================
// Get location in meta data, if the global variable is UniformConstant.
//
// @param [in] var : Global variable to get uniform constant location
unsigned LowerGlCompatibility::getUniformLocation(llvm::GlobalVariable *var) {
  assert(var->getType()->getAddressSpace() == SPIRAS_Uniform && var->hasMetadata(gSPIRVMD::UniformConstant));
  MDNode *metaNode = var->getMetadata(gSPIRVMD::UniformConstant);
  return mdconst::extract<ConstantInt>(metaNode->getOperand(3))->getZExtValue();
}

// =====================================================================================================================
// Retrieves metadata for shader input/output elements based on their type.
//
// @param elementType : Type of the shader input/output element
// @param elementMetadata : Metadata values for initializing the metadata structure
ShaderInOutMetadata LowerGlCompatibility::getShaderInOutMetadata(Type *elementType, Constant *elementMetadata) {
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
// Collect "Return" instructions and replace those instructions with a branch instruction point to "ReturnBlock".
//
// @param [in]  func : The entry function of the shader module.
void LowerGlCompatibility::unifyFunctionReturn(Function *func) {
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
void LowerGlCompatibility::collectEmitInst() {
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
void LowerGlCompatibility::collectEmulationResource() {
  // Collect emulation information.
  for (auto &global : m_module->globals()) {
    Type *valueType = global.getValueType();
    // Note: The compatibility type structure or array of structures will be separated in this lowering pass
    // by ScalarReplacementOfBuiltins. There are no types to handle in this lower compatibility pass.
    if (valueType->isStructTy())
      continue;
    if (global.getType()->getAddressSpace() == SPIRAS_Uniform && global.hasMetadata(gSPIRVMD::UniformConstant)) {
      if (getUniformLocation(&global) == Vkgc::GlCompatibilityUniformLocation::ClipPlane) {
        assert(m_clipPlane == nullptr);
        m_clipPlane = &global;
      }
    } else if (global.getType()->getAddressSpace() == SPIRAS_Input) {
      MDNode *metaNode = global.getMetadata(gSPIRVMD::InOut);
      assert(metaNode);
      Constant *inOutMetaConst = mdconst::extract<Constant>(metaNode->getOperand(0));
      ShaderInOutMetadata inOutMeta = getShaderInOutMetadata(valueType, inOutMetaConst);
      unsigned builtInId = inOutMeta.Value;

      if (m_shaderStage == ShaderStageFragment) {
        // In fragment shader, gl_Color have same location with gl_FrontColor in pre-stage outputs.
        // gl_SecondaryColor have same location with gl_FrontSecondaryColor in pre-stage outputs.
        // So we can use location of gl_FrontColor and gl_FrontSecondaryColor to find gl_Color and gl_FrontColor
        if (inOutMeta.IsLoc) {
          if (builtInId == Vkgc::GlCompatibilityInOutLocation::FrontColor)
            m_color = &global;
          else if (builtInId == Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor)
            m_secondaryColor = &global;
        }
      }
    } else if (global.getType()->getAddressSpace() == SPIRAS_Output) {
      MDNode *metaNode = global.getMetadata(gSPIRVMD::InOut);
      assert(metaNode);
      Constant *inOutMetaConst = mdconst::extract<Constant>(metaNode->getOperand(0));
      ShaderInOutMetadata inOutMeta = getShaderInOutMetadata(valueType, inOutMetaConst);
      unsigned builtInId = inOutMeta.Value;

      if (m_shaderStage == ShaderStageFragment) {
        if (inOutMeta.IsBuiltIn) {
          if (builtInId == spv::BuiltInFragDepth)
            m_fragDepth = &global;
          else if (builtInId == spv::BuiltInFragStencilRefEXT)
            m_fragStencilRef = &global;
        } else {
          if (builtInId == Vkgc::GlCompatibilityInOutLocation::SpecialFragOut)
            m_fragColor = &global;
        }
      }

      if (inOutMeta.IsLoc) {
        switch (builtInId) {
        case Vkgc::GlCompatibilityInOutLocation::ClipVertex:
          m_clipVertex = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::FrontColor:
          m_frontColor = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::BackColor:
          m_backColor = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor:
          m_frontSecondaryColor = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor:
          m_backSecondaryColor = &global;
          break;
        default:
          break;
        }
      } else if (inOutMeta.IsBuiltIn) {
        switch (builtInId) {
        case spv::BuiltInClipDistance:
          m_clipDistance = &global;
          break;
        case spv::BuiltInFrontFacing:
          m_frontFacing = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::BackColor:
          m_backColor = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::FrontSecondaryColor:
          m_frontSecondaryColor = &global;
          break;
        case Vkgc::GlCompatibilityInOutLocation::BackSecondaryColor:
          m_backSecondaryColor = &global;
          break;
        default:
          break;
        }
      }
    }
  }
}

// =====================================================================================================================
// Acquire the patch pointer for do lower, function unifyFunctionReturn may cause IR change.
void LowerGlCompatibility::buildPatchPositionInfo() {
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
bool LowerGlCompatibility::needLowerClipVertex() {
  return m_clipVertex != nullptr;
}

// =====================================================================================================================
// Check whether need do lower for FrontColor.
bool LowerGlCompatibility::needLowerFrontColor() {
  return m_frontColor != nullptr;
}

// =====================================================================================================================
// Check whether need do lower for BackColor.
bool LowerGlCompatibility::needLowerBackColor() {
  return m_backColor != nullptr;
}

// =====================================================================================================================
// Check whether need do lower for FrontSecondaryColor.
bool LowerGlCompatibility::needLowerFrontSecondaryColor() {
  return m_frontSecondaryColor != nullptr;
}

// =====================================================================================================================
// Check whether need do lower for BackSecondaryColor.
bool LowerGlCompatibility::needLowerBackSecondaryColor() {
  return m_backSecondaryColor != nullptr;
}

// =====================================================================================================================
// Check whether need do emulate for draw pixels.
bool LowerGlCompatibility::needEmulateDrawPixels() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) && (buildInfo->glState.drawPixelsType != Vkgc::DrawPixelsTypeNone);
}

// =====================================================================================================================
// Check whether need do emulate for two-side lighting.
bool LowerGlCompatibility::needEmulateTwoSideLighting() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) && buildInfo->glState.enableTwoSideLighting &&
         (m_color != nullptr || m_secondaryColor != nullptr);
}

// =====================================================================================================================
// Check whether need do emulate for bitmap.
bool LowerGlCompatibility::needEmulateBitmap() {
  auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) && buildInfo->glState.enableBitmap;
}

// =====================================================================================================================
// Check whether need do emulate point/line smooth and line/polygon stipple.
bool LowerGlCompatibility::needEmulateSmoothStipple() {
  auto options = m_context->getPipelineContext()->getPipelineOptions();
  return (m_shaderStage == ShaderStageFragment) &&
         (options->getGlState().enablePolygonStipple || options->getGlState().enableLineSmooth ||
          options->getGlState().enablePointSmooth);
}

// =====================================================================================================================
// Check whether need do clamp fs
bool LowerGlCompatibility::needLowerFragColor() {
  auto buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return m_fragColor && (m_shaderStage == ShaderStageFragment) && (buildInfo->glState.enableColorClampFs);
}

// =====================================================================================================================
// Check whether need do alphaTest.
bool LowerGlCompatibility::needLowerAlphaTest() {
  auto buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  return (m_shaderStage == ShaderStageFragment) && (buildInfo->glState.alphaTestFunc != Vkgc::AlphaTestFunc::Always);
}

// =====================================================================================================================
// Create InOut global variable Metadata.
//
// @param [in] md : The base information of the in/out meta date.
MDTuple *LowerGlCompatibility::createInOutMd(const ShaderInOutMetadata &md) {
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
MDTuple *LowerGlCompatibility::createBuiltInInOutMd(lgc::BuiltInKind builtIn) {
  ShaderInOutMetadata inOutMd = {};
  inOutMd.IsBuiltIn = true;
  inOutMd.Value = builtIn;
  return createInOutMd(inOutMd);
}

// =====================================================================================================================
// Create the SPIR-V output builtin variable "gl_ClipDistance".
void LowerGlCompatibility::createClipDistance() {
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
void LowerGlCompatibility::createClipPlane() {
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
void LowerGlCompatibility::createBackColor() {
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
void LowerGlCompatibility::createBackSecondaryColor() {
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
void LowerGlCompatibility::createFrontFacing() {
  assert(m_frontFacing == nullptr);
  auto frontFacing =
      new GlobalVariable(*m_module, m_builder->getInt1Ty(), false, GlobalValue::ExternalLinkage, nullptr,
                         "gl_FrontFacing", nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Input);
  frontFacing->addMetadata(gSPIRVMD::InOut, *createBuiltInInOutMd(lgc::BuiltInKind::BuiltInFrontFacing));
  m_frontFacing = frontFacing;
}

// =====================================================================================================================
// Create the ARB builtin variable "patchTexCoord".
void LowerGlCompatibility::createPatchTexCoord() {
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
void LowerGlCompatibility::createFragDepth() {
  assert(m_fragDepth == nullptr);
  auto fragDepth =
      new GlobalVariable(*m_module, m_builder->getFloatTy(), false, GlobalValue::ExternalLinkage, nullptr,
                         "gl_FragDepth", nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Output);
  fragDepth->addMetadata(gSPIRVMD::InOut, *createBuiltInInOutMd(lgc::BuiltInKind::BuiltInFragDepth));
  m_fragDepth = fragDepth;
}

// =====================================================================================================================
// Create the GLSL builtin variable "gl_fragStencilRef".
void LowerGlCompatibility::createFragStencilRef() {
  assert(m_fragStencilRef == nullptr);
  auto fragStencilRef =
      new GlobalVariable(*m_module, m_builder->getInt32Ty(), false, GlobalValue::ExternalLinkage, nullptr,
                         "gl_FragStencilRef", nullptr, GlobalVariable::GeneralDynamicTLSModel, SPIRV::SPIRAS_Output);
  fragStencilRef->addMetadata(gSPIRVMD::InOut, *createBuiltInInOutMd(lgc::BuiltInKind::BuiltInFragStencilRef));
  m_fragStencilRef = fragStencilRef;
}

// =====================================================================================================================
// Inline the emulation instruction of clip vertex.
void LowerGlCompatibility::emulateStoreClipVertex() {
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
void LowerGlCompatibility::emulationOutputColor(llvm::User *color) {
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
void LowerGlCompatibility::emulateDrawPixels() {
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
void LowerGlCompatibility::emulateTwoSideLighting() {
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
void LowerGlCompatibility::emulateBitmap() {
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
// Patch alpha scaling factor to the 4th channel of a fragment output, excluding built-in variables.
//
// @param [in] val           : input value for alpha scaling, which is an output in fragment stage.
// @param [in] valTy         : current input value's type, should be global's valueType in top-level.
// @param [in] metaVal       : metadata value of current output variable.
// @param [in] alphaScaleVal : calculated alpha scaling results, default value is one.
void LowerGlCompatibility::patchAlphaScaling(Value *val, Type *valTy, Constant *metaVal, Value *alphaScaleVal) {
  ShaderInOutMetadata outputMeta = {};

  if (valTy->isArrayTy()) {
    outputMeta.U64All[0] = cast<ConstantInt>(metaVal->getOperand(2))->getZExtValue();
    outputMeta.U64All[1] = cast<ConstantInt>(metaVal->getOperand(3))->getZExtValue();

    if (!outputMeta.IsBuiltIn) {
      auto elemMeta = cast<Constant>(metaVal->getOperand(1));
      const uint64_t elemCount = val->getType()->getArrayNumElements();
      for (unsigned idx = 0; idx < elemCount; ++idx) {
        Value *elem = m_builder->CreateExtractValue(val, {idx}, "");
        patchAlphaScaling(elem, elem->getType(), elemMeta, alphaScaleVal);
      }
    }
  } else if (valTy->isStructTy()) {
    const uint64_t memberCount = val->getType()->getStructNumElements();
    for (unsigned memberIdx = 0; memberIdx < memberCount; ++memberIdx) {
      auto memberMeta = cast<Constant>(metaVal->getOperand(memberIdx));
      Value *member = m_builder->CreateExtractValue(val, {memberIdx});
      patchAlphaScaling(member, member->getType(), memberMeta, alphaScaleVal);
    }
  } else {
    Constant *inOutMetaConst = cast<Constant>(metaVal);
    outputMeta.U64All[0] = cast<ConstantInt>(inOutMetaConst->getOperand(0))->getZExtValue();
    outputMeta.U64All[1] = cast<ConstantInt>(inOutMetaConst->getOperand(1))->getZExtValue();

    // When enabling line smooth, alpha channel will be patched with a scaling factor.
    if (!outputMeta.IsBuiltIn && outputMeta.NumComponents == 4 && alphaScaleVal) {
      Value *outputValue = m_builder->CreateLoad(valTy, val);
      Value *scaledAlpha = m_builder->CreateExtractElement(outputValue, 3);
      Value *alphaScaleFactor = m_builder->CreateLoad(m_builder->getFloatTy(), alphaScaleVal);
      scaledAlpha = m_builder->CreateFMul(alphaScaleFactor, scaledAlpha);
      outputValue = m_builder->CreateInsertElement(outputValue, scaledAlpha, m_builder->getInt32(3));
      m_builder->CreateStore(outputValue, val);
    }
  }
}

// =====================================================================================================================
// Emulate for point/line smooth and line/polygon stipple.
void LowerGlCompatibility::emulateSmoothStipple() {
  auto options = m_context->getPipelineContext()->getPipelineOptions();
  auto pipelineBuildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  bool needYInvert = pipelineBuildInfo->getGlState().originUpperLeft;
  m_builder->SetInsertPointPastAllocas(m_entryPoint);
  // Acquire FragCoord.
  Value *fragCoord = m_builder->CreateReadBuiltInInput(lgc::BuiltInKind::BuiltInFragCoord);
  // Acquire PrimType.
  // 0 : point.
  // 1 : line.
  // 2 : triangle.
  // 3 : rectangle.
  // PrimType (i32) : comes from HW PS Input : ANCILLARY_ENA - Prim Type[1:0]
  Value *primType = m_builder->CreateReadBuiltInInput(lgc::BuiltInKind::BuiltInPrimType);

  // 1. Patch Polygon Stipple.
  if (options->getGlState().enablePolygonStipple) {
    constexpr uint32_t PolygonStippleSize = 32; // For Y Invert.

    // If this is in triangle mode, skip emulation.
    Value *isTriangle = m_builder->CreateICmpUGT(primType, m_builder->getInt32(1));
    m_builder->SetInsertPoint(SplitBlockAndInsertIfThen(isTriangle, m_builder->GetInsertPoint(), false));

    Value *calcFragCoord = m_builder->CreateFPToUI(fragCoord, FixedVectorType::get(m_builder->getInt32Ty(), 4));
    Value *calcFragCoordX = m_builder->CreateExtractElement(calcFragCoord, m_builder->getInt32(0));
    Value *calcFragCoordY = m_builder->CreateExtractElement(calcFragCoord, m_builder->getInt32(1));
    Value *bufferDesc = m_builder->create<lgc::LoadBufferDescOp>(
        Vkgc::InternalDescriptorSetId, Vkgc::InternalBinding::PixelOpInternalBinding, m_builder->getInt32(0),
        lgc::Builder::BufferFlagNonConst);

    // For Y Invert
    if (needYInvert) {
      Value *winSizeOffset =
          m_builder->CreateInBoundsGEP(m_builder->getInt32Ty(), bufferDesc, m_builder->getInt32(PolygonStippleSize));
      winSizeOffset = m_builder->CreateLoad(m_builder->getInt32Ty(), winSizeOffset);
      calcFragCoordY = m_builder->CreateSub(winSizeOffset, calcFragCoordY);
    }

    // active = ( x % 32 ) & ( y % 32 )
    // HW load polygon stipple pattern in right order in Bytes here, y offset doesn't need to be reverted.
    Value *yOffset = m_builder->CreateAnd(calcFragCoordY, m_builder->getInt32(0x1fu));
    Value *descPtr = m_builder->CreateInBoundsGEP(m_builder->getInt32Ty(), bufferDesc, yOffset);
    Value *stipplePattern = m_builder->CreateLoad(m_builder->getInt32Ty(), descPtr);

    // xOffset = ( x % 32 ) / 8
    Value *xOffset = m_builder->CreateAnd(calcFragCoordX, m_builder->getInt32(0x18u));
    // xInByteOffset = x % 8
    Value *xInByteOffset = m_builder->CreateAnd(calcFragCoordX, m_builder->getInt32(0x7u));
    // xInByteOffset = 7 - xInByteOffset
    // Due to concern with default turned on option LsbFirst, x bits are in reverse order within each 8 bits pattern.
    if (pipelineBuildInfo->glState.enableBitmapLsb) {
      xInByteOffset = m_builder->CreateSub(m_builder->getInt32(0x7u), xInByteOffset);
    }
    // xOffset = xInByteOffset + xOffset
    xOffset = m_builder->CreateAdd(xOffset, xInByteOffset);

    Value *shouldDiscard = m_builder->CreateExtractBitField(stipplePattern, xOffset, m_builder->getInt32(1), false);
    shouldDiscard = m_builder->CreateICmpEQ(shouldDiscard, m_builder->getInt32(0));
    m_builder->SetInsertPoint(SplitBlockAndInsertIfThen(shouldDiscard, m_builder->GetInsertPoint(), false));
    m_builder->CreateKill();
  }

  // 2. Patch Line Smooth.
  if (options->getGlState().enableLineSmooth) {
    Value *isLine = m_builder->CreateICmpEQ(primType, m_builder->getInt32(1));
    Value *alphaScaleVal = m_builder->CreateAllocaAtFuncEntry(m_builder->getFloatTy(), "patchAlphaScale");
    m_builder->CreateStore(ConstantFP::get(m_builder->getFloatTy(), 1.0), alphaScaleVal);
    m_builder->SetInsertPoint(SplitBlockAndInsertIfThen(isLine, m_builder->GetInsertPoint(), false));

    // Get const for line smooth
    Value *lineSmoothConstArr[4];
    for (uint32_t i = 0; i < 4; i++)
      lineSmoothConstArr[i] = ConstantFP::get(m_builder->getFloatTy(), pipelineBuildInfo->getGlState().lineSmooth[i]);

    // Emulate line stipple with wide AA line
    if (options->getGlState().emulateWideLineStipple) {
      // LineStipple (f32) is read from SPIA:LINE_STIPPLE_TEX_ENA
      Value *lineStipple = m_builder->CreateReadBuiltInInput(lgc::BuiltInKind::BuiltInLineStipple);
      Value *lineStippleScale = lineSmoothConstArr[2];
      Value *lineStipplePattern = m_builder->CreateBitCast(lineSmoothConstArr[3], m_builder->getInt32Ty());

      Value *result = m_builder->CreateFMul(lineStipple, lineStippleScale);
      result = m_builder->CreateFPToSI(result, m_builder->getInt32Ty());
      result = m_builder->CreateAnd(result, m_builder->getInt32(15));
      result = m_builder->CreateShl(m_builder->getInt32(1), result);
      // lineSmooth[3] is the line stipple pattern, it is integer in memory.
      result = m_builder->CreateAnd(result, lineStipplePattern);
      Value *shouldDiscard = m_builder->CreateICmpEQ(result, m_builder->getInt32(0));
      m_builder->SetInsertPoint(SplitBlockAndInsertIfThen(shouldDiscard, m_builder->GetInsertPoint(), false));
      m_builder->CreateKill();
    }

    // Primitive Coord (fp32vec2)
    Value *primCoord = m_builder->CreateReadBuiltInInput(lgc::BuiltInKind::BuiltInPrimCoord);
    Value *negHalfLineWidth = m_builder->CreateFNeg(lineSmoothConstArr[0]);
    Value *lineWidth = m_builder->CreateFMul(lineSmoothConstArr[0], ConstantFP::get(m_builder->getFloatTy(), 2.0));
    Value *alphaBias = lineSmoothConstArr[1];

    primCoord = m_builder->CreateExtractElement(primCoord, 1);
    Value *scaledVal = m_builder->CreateFma(primCoord, lineWidth, negHalfLineWidth);
    // Recalculate alpha scale value which will be inserted into frag color's alpha channel, when doing smooth.
    scaledVal = m_builder->CreateIntrinsic(Intrinsic::fabs, scaledVal->getType(), scaledVal);
    scaledVal = m_builder->CreateFSub(alphaBias, scaledVal);
    m_builder->CreateStore(scaledVal, alphaScaleVal);

    m_builder->SetInsertPoint(m_retInst);
    for (GlobalVariable &global : m_module->globals()) {
      auto addrSpace = global.getType()->getAddressSpace();
      if (addrSpace == SPIRAS_Output) {
        auto outputMetaVal = mdconst::extract<Constant>(global.getMetadata(gSPIRVMD::InOut)->getOperand(0));
        patchAlphaScaling(&global, global.getValueType(), outputMetaVal, alphaScaleVal);
      }
    }
  }

  // 3. Patch Point Smooth.
  if (options->getGlState().enablePointSmooth) {
    Value *isPoint = m_builder->CreateICmpEQ(primType, m_builder->getInt32(0));
    Value *alphaScaleVal = m_builder->CreateAllocaAtFuncEntry(m_builder->getFloatTy(), "patchAlphaScale");
    m_builder->CreateStore(ConstantFP::get(m_builder->getFloatTy(), 1.0), alphaScaleVal);
    m_builder->SetInsertPoint(SplitBlockAndInsertIfThen(isPoint, m_builder->GetInsertPoint(), false));
    // Primitive Coord (fp32vec2)
    Value *primCoord =
        m_builder->CreateReadBuiltInInput(lgc::BuiltInKind::BuiltInPrimCoord); // Get const for line smooth

    Value *pointSmoothConstArr[2];
    for (uint32_t i = 0; i < 2; i++)
      pointSmoothConstArr[i] = ConstantFP::get(m_builder->getFloatTy(), pipelineBuildInfo->getGlState().pointSmooth[i]);

    Value *halfPointSize = pointSmoothConstArr[0];
    Value *alphaBias = pointSmoothConstArr[1];

    Value *negHalfPointSize = m_builder->CreateFNeg(halfPointSize);
    Value *negHalfPointSizeVal = PoisonValue::get(FixedVectorType::get(m_builder->getFloatTy(), 2));
    negHalfPointSizeVal = m_builder->CreateInsertElement(negHalfPointSizeVal, negHalfPointSize, m_builder->getInt32(0));
    negHalfPointSizeVal = m_builder->CreateInsertElement(negHalfPointSizeVal, negHalfPointSize, m_builder->getInt32(1));
    Value *pointSize = m_builder->CreateFMul(halfPointSize, ConstantFP::get(m_builder->getFloatTy(), 2.0));
    Value *pointSizeVal = PoisonValue::get(FixedVectorType::get(m_builder->getFloatTy(), 2));
    pointSizeVal = m_builder->CreateInsertElement(pointSizeVal, pointSize, m_builder->getInt32(0));
    pointSizeVal = m_builder->CreateInsertElement(pointSizeVal, pointSize, m_builder->getInt32(1));

    Value *scaledVal = m_builder->CreateFma(primCoord, pointSizeVal, negHalfPointSizeVal);
    Value *alphaScale = m_builder->CreateDotProduct(scaledVal, scaledVal);
    alphaScale = m_builder->CreateSqrt(alphaScale);
    alphaScale = m_builder->CreateFSub(halfPointSize, alphaScale);
    Value *discard = m_builder->CreateFCmpULT(alphaScale, ConstantFP::get(m_builder->getFloatTy(), 0));
    Instruction *InsertI = &*m_builder->GetInsertPoint();
    Instruction *thenInst = nullptr;
    Instruction *elseInst = nullptr;
    SplitBlockAndInsertIfThenElse(discard, InsertI, &thenInst, &elseInst);
    m_builder->SetInsertPoint(thenInst);
    m_builder->CreateKill();
    m_builder->SetInsertPoint(elseInst);
    alphaScale = m_builder->CreateFAdd(alphaScale, alphaBias);
    m_builder->CreateStore(alphaScale, alphaScaleVal);

    m_builder->SetInsertPoint(m_retInst);
    for (GlobalVariable &global : m_module->globals()) {
      auto addrSpace = global.getType()->getAddressSpace();
      if (addrSpace == SPIRAS_Output) {
        auto outputMetaVal = mdconst::extract<Constant>(global.getMetadata(gSPIRVMD::InOut)->getOperand(0));
        patchAlphaScaling(&global, global.getValueType(), outputMetaVal, alphaScaleVal);
      }
    }
  }
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_ClipVertex".
void LowerGlCompatibility::lowerClipVertex() {
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
void LowerGlCompatibility::lowerColor(llvm::User *color) {
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
void LowerGlCompatibility::lowerFrontColor() {
  lowerColor(m_frontColor);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_BackColor".
void LowerGlCompatibility::lowerBackColor() {
  lowerColor(m_backColor);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_FrontSecondaryColor".
void LowerGlCompatibility::lowerFrontSecondaryColor() {
  lowerColor(m_frontSecondaryColor);
}

// =====================================================================================================================
// Does lowering operations for GLSL variable "gl_BackSecondaryColor".
void LowerGlCompatibility::lowerBackSecondaryColor() {
  lowerColor(m_backSecondaryColor);
}

// =====================================================================================================================
// Does clamp fragment color
void LowerGlCompatibility::lowerFragColor() {
  lowerColor(m_fragColor);
}

// =====================================================================================================================
// Does lowering operations for alpha test.
void LowerGlCompatibility::lowerAlphaTest() {
  GlobalVariable *outputLocationZero = nullptr;
  auto floatTy = m_builder->getFloatTy();
  Type *vec4Type = VectorType::get(floatTy, 4, false);

  for (GlobalVariable &global : m_module->globals()) {
    if (global.getType()->getAddressSpace() == SPIRAS_Output) {
      ShaderInOutMetadata outputMeta = {};
      MDNode *metaNode = global.getMetadata(gSPIRVMD::InOut);
      auto *meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
      outputMeta.U64All[0] = cast<ConstantInt>(meta->getOperand(0))->getZExtValue();
      outputMeta.U64All[1] = cast<ConstantInt>(meta->getOperand(1))->getZExtValue();

      if (outputMeta.Value == 0) {
        outputLocationZero = &global;
        break;
      }
    }
  }

  if (outputLocationZero != nullptr && outputLocationZero->getValueType()->isVectorTy()) {
    auto type = cast<FixedVectorType>(outputLocationZero->getValueType());
    uint32_t vectorNum = type->getNumElements();
    if (vectorNum != 4 || !type->getElementType()->isFloatTy())
      return;
  } else
    return;

  auto buildInfo = static_cast<const GraphicsPipelineBuildInfo *>(m_context->getPipelineBuildInfo());
  auto predicate = CmpInst::Predicate::BAD_FCMP_PREDICATE;

  switch (buildInfo->glState.alphaTestFunc) {
  case Vkgc::AlphaTestFunc::Always: {
    // always pass, do nothing
    return;
  }
  case Vkgc::AlphaTestFunc::Never: {
    predicate = CmpInst::Predicate::FCMP_FALSE;
    break;
  }
  case Vkgc::AlphaTestFunc::Less: {
    predicate = CmpInst::Predicate::FCMP_OLT;
    break;
  }
  case Vkgc::AlphaTestFunc::LEqual: {
    predicate = CmpInst::Predicate::FCMP_OLE;
    break;
  }
  case Vkgc::AlphaTestFunc::Equal: {
    predicate = CmpInst::Predicate::FCMP_OEQ;
    break;
  }
  case Vkgc::AlphaTestFunc::GEqual: {
    predicate = CmpInst::Predicate::FCMP_OGE;
    break;
  }
  case Vkgc::AlphaTestFunc::Greater: {
    predicate = CmpInst::Predicate::FCMP_OGT;
    break;
  }
  case Vkgc::AlphaTestFunc::NotEqual: {
    predicate = CmpInst::Predicate::FCMP_ONE;
    break;
  }
  }

  m_builder->SetInsertPoint(m_retInst);
  auto lastBB = m_builder->GetInsertBlock();
  lastBB->splitBasicBlock(m_retInst);
  m_builder->SetInsertPoint(lastBB->getTerminator());

  // if the alpha test is never, then discard it
  if (predicate == llvm::CmpInst::Predicate::FCMP_FALSE) {
    // Always discard.
    m_builder->CreateKill();
    return;
  }

  // get mrt0.alpha
  Value *outputValue = m_builder->CreateLoad(vec4Type, outputLocationZero);
  Value *outputAlpha = m_builder->CreateExtractElement(outputValue, 3);

  // get alphaRef
  auto alphaRef = new GlobalVariable(*m_module, floatTy, false, GlobalValue::ExternalLinkage, nullptr, "alphaTestRef",
                                     nullptr, GlobalVariable::NotThreadLocal, SPIRV::SPIRAS_Uniform);
  auto locationFound =
      getUniformConstantEntryByLocation(m_context, m_shaderStage, Vkgc::GlCompatibilityUniformLocation::AlphaTestRef);
  assert(locationFound != nullptr);
  auto alphaTestBaseOffset = locationFound->offset;
  unsigned constBufferBinding =
      Vkgc::ConstantBuffer0Binding + static_cast<GraphicsContext *>(m_context->getPipelineContext())
                                         ->getPipelineShaderInfo(m_shaderStage)
                                         ->options.constantBufferBindingOffset;
  std::vector<Metadata *> mDs;
  auto int32Ty = Type::getInt32Ty(*m_context);
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, Vkgc::InternalDescriptorSetId)));
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, constBufferBinding)));
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, alphaTestBaseOffset)));
  mDs.push_back(ConstantAsMetadata::get(ConstantInt::get(int32Ty, Vkgc::GlCompatibilityUniformLocation::AlphaTestRef)));
  auto mdNode = MDNode::get(*m_context, mDs);
  alphaRef->addMetadata(gSPIRVMD::UniformConstant, *mdNode);

  Value *refValue = m_builder->CreateLoad(floatTy, alphaRef);

  // br %1, label %.AlphaTestDiscard, label %.AlphaTestPass
  //
  // .AlphaTestDiscard:
  // call void (...) @glc.create.kill()
  // br label %2
  //
  // .AlphaTestPass:
  // br label %2
  //
  // label %2:
  // br label %3
  //
  // label %3:
  // terminator
  auto *cond = dyn_cast<Instruction>(m_builder->CreateCmp(predicate, outputAlpha, refValue));

  auto *compBB = m_builder->GetInsertBlock();
  auto *exitBB = compBB->splitBasicBlock(cond->getParent()->getTerminator());
  auto discardBB = BasicBlock::Create(*m_context, ".AlphaTestDiscard", cond->getFunction(), exitBB);
  auto passBB = BasicBlock::Create(*m_context, ".AlphaTestPass", cond->getFunction(), exitBB);

  m_builder->SetInsertPoint(compBB);
  compBB->getTerminator()->eraseFromParent();
  m_builder->CreateCondBr(cond, passBB, discardBB);
  m_builder->SetInsertPoint(discardBB);

  m_builder->CreateKill();
  m_builder->CreateBr(exitBB);

  m_builder->SetInsertPoint(passBB);
  m_builder->CreateBr(exitBB);
}

} // namespace Llpc
