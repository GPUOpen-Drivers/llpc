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
* @file  PrepareTransformVertexShader.cpp
* @brief Prepare a vertex shader for linking into a transform compute shader.
***********************************************************************************************************************
*/
#include "PrepareTransformVertexShader.h"
#include "LoweringUtil.h"
#include "llpcContext.h"
#include "compilerutils/CompilerUtils.h"
#include "lgc/Builder.h"
#include "llvm/IR/Instructions.h"

using namespace lgc;
using namespace llvm;
using namespace compilerutils;

namespace Llpc {
#define DEBUG_TYPE "prepare-transform-shader"
static const char TransformVsEntry[] = "TransformVertexEntry";

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses PrepareTransformVertexShader::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Prepare-transform-vertexShader\n");

  Lowering::init(&module);
  collectVtxBuiltInSymbols(module);

  Function *func = module.getFunction("main");
  if (func != nullptr)
    genFunTransformVertex(*func);

  return PreservedAnalyses::none();
}

// Collect Vertex shader builtIn inputs and outputs
//
// @param [in/out] module : LLVM module to be run on
void PrepareTransformVertexShader::collectVtxBuiltInSymbols(Module &module) {
  for (auto &global : module.globals()) {
    auto type = global.getType();

    MDNode *metaNode = global.getMetadata(gSPIRVMD::InOut);
    if (!metaNode)
      continue;

    auto inOutMetaConst = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));
    if (type->getAddressSpace() == SPIRAS_Output) {
      auto valueType = global.getValueType();
      SmallVector<ShaderInOutMetadata> mds;
      decodeInOutMetaRecursively(valueType, inOutMetaConst, mds);

      for (auto md : mds) {
        if (md.IsBuiltIn) {
          if (md.Value == spv::BuiltInPosition) {
            m_outputBuiltIns[Position] = &global;
          } else if (md.Value == spv::BuiltInClipDistance) {
            m_outputBuiltIns[ClipDistance0] = &global;
            m_outputBuiltIns[ClipDistance1] = &global;
          }
        } else {
          if (md.IsLoc) {
            if (md.Value == Vkgc::GlCompatibilityInOutLocation::FrontColor) {
              m_outputBuiltIns[FrontColor] = &global;
            } else if (md.Value == Vkgc::GlCompatibilityInOutLocation::TexCoord) {
              m_outputBuiltIns[TexCoord] = &global;
            }
          }
        }
      }
    } else if (type->getAddressSpace() == SPIRAS_Input) {
      ShaderInOutMetadata inputMeta = {};
      Constant *metaValConst = cast<Constant>(inOutMetaConst);
      inputMeta.U64All[0] = cast<ConstantInt>(metaValConst->getOperand(0))->getZExtValue();
      inputMeta.U64All[1] = cast<ConstantInt>(metaValConst->getOperand(1))->getZExtValue();

      if (inputMeta.IsBuiltIn) {
        auto builtIn = static_cast<lgc::BuiltInKind>(inputMeta.Value);
        if (builtIn == lgc::BuiltInVertexIndex)
          m_inputBuiltIns[VertexIndex] = &global;
        else if (builtIn == lgc::BuiltInInstanceId)
          m_inputBuiltIns[InstanceIndex] = &global;
        else if (builtIn == lgc::BuiltInDrawIndex)
          m_inputBuiltIns[DrawID] = &global;
        else if (builtIn == lgc::BuiltInBaseVertex)
          m_inputBuiltIns[BaseVertex] = &global;
        else if (builtIn == lgc::BuiltInBaseInstance)
          m_inputBuiltIns[BaseInstance] = &global;
        else
          assert("unreachable!");
      }
    }
  }
}

// =====================================================================================================================
// Load the clip distance component from structure member
//
// @param [in] index : Indicates which variable is being accessed, TransformClipDistance0 or TransformClipDistance1
// @param [in] component : Indicates which component of the clip distance to load
// @returns Value of the component
Value *PrepareTransformVertexShader::loadClipDistanceComponent(unsigned index, unsigned component) {
  auto clipDistance = cast<GlobalVariable>(m_outputBuiltIns[index]);
  Type *ty = clipDistance->getValueType();
  auto arraySize = ty->getArrayNumElements();
  auto floatType = m_builder->getFloatTy();

  unsigned redirectIdx = (index == ClipDistance1) ? component + 4 : component;
  if (redirectIdx < arraySize) {
    return m_builder->CreateLoad(floatType, m_builder->CreateConstGEP2_32(ty, clipDistance, 0, redirectIdx));
  } else {
    return ConstantFP::get(floatType, 1.0);
  }
}

// =====================================================================================================================
// Generate transform vertex shader: TransformVertexEntry
//
// @param [in] function : The main function of the original vertex shader
void PrepareTransformVertexShader::genFunTransformVertex(Function &function) {
  // 1. Create a structure to store vs output: gl_Position, gl_ClipDistance[0~3], gl_ClipDistance[4~7],
  // gl_FrontColor and gl_TexCoord
  auto floatType = m_builder->getFloatTy();
  Type *vec4Type = VectorType::get(floatType, 4, false);
  auto structTy = StructType::get(*m_context, {vec4Type, vec4Type, vec4Type, vec4Type, vec4Type});
  Value *vsOutput = PoisonValue::get(structTy);

  // 2. Handle early returns
  m_unifiedReturn = compilerutils::unifyReturns(function, *m_builder);
  m_builder->SetInsertPoint(m_unifiedReturn);

  // 3. Store gl_Position, gl_ClipDistance, gl_FrontColor and gl_TextureCoord[0] in the struct
  // If any of these variables are not existed, set them to the default value vec4(1.0f)
  auto floatOne = ConstantFP::get(floatType, 1.0);
  auto vecOne = ConstantVector::get({floatOne, floatOne, floatOne, floatOne});

  for (unsigned idx = 0; idx < OutputCount; idx++) {
    Value *memberValue;
    if (m_outputBuiltIns[idx] != nullptr) {
      // gl_ClipDistance need to be handled specially
      if (idx == ClipDistance0 || idx == ClipDistance1) {
        memberValue = PoisonValue::get(vec4Type);
        for (unsigned i = 0; i < 4; i++) {
          Value *clipValue = loadClipDistanceComponent(idx, i);
          memberValue = m_builder->CreateInsertElement(memberValue, clipValue, i);
        }
      } else {
        memberValue = m_builder->CreateLoad(vec4Type, m_outputBuiltIns[idx]);
      }
    } else {
      memberValue = vecOne;
    }
    vsOutput = m_builder->CreateInsertValue(vsOutput, memberValue, idx);
  }

  // 4. Remove the instruction of "return void", insert the instruction for the new return
  m_builder->CreateRet(vsOutput);
  m_unifiedReturn->eraseFromParent();

  // 5. Create a new function as following
  //  { <4 x float>, <4 x float>, <4 x float>, <4 x float>, <4 x float> }
  //  @transform_vs_entry(i32 %vertexId, i32 %InstanceId, i32 %drawId, i32 %baseVertex, i32 %baseInstance)
  auto int32Ty = m_builder->getInt32Ty();
  SmallVector<Type *, InputCount> allArgTys = {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty};
  Function *transformVertexFunc = mutateFunctionArguments(function, structTy, allArgTys, function.getAttributes());
  transformVertexFunc->setName(TransformVsEntry);

  // 6. Transfer function body from old function to new function.
  while (!function.empty()) {
    BasicBlock *block = &function.front();
    block->removeFromParent();
    block->insertInto(transformVertexFunc);
  }

  // 7. If builtin inputs such as gl_VertexID, gl_InstanceID are used in the shader, replace them by function params
  SmallVector<LoadInst *> loadInsts;
  for (unsigned i = 0; i < InputCount; i++) {
    if (m_inputBuiltIns[i] != nullptr) {
      auto builtInInput = cast<GlobalVariable>(m_inputBuiltIns[i]);
      Value *param = transformVertexFunc->getArg(i);
      for (auto user : builtInInput->users()) {
        if (auto *loadInst = dyn_cast<LoadInst>(user)) {
          loadInst->replaceAllUsesWith(param);
          loadInsts.push_back(loadInst);
        }
      }
    }
  }

  // Remove the load instruction
  for (auto &loadInst : loadInsts) {
    loadInst->eraseFromParent();
  }

  // 8. Remove the old main function and its metadata
  function.dropAllReferences();
  function.getParent()->getFunctionList().remove(&function);
}
} // namespace Llpc
