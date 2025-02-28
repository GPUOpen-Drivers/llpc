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
 * @file  LinkTransformShaders.cpp
 * @brief Link a prepared vertex shader into a transform compute shader.
 ***********************************************************************************************************************
 */
#include "LinkTransformShaders.h"
#include "LoweringUtil.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "compilerutils/CompilerUtils.h"
#include "lgc/Builder.h"
#include "llvm/Bitcode/BitcodeReader.h"

#define DEBUG_TYPE "link-transform-shader"

using namespace lgc;
using namespace llvm;
using namespace Llpc;

namespace Llpc {

static const char TransformVsEntry[] = "TransformVertexEntry";
static const char TransformVertex[] = "TransformVertexAmd";
static const char GetTransformVertexAttribute[] = "GetTransformVertexAttributeAmd";

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LinkTransformShaders::run(Module &module, ModuleAnalysisManager &analysisManager) {
  SpirvLower::init(&module);
  LLVM_DEBUG(dbgs() << "Run the pass Lower-transform-shader\n");
  processTransformCsFunctions(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// traverse the functions in the module, call the TransformVertexEntry to overwrite the predefined functions in
// compute shader
//
// @param [in/out] module : LLVM module to be run on
void LinkTransformShaders::processTransformCsFunctions(Module &module) {
  auto llpcContext = static_cast<Context *>(&module.getContext());
  auto computeContext = static_cast<ComputeContext *>(llpcContext->getPipelineContext());
  assert(computeContext != nullptr);

  // The bitcode of transform vertex shader is stored in computeContext, convert it to llvm IR before being linked
  std::unique_ptr<Module> vtxShaderModule;
  if (computeContext != nullptr) {
    auto vtxShaderStream = computeContext->getVtxShaderStream();
    MemoryBufferRef bcBufferRef(vtxShaderStream, "");
    Expected<std::unique_ptr<Module>> moduleOrErr = parseBitcodeFile(bcBufferRef, *llpcContext);
    if (!moduleOrErr)
      report_fatal_error("Failed to read bitcode");
    vtxShaderModule = std::move(*moduleOrErr);

    // After translate LLVM IR to bitcode, the module ID is disappeared, need to set it explicitly
    vtxShaderModule->setModuleIdentifier("transform-runtime");
  }

  // Link the gfxruntime library module
  auto *transformVsEntry = vtxShaderModule->getFunction(TransformVsEntry);
  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    auto funcName = func->getName();
    if (funcName.starts_with(GetTransformVertexAttribute)) {
      // Handle VS output for object selection and feedback, the function GetTransformVertexAttribute will be
      // overwritten All the VS built-in outputs will be required for selection and feedback
      processLibraryFunction(func, transformVsEntry, false);
    } else if (funcName.starts_with(TransformVertex)) {
      // Handle VS output for primitive culling, the function TransformVertexAmd will be overwritten, only gl_Position
      // will be required for primitive culling
      processLibraryFunction(func, transformVsEntry, true);
    }
  }
}

// =====================================================================================================================
// Call the shader library to overwrite the predefined function TransformVertexAmd or GetTransformVertexAttribute
//
// @param func : The function to process in transform compute shader
// @param transformVsFunc : The function to be called in transform vertex shader
// @param primCulling : Whether the function is called for primitive culling
void LinkTransformShaders::processLibraryFunction(Function *&func, Function *transformVsFunc, bool primCulling) {
  m_builder->SetInsertPoint(clearBlock(func));

  // Cross module inliner cannot be used to inline a function with multiple blocks into in a degenerate block, create
  // a temporary terminator first.
  auto tempTerminator = m_builder->CreateUnreachable();
  m_builder->SetInsertPoint(tempTerminator);

  SmallVector<Value *> args;
  auto int32Ty = m_builder->getInt32Ty();
  for (auto &arg : func->args()) {
    Value *v = m_builder->CreateLoad(int32Ty, &arg);
    args.push_back(v);
  }

  compilerutils::CrossModuleInliner inliner;
  auto *vsOutput = inliner.inlineCall(*m_builder, transformVsFunc, {args}).returnValue;

  if (primCulling) {
    // For primitive culling, return gl_Position
    m_builder->CreateRet(m_builder->CreateExtractValue(vsOutput, 0));
    tempTerminator->eraseFromParent();
  } else {
    // For selection and feedback, return all the required built-in outputs
    m_builder->CreateRet(vsOutput);
    tempTerminator->eraseFromParent();
  }
}

} // namespace Llpc
