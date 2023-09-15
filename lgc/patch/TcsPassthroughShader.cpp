/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  TcsPassthroughShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::TcsPassthroughShader.
 ***********************************************************************************************************************
 */
#include "lgc/patch/TcsPassthroughShader.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/Internal.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-tcs-passthrough-shader"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param module : LLVM module to be run on
// @param analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses TcsPassthroughShader::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass TCS pass-through shader\n");

  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();

  generateTcsPassthroughShader(module, pipelineShaders, pipelineState);
  updatePipelineState(module, pipelineState);

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Updates the the pipeline state with the data for the tessellation control pass-through shader.
//
// @param module : LLVM module to be run on
// @param pipelineState : The pipeline state read from module.
void TcsPassthroughShader::updatePipelineState(Module &module, PipelineState *pipelineState) const {
  pipelineState->setShaderStageMask(pipelineState->getShaderStageMask() | shaderStageToMask(ShaderStageTessControl));

  TessellationMode tessellationMode = pipelineState->getShaderModes()->getTessellationMode();
  tessellationMode.outputVertices = tessellationMode.inputVertices;
  pipelineState->setTessellationMode(module, ShaderStageTessControl, tessellationMode);
  pipelineState->readState(&module);

  ShaderOptions options = pipelineState->getShaderOptions(ShaderStageTessControl);
  options.hash[0] = (uint64_t)-1;
  options.hash[1] = (uint64_t)-1;
  pipelineState->setShaderOptions(ShaderStageTessControl, options);
}

// =====================================================================================================================
// Generate a new tcs pass-through shader.
//
// @param module : The LLVM module in which to add the shader.
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : The pipeline state read from module.
// @returns : the entry point for the TCS pass-through shader.
Function *TcsPassthroughShader::generateTcsPassthroughShader(Module &module, PipelineShadersResult &pipelineShaders,
                                                             PipelineState *pipelineState) {
  Function *entryPoint = generateTcsPassthroughEntryPoint(module, pipelineState);
  generateTcsPassthroughShaderBody(module, pipelineShaders, pipelineState, entryPoint);
  return entryPoint;
}

// =====================================================================================================================
// Generate a new entry point for a null fragment shader.
//
// @param module : The LLVM module in which to add the shader.
// @param pipelineState : The pipeline state read from module.
// @returns : The new entry point.
Function *TcsPassthroughShader::generateTcsPassthroughEntryPoint(Module &module, PipelineState *pipelineState) {
  FunctionType *entryPointTy = FunctionType::get(Type::getVoidTy(module.getContext()), ArrayRef<Type *>(), false);
  Function *entryPoint =
      Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::TcsPassthroughEntryPoint, &module);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(entryPoint, ShaderStageTessControl);
  entryPoint->setCallingConv(CallingConv::SPIR_FUNC);
  return entryPoint;
}

// =====================================================================================================================
// Generate the body of the null fragment shader.
//
// @param module : The LLVM module in which to add the shader.
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : The pipeline state read from module.
// @param entryPointName : the entry point for the TCS pass-through shader.
void TcsPassthroughShader::generateTcsPassthroughShaderBody(Module &module, PipelineShadersResult &pipelineShaders,
                                                            PipelineState *pipelineState, Function *entryPoint) {
  BasicBlock *block = BasicBlock::Create(entryPoint->getContext(), "", entryPoint);

  BuilderBase builder(module.getContext());
  builder.SetInsertPoint(block);

  ResourceUsage *tcsResourceUsage = pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  auto &tcsInputLocInfoMap = tcsResourceUsage->inOutUsage.inputLocInfoMap;
  auto &tcsOutputLocInfoMap = tcsResourceUsage->inOutUsage.outputLocInfoMap;
  auto &tcsBuiltInInfo = tcsResourceUsage->builtInUsage.tcs;

  // ---------------------------------------------------------------------------------------------
  // output control point
  SmallVector<Value *, 6> args;
  args.push_back(builder.getInt32(InvalidValue)); // BuiltIn
  args.push_back(builder.getInt32(InvalidValue)); // Index
  args.push_back(builder.getInt32(InvalidValue)); // Vertex index
  args.push_back(builder.getInt32(InvalidValue)); // Value to write
  std::string outputTessLevelInner = std::string(lgcName::OutputExportBuiltIn) + "TessLevelInner.i32.i32.i32.f32";
  std::string outputTessLevelOuter = std::string(lgcName::OutputExportBuiltIn) + "TessLevelOuter.i32.i32.i32.f32";

  args[0] = builder.getInt32(BuiltInTessLevelInner);
  args[1] = builder.getInt32(0);
  args[3] = builder.getFpConstant(builder.getFloatTy(), APFloat(pipelineState->getTessLevelInner(0)));
  builder.CreateNamedCall(outputTessLevelInner, builder.getVoidTy(), args, {}); // TessLevelInner0
  args[1] = builder.getInt32(1);
  args[3] = builder.getFpConstant(builder.getFloatTy(), APFloat(pipelineState->getTessLevelInner(1)));
  builder.CreateNamedCall(outputTessLevelInner, builder.getVoidTy(), args, {}); // TessLevelInner1

  args[0] = builder.getInt32(BuiltInTessLevelOuter);
  args[1] = builder.getInt32(0);
  args[3] = builder.getFpConstant(builder.getFloatTy(), APFloat(pipelineState->getTessLevelOuter(0)));
  builder.CreateNamedCall(outputTessLevelOuter, builder.getVoidTy(), args, {}); // TessLevelOuter0
  args[1] = builder.getInt32(1);
  args[3] = builder.getFpConstant(builder.getFloatTy(), APFloat(pipelineState->getTessLevelOuter(1)));
  builder.CreateNamedCall(outputTessLevelOuter, builder.getVoidTy(), args, {}); // TessLevelOuter1
  args[1] = builder.getInt32(2);
  args[3] = builder.getFpConstant(builder.getFloatTy(), APFloat(pipelineState->getTessLevelOuter(2)));
  builder.CreateNamedCall(outputTessLevelOuter, builder.getVoidTy(), args, {}); // TessLevelOuter2
  args[1] = builder.getInt32(3);
  args[3] = builder.getFpConstant(builder.getFloatTy(), APFloat(pipelineState->getTessLevelOuter(3)));
  builder.CreateNamedCall(outputTessLevelOuter, builder.getVoidTy(), args, {}); // TessLevelOuter3

  tcsBuiltInInfo.tessLevelInner = true;
  tcsBuiltInInfo.tessLevelOuter = true;

  // ---------------------------------------------------------------------------------------------
  // read built-in InvocationId
  args.clear();
  args.push_back(builder.getInt32(BuiltInInvocationId)); // built-in
  args.push_back(builder.getInt32(InvalidValue));        // index
  args.push_back(builder.getInt32(InvalidValue));        // vertex index
  std::string inputInvocationId = std::string(lgcName::InputImportBuiltIn) + "InvocationId.i32.i32.i32.i32";

  Value *invocationId = builder.CreateNamedCall(inputInvocationId, builder.getInt32Ty(), args,
                                                {Attribute::ReadOnly, Attribute::WillReturn});
  invocationId->setName(PipelineState::getBuiltInName(BuiltInInvocationId));

  tcsBuiltInInfo.invocationId = true;

  // ---------------------------------------------------------------------------------------------
  // copy vs generic output and built-in output to tcs output
  Function *vsEntryPoint = pipelineShaders.getEntryPoint(ShaderStageVertex);
  for (Function &func : *vsEntryPoint->getParent()) {
    if (func.getName().startswith(lgcName::OutputExportGeneric)) {
      for (auto user : func.users()) {
        CallInst *callInst = dyn_cast<CallInst>(user);
        if (!callInst || callInst->getParent()->getParent() != vsEntryPoint)
          continue;

        Value *elemIdx = callInst->getOperand(1);
        Value *vsOutput = callInst->getOperand(callInst->arg_size() - 1);
        uint32_t location = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();  // location
        uint32_t component = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue(); // component
        Type *vsOutputTy = vsOutput->getType();

        InOutLocationInfo origLocInfo;
        origLocInfo.setLocation(location);
        if (vsOutputTy->getScalarSizeInBits() == 64)
          component *= 2; // Component in location info is dword-based
        origLocInfo.setComponent(component);

        // CreateReadGenericInput
        Value *passThroughValue = builder.create<InputImportGenericOp>(vsOutputTy,
                                                                       false,                         // isPerPrimitive
                                                                       location, builder.getInt32(0), // locationOffset
                                                                       elemIdx, invocationId);

        // CreateWriteGenericOutput
        args.clear();
        args.push_back(builder.getInt32(location));
        args.push_back(builder.getInt32(0)); // locationOffset
        args.push_back(elemIdx);
        args.push_back(invocationId);
        args.push_back(passThroughValue);
        std::string llpcCallName = lgcName::OutputExportGeneric;
        addTypeMangling(nullptr, args, llpcCallName);
        builder.CreateNamedCall(llpcCallName, builder.getVoidTy(), args, {});

        // markGenericInputOutputUsage
        tcsInputLocInfoMap[origLocInfo].setData(InvalidValue);
        tcsOutputLocInfoMap[origLocInfo].setData(InvalidValue);
      }
    } else if (func.getName().startswith(lgcName::OutputExportBuiltIn)) {
      for (auto user : func.users()) {
        CallInst *callInst = dyn_cast<CallInst>(user);
        if (!callInst || callInst->getParent()->getParent() != vsEntryPoint)
          continue;

        BuiltInKind builtIn = static_cast<BuiltInKind>(cast<ConstantInt>(callInst->getOperand(0))->getZExtValue());
        Value *vsOutput = callInst->getOperand(callInst->arg_size() - 1);
        Type *vsOutputTy = vsOutput->getType();
        unsigned arraySize = 0;
        if (vsOutputTy->isArrayTy()) {
          ArrayType *arrayTy = cast<ArrayType>(vsOutputTy);
          arraySize = arrayTy->getNumElements();
        }

        // CreateReadBuiltInInput
        args.clear();
        args.push_back(builder.getInt32(builtIn));
        args.push_back(builder.getInt32(InvalidValue));
        args.push_back(invocationId);

        std::string inputCallName = lgcName::InputImportBuiltIn;
        inputCallName += PipelineState::getBuiltInName(builtIn);
        addTypeMangling(vsOutputTy, args, inputCallName);
        Value *passThroughValue =
            builder.CreateNamedCall(inputCallName, vsOutputTy, args, {Attribute::ReadOnly, Attribute::WillReturn});
        passThroughValue->setName(PipelineState::getBuiltInName(builtIn));

        // CreateWriteBuiltInOutput
        args.clear();
        args.push_back(builder.getInt32(builtIn));
        args.push_back(builder.getInt32(InvalidValue));
        args.push_back(invocationId);
        args.push_back(passThroughValue);

        std::string outputCallName = lgcName::OutputExportBuiltIn;
        outputCallName += PipelineState::getBuiltInName(builtIn);
        addTypeMangling(nullptr, args, outputCallName);
        builder.CreateNamedCall(outputCallName, builder.getVoidTy(), args, {});

        // markBuiltInInputUsage
        switch (builtIn) {
        case BuiltInPointSize:
          tcsBuiltInInfo.pointSizeIn = true;
          break;
        case BuiltInPosition:
          tcsBuiltInInfo.positionIn = true;
          break;
        case BuiltInClipDistance:
          tcsBuiltInInfo.clipDistanceIn = arraySize;
          break;
        case BuiltInCullDistance:
          tcsBuiltInInfo.cullDistanceIn = arraySize;
          break;
        case BuiltInPatchVertices:
          tcsBuiltInInfo.patchVertices = true;
          break;
        case BuiltInPrimitiveId:
          tcsBuiltInInfo.primitiveId = true;
          break;
        case BuiltInInvocationId:
          tcsBuiltInInfo.invocationId = true;
          break;
        case BuiltInViewIndex:
          tcsBuiltInInfo.viewIndex = true;
          break;
        default:
          break;
        }
      }
    }
  }

  // ---------------------------------------------------------------------------------------------
  builder.CreateRetVoid();
}
