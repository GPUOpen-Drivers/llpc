/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  InitializeUndefInputs.h
 * @brief LLPC header file: contains declaration of class lgc::InitializeUndefInputs.
 ***********************************************************************************************************************
 */
#include "lgc/lowering/InitializeUndefInputs.h"
#include "compilerutils/CompilerUtils.h"
#include "lgc/LgcDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"

#define DEBUG_TYPE "lgc-initialize-undef-inputs"
using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses InitializeUndefInputs::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);

  LLVM_DEBUG(dbgs() << "Run the pass Initialize-Undef-variables \n");
  LgcLowering::init(&module);

  m_pipelineState = pipelineState;
  m_pipelineShaders = &pipelineShaders;

  // This pass works on graphic pipelines
  if (m_pipelineState->hasShaderStage(ShaderStage::Compute))
    return PreservedAnalyses::all();

  if (!m_pipelineState->getOptions().enableInitUndefZero)
    return PreservedAnalyses::all();

  if (getUndefinedInputs(&module))
    setUndefinedInputsToZero(&module);

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Analyze the shader resource usage info to check whether has undefined input variables
//
// @param [in] module : LLVM module to be run on
// @returns : True if there are undefined output variables
bool InitializeUndefInputs::getUndefinedInputs(llvm::Module *module) {
  bool hasUndefVariables = false;

  auto curStage = ShaderStage::Fragment;
  auto preStage = m_pipelineState->getPrevShaderStage(curStage);

  // Lambda to collect location and minimum component
  auto collectOutputSymbolInfo = [](const auto &preOutLocInfoMap, std::map<unsigned, unsigned> &locCompMap) {
    // Iterate over the map and collect locations
    for (const auto &entry : preOutLocInfoMap) {
      unsigned location = entry.first.getLocation();
      unsigned component = entry.first.getComponent();

      // Update the locCompMap with the minimum component
      locCompMap[location] = locCompMap.contains(location) ? std::min(locCompMap[location], component) : component;
    }
  };

  // For OGL point sprite, if the the FS input gl_TexCoord[i] is not exported from VS output, the value of
  // gl_TexCoord[i] will be identical to point coordinate, for such case, don't consider gl_TexCoord[i] as
  // uninitialized

  // Lambda to check if the current location is a texture coordinate and whether it will be replaced by gl_PointCoord
  auto replaceTextureCoord = [&](unsigned location) -> bool {
    for (unsigned i = 0; i < m_pipelineState->getOptions().numTexPointSprite; i++) {
      // Check if the current location matches the texture coordinate location
      if (m_pipelineState->getOptions().texPointSpriteLocs[i] == location)
        return true;
    }
    return false;
  };

  while (preStage != std::nullopt) {
    auto &curInOutUsage = m_pipelineState->getShaderResourceUsage(curStage)->inOutUsage;
    auto &preInOutUsage = m_pipelineState->getShaderResourceUsage(preStage.value())->inOutUsage;
    auto &curInLocInfoMap = curInOutUsage.inputLocInfoMap;
    auto &preOutLocInfoMap = preInOutUsage.outputLocInfoMap;

    std::map<unsigned, unsigned> outputLocCompMap;
    collectOutputSymbolInfo(preOutLocInfoMap, outputLocCompMap);

    // If a symbol's location and component can be found in current shader's input, but not found from the prev-shader's
    // output,we can confirm the input symbol is uninitiaized.
    for (const auto &inLoc : curInLocInfoMap) {
      unsigned location = inLoc.first.getLocation();
      unsigned component = inLoc.first.getComponent();

      if (curStage == ShaderStage::Fragment && replaceTextureCoord(location)) {
        continue;
      }

      auto iter = outputLocCompMap.find(location);
      if (iter == outputLocCompMap.end() || (iter->second > component)) {
        LocCompInfo locCompInfo;
        locCompInfo.u32All = 0;
        locCompInfo.location = location;
        locCompInfo.component = component;

        m_undefInputs[curStage].insert(locCompInfo.u32All);
        hasUndefVariables = true;
      }
    }

    // Check each stage whether all the input variables have been initialized
    curStage = preStage.value();
    preStage = m_pipelineState->getPrevShaderStage(curStage);
  }

  return hasUndefVariables;
}

// =====================================================================================================================
// Set all undefined inputs to Zero
//
// @param [in] module : LLVM module to be run on
void InitializeUndefInputs::setUndefinedInputsToZero(llvm::Module *module) {
  SmallVector<GenericLocationOp *> undefInputCalls;

  struct Payload {
    InitializeUndefInputs *self;
    SmallVectorImpl<GenericLocationOp *> &inputCalls;
  };
  Payload payload = {this, undefInputCalls};

  static auto visitInput = [](Payload &payload, GenericLocationOp &input) {
    auto shaderStage = payload.self->m_pipelineShaders->getShaderStage(input.getFunction());
    auto &undefLocs = payload.self->m_undefInputs[shaderStage.value()];
    unsigned location = input.getLocation();
    unsigned component = unsigned(-1);
    Value *elemIdx = input.getElemIdx();
    if (isa<ConstantInt>(elemIdx)) {
      component = cast<ConstantInt>(elemIdx)->getZExtValue();
      LocCompInfo locCompInfo;
      locCompInfo.u32All = 0;
      locCompInfo.location = location;
      locCompInfo.component = component;

      if (!undefLocs.empty() && undefLocs.find(locCompInfo.u32All) != undefLocs.end())
        payload.inputCalls.push_back(&input);
    }
  };

  static auto visitor =
      llvm_dialects::VisitorBuilder<Payload>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .addSet<InputImportGenericOp, InputImportInterpolatedOp>(
              [](Payload &payload, Instruction &op) { visitInput(payload, cast<GenericLocationOp>(op)); })
          .build();

  // Visit InputImportGenericOp and InputImportInterpolatedOp to collect all input calls which have undefined value
  visitor.visit(payload, *module);

  for (CallInst *call : undefInputCalls) {
    // Create a zero value of the appropriate type
    Type *returnType = call->getType();
    Constant *zeroValue = Constant::getNullValue(returnType);

    // Replace all uses of the call with zero
    call->replaceAllUsesWith(zeroValue);
    call->eraseFromParent();
  }
}

} // namespace lgc
