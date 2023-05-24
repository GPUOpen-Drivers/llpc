/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchCopyShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchCopyShader.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchCopyShader.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <set>

#define DEBUG_TYPE "lgc-patch-copy-shader"

namespace llvm {
namespace cl {

extern opt<bool> InRegEsGsLdsSize;

} // namespace cl
} // namespace llvm

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchCopyShader::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  if (runImpl(module, pipelineShaders, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchCopyShader::runImpl(Module &module, PipelineShadersResult &pipelineShaders, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Copy-Shader\n");

  Patch::init(&module);

  m_pipelineState = pipelineState;
  m_pipelineSysValues.initialize(m_pipelineState);

  auto gsEntryPoint = pipelineShaders.getEntryPoint(ShaderStageGeometry);
  if (!gsEntryPoint) {
    // Skip copy shader generation if GS is absent
    return false;
  }

  // Tell pipeline state there is a copy shader.
  m_pipelineState->setShaderStageMask(m_pipelineState->getShaderStageMask() | (1U << ShaderStageCopyShader));

  // Gather GS generic export details.
  collectGsGenericOutputInfo(gsEntryPoint);

  BuilderBase builder(*m_context);

  auto int32Ty = Type::getInt32Ty(*m_context);
  SmallVector<Type *, 16> argTys;
  SmallVector<bool, 16> argInReg;
  SmallVector<const char *, 16> argNames;

  const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  if (!m_pipelineState->getNggControl()->enableNgg) {
    // Create type of new function with fixed HW layout:
    //
    //   void copyShader(
    //     i32 inreg globalTable,
    //     i32 inreg perShaderTable,
    //     i32 inreg streamOutTable (GFX6-GFX8) / esGsLdsSize (GFX9+),
    //     i32 inreg esGsLdsSize (GFX6-GFX8) / streamOutTable (GFX9+),
    //     i32 inreg streamOutInfo,
    //     i32 inreg streamOutWriteIndex,
    //     i32 inreg streamOutOffset0,
    //     i32 inreg streamOutOffset1,
    //     i32 inreg streamOutOffset2,
    //     i32 inreg streamOutOffset3,
    //     i32 vertexOffset)
    //
    argTys = {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty};
    argInReg = {true, true, true, true, true, true, true, true, true, true, false};
    argNames = {"globalTable",
                "perShaderTable",
                gfxIp.major <= 8 ? "streamOutTable" : "esGsLdsSize",
                gfxIp.major <= 8 ? "esGsLdsSize" : "streamOutTable",
                "streamOutInfo",
                "streamOutWriteIndex",
                "streamOutOffset0",
                "streamOutOffset1",
                "streamOutOffset2",
                "streamOutOffset3",
                "vertexOffset"};
  } else {
    // If NGG, the copy shader is not a real HW VS and will be incorporated into NGG primitive shader finally. Thus,
    // the argument definitions are decided by compiler not by HW. We could have such variable layout (not fixed with
    // GPU generation evolvement):
    //
    // GFX10:
    //   void copyShader(
    //     i32 vertexIndex)
    //
    // GFX11+:
    //   void copyShader(
    //     i32 inreg globalTable,
    //     i32 vertexId)
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 10) {
      argTys = {int32Ty};
      argInReg = {false};
      argNames = {"vertexId"};
    } else {
      argTys = {int32Ty, int32Ty};
      argInReg = {true, false};
      argNames = {"globalTable", "vertexId"};
    }
  }

  auto entryPointTy = FunctionType::get(builder.getVoidTy(), argTys, false);

  // Create function for the copy shader entrypoint, and insert it before the FS (if there is one).
  auto entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::CopyShaderEntryPoint);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  entryPoint->setCallingConv(CallingConv::AMDGPU_VS);

  // Set the shader stage on the new function.
  setShaderStage(entryPoint, ShaderStageCopyShader);

  auto insertPos = module.getFunctionList().end();
  auto fsEntryPoint = pipelineShaders.getEntryPoint(ShaderStageFragment);
  if (fsEntryPoint)
    insertPos = fsEntryPoint->getIterator();
  module.getFunctionList().insert(insertPos, entryPoint);

  // Make the args "inreg" (passed in SGPR) as appropriate and make their names.
  for (unsigned i = 0; i < entryPoint->arg_size(); ++i) {
    if (argInReg[i])
      entryPoint->getArg(i)->addAttr(Attribute::InReg);
    entryPoint->getArg(i)->setName(argNames[i]);
  }

  // Create ending basic block, and terminate it with return.
  auto endBlock = BasicBlock::Create(*m_context, "", entryPoint, nullptr);
  builder.SetInsertPoint(endBlock);
  builder.CreateRetVoid();

  // Create entry basic block
  auto entryBlock = BasicBlock::Create(*m_context, "", entryPoint, endBlock);
  builder.SetInsertPoint(entryBlock);

  auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageCopyShader);

  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 8) {
    // For GFX6 ~ GFX8, streamOutTable SGPR index value should be less than esGsLdsSize
    intfData->userDataUsage.gs.copyShaderEsGsLdsSize = 3;
    intfData->userDataUsage.gs.copyShaderStreamOutTable = 2;
  } else {
    if (!m_pipelineState->getNggControl()->enableNgg) {
      // For GFX9+, streamOutTable SGPR index value should be greater than esGsLdsSize
      intfData->userDataUsage.gs.copyShaderEsGsLdsSize = 2;
      intfData->userDataUsage.gs.copyShaderStreamOutTable = 3;
    } else {
      // If NGG, both esGsLdsSize and streamOutTable are not used
      intfData->userDataUsage.gs.copyShaderEsGsLdsSize = InvalidValue;
      intfData->userDataUsage.gs.copyShaderStreamOutTable = InvalidValue;
    }
  }

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);

  if (!m_pipelineState->getNggControl()->enableNgg) {
    // If no NGG, the copy shader will become a real HW VS. Set the user data entries in the
    // PAL metadata here.
    m_pipelineState->getPalMetadata()->setUserDataEntry(ShaderStageCopyShader, 0, UserDataMapping::GlobalTable);
    if (m_pipelineState->enableXfb()) {
      m_pipelineState->getPalMetadata()->setUserDataEntry(
          ShaderStageCopyShader, intfData->userDataUsage.gs.copyShaderStreamOutTable, UserDataMapping::StreamOutTable);
    }
    if (cl::InRegEsGsLdsSize && m_pipelineState->isGsOnChip()) {
      m_pipelineState->getPalMetadata()->setUserDataEntry(
          ShaderStageCopyShader, intfData->userDataUsage.gs.copyShaderEsGsLdsSize, UserDataMapping::EsGsLdsSize);
    }
  }

  if (m_pipelineState->isGsOnChip())
    m_lds = Patch::getLdsVariable(m_pipelineState, &module);

  unsigned outputStreamCount = 0;
  unsigned outputStreamId = InvalidValue;
  for (int i = 0; i < MaxGsStreams; ++i) {
    if (resUsage->inOutUsage.gs.outLocCount[i] > 0) {
      outputStreamCount++;
      if (outputStreamId == InvalidValue)
        outputStreamId = i;
    }
  }

  if (outputStreamCount > 1 && m_pipelineState->enableXfb()) {
    if (!m_pipelineState->getNggControl()->enableNgg) {
      // StreamId = streamInfo[25:24]
      auto streamInfo = getFunctionArgument(entryPoint, CopyShaderUserSgprIdxStreamInfo);

      Value *streamId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                                {
                                                    streamInfo,
                                                    builder.getInt32(24),
                                                    builder.getInt32(2),
                                                });
      //
      // copyShader() {
      //   ...
      //   switch(streamId) {
      //   case 0:
      //     export outputs of stream 0
      //     break
      //   ...
      //   case rasterStream:
      //     export outputs of raster stream
      //     break
      //   ...
      //   case 3:
      //     export outputs of stream 3
      //     break
      //   }
      //
      //   return
      // }
      //

      // Add switchInst to entry block
      auto switchInst = builder.CreateSwitch(streamId, endBlock, outputStreamCount);

      for (unsigned streamId = 0; streamId < MaxGsStreams; ++streamId) {
        if (resUsage->inOutUsage.gs.outLocCount[streamId] > 0) {
          std::string blockName = ".stream" + std::to_string(streamId);
          BasicBlock *streamBlock = BasicBlock::Create(*m_context, blockName, entryPoint, endBlock);
          builder.SetInsertPoint(streamBlock);

          switchInst->addCase(builder.getInt32(streamId), streamBlock);

          exportOutput(streamId, builder);
          builder.CreateBr(endBlock);
        }
      }
    } else {
      // NOTE: If NGG, the copy shader with stream-out is not a real HW VS and will be incorporated into NGG
      // primitive shader later. Therefore, there is no multiple HW executions.

      //
      // copyShader() {
      //   ...
      //   export outputs of stream 0
      //   ...
      //   export outputs of raster stream
      //   ...
      //   export outputs of stream 3
      //
      //   return
      // }
      //
      assert(gfxIp.major >= 11); // Must be GFX11+

      for (unsigned streamId = 0; streamId < MaxGsStreams; ++streamId) {
        if (resUsage->inOutUsage.gs.outLocCount[streamId] > 0)
          exportOutput(streamId, builder);
      }
      builder.CreateBr(endBlock);
    }
  } else {
    outputStreamId = outputStreamCount == 0 ? 0 : outputStreamId;
    exportOutput(outputStreamId, builder);
    builder.CreateBr(endBlock);
  }

  return true;
}

// =====================================================================================================================
// Collects info for GS generic outputs.
//
// @param gsEntryPoint : Geometry shader entrypoint
void PatchCopyShader::collectGsGenericOutputInfo(Function *gsEntryPoint) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);
  const auto &outputLocInfoMap = resUsage->inOutUsage.outputLocInfoMap;
  std::set<InOutLocationInfo> visitedLocInfos;

  // Collect the byte sizes of the output value at each mapped location
  for (auto &func : *gsEntryPoint->getParent()) {
    if (func.getName().startswith(lgcName::OutputExportGeneric)) {
      for (auto user : func.users()) {
        auto callInst = dyn_cast<CallInst>(user);
        if (!callInst || callInst->getParent()->getParent() != gsEntryPoint)
          continue;

        assert(callInst->arg_size() == 4);
        Value *output = callInst->getOperand(callInst->arg_size() - 1); // Last argument
        auto outputTy = output->getType();

        InOutLocationInfo origLocInfo;
        origLocInfo.setLocation(cast<ConstantInt>(callInst->getOperand(0))->getZExtValue());
        unsigned component = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue();
        if (outputTy->getScalarSizeInBits() == 64)
          component *= 2; // Component in location info is dword-based
        origLocInfo.setComponent(component);
        origLocInfo.setStreamId(cast<ConstantInt>(callInst->getOperand(2))->getZExtValue());

        const auto locInfoMapIt = outputLocInfoMap.find(origLocInfo);
        if (locInfoMapIt == outputLocInfoMap.end() || visitedLocInfos.count(origLocInfo) > 0)
          continue;
        visitedLocInfos.insert(origLocInfo);

        unsigned dwordSize = 1; // Each output call is scalarized and exports 1 dword for packing
        if (!m_pipelineState->canPackOutput(ShaderStageGeometry)) {
          unsigned compCount = 1;
          auto compTy = outputTy;
          auto outputVecTy = dyn_cast<FixedVectorType>(outputTy);
          if (outputVecTy) {
            compCount = outputVecTy->getNumElements();
            compTy = outputVecTy->getElementType();
          }

          unsigned bitWidth = compTy->getScalarSizeInBits();
          // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend
          // byte/word to dword and store dword to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size
          // is based on number of dwords.
          bitWidth = std::max(32u, bitWidth);
          dwordSize = bitWidth / 32 * compCount;
        }

        const unsigned streamId = origLocInfo.getStreamId();
        const unsigned newLoc = locInfoMapIt->second.getLocation();
        const unsigned newComp = locInfoMapIt->second.getComponent();
        if (dwordSize > 4) {
          assert(dwordSize <= 8); // <8 x dword> at most
          m_outputLocCompSizeMap[streamId][newLoc][newComp] = 4;
          m_outputLocCompSizeMap[streamId][newLoc + 1][newComp] = dwordSize - 4;
        } else {
          m_outputLocCompSizeMap[streamId][newLoc][newComp] = dwordSize;
        }
      }
    }
  }
}

// =====================================================================================================================
// Exports outputs of geometry shader, inserting buffer-load/output-export calls.
//
// @param streamId : Export output of this stream
// @param builder : BuilderBase to use for instruction constructing
void PatchCopyShader::exportOutput(unsigned streamId, BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);
  auto &builtInUsage = resUsage->builtInUsage.gs;
  auto &locInfoXfbOutInfoMap = resUsage->inOutUsage.locInfoXfbOutInfoMap;
  auto &outputLocInfoMap = resUsage->inOutUsage.outputLocInfoMap;

  // The output value at the new mapped <location, component>
  DenseMap<unsigned, DenseMap<unsigned, Value *>> outputLocCompValue;
  for (auto &locCompSizeInfo : m_outputLocCompSizeMap[streamId]) {
    const unsigned newLoc = locCompSizeInfo.first;
    for (auto &compSizeInfo : locCompSizeInfo.second) {
      const unsigned newComp = compSizeInfo.first;
      const unsigned dwordSize = compSizeInfo.second;
      Value *outputValue = loadValueFromGsVsRing(dwordSize > 1 ? FixedVectorType::get(builder.getFloatTy(), dwordSize)
                                                               : builder.getFloatTy(),
                                                 newLoc, newComp, streamId, builder);
      outputLocCompValue[newLoc][newComp] = outputValue;
    }
  }

  if (m_pipelineState->enableXfb()) {
    // Export XFB output
    for (const auto &locInfoXfbInfoPair : locInfoXfbOutInfoMap) {
      const InOutLocationInfo &origLocInfo = locInfoXfbInfoPair.first;
      if (origLocInfo.getStreamId() != streamId || origLocInfo.isBuiltIn())
        continue;

      assert(outputLocInfoMap.count(origLocInfo) > 0);
      auto &newLocInfo = outputLocInfoMap[origLocInfo];

      const unsigned newLoc = newLocInfo.getLocation();
      const unsigned newComp = newLocInfo.getComponent();

      if (outputLocCompValue[newLoc].count(newComp) > 0) {
        // NOTE: If the XFB output value exists, we export it. Otherwise, the XFB output value is not written by API
        // shader and we can skip its export.
        Value *xfbOutputValue = outputLocCompValue[newLoc][newComp];
        assert(locInfoXfbOutInfoMap.count(origLocInfo) > 0);
        auto &xfbInfo = locInfoXfbOutInfoMap[origLocInfo];
        exportXfbOutput(xfbOutputValue, xfbInfo, builder);
      }
    }
  }

  // Following non-XFB output exports are only for rasterization stream
  if (resUsage->inOutUsage.gs.rasterStream != streamId)
    return;

  // Reconstruct output value at the new mapped location
  DenseMap<unsigned, Value *> outputLocValueMap;
  for (auto &locCompSizeInfo : m_outputLocCompSizeMap[streamId]) {
    const unsigned newLoc = locCompSizeInfo.first;

    // Count dword size of each component at the same location
    unsigned locDwordSize = 0;
    for (auto &compSizeInfo : locCompSizeInfo.second) {
      const unsigned newComp = compSizeInfo.first;
      const unsigned dwordSize = compSizeInfo.second;
      locDwordSize = std::max(locDwordSize, newComp + dwordSize);
    }

    // Reconstruct the output value from each component
    Value *outputValue = UndefValue::get(FixedVectorType::get(builder.getFloatTy(), locDwordSize));
    for (auto &compValueInfo : outputLocCompValue[newLoc]) {
      const unsigned newComp = compValueInfo.first;
      auto compValue = compValueInfo.second;
      if (compValue->getType()->isVectorTy()) {
        for (unsigned i = 0; i < cast<FixedVectorType>(compValue->getType())->getNumElements(); ++i) {
          outputValue =
              builder.CreateInsertElement(outputValue, builder.CreateExtractElement(compValue, i), newComp + i);
        }
      } else {
        outputValue = builder.CreateInsertElement(outputValue, compValue, newComp);
      }
    }

    outputLocValueMap[newLoc] = outputValue;
  }

  for (const auto &locValueInfo : outputLocValueMap)
    exportGenericOutput(locValueInfo.second, locValueInfo.first, builder);

  // Export built-in outputs
  std::vector<std::pair<BuiltInKind, Type *>> builtInPairs;

  if (builtInUsage.position)
    builtInPairs.push_back(std::make_pair(BuiltInPosition, FixedVectorType::get(builder.getFloatTy(), 4)));

  if (builtInUsage.pointSize)
    builtInPairs.push_back(std::make_pair(BuiltInPointSize, builder.getFloatTy()));

  if (builtInUsage.clipDistance > 0) {
    builtInPairs.push_back(
        std::make_pair(BuiltInClipDistance, ArrayType::get(builder.getFloatTy(), builtInUsage.clipDistance)));
  }

  if (builtInUsage.cullDistance > 0) {
    builtInPairs.push_back(
        std::make_pair(BuiltInCullDistance, ArrayType::get(builder.getFloatTy(), builtInUsage.cullDistance)));
  }

  if (builtInUsage.primitiveId)
    builtInPairs.push_back(std::make_pair(BuiltInPrimitiveId, builder.getInt32Ty()));

  if (builtInUsage.layer)
    builtInPairs.push_back(std::make_pair(BuiltInLayer, builder.getInt32Ty()));

  if (builtInUsage.viewportIndex)
    builtInPairs.push_back(std::make_pair(BuiltInViewportIndex, builder.getInt32Ty()));

  if (m_pipelineState->getInputAssemblyState().enableMultiView)
    builtInPairs.push_back(std::make_pair(BuiltInViewIndex, builder.getInt32Ty()));

  if (builtInUsage.primitiveShadingRate)
    builtInPairs.push_back(std::make_pair(BuiltInPrimitiveShadingRate, builder.getInt32Ty()));

  for (auto &builtInPair : builtInPairs) {
    auto builtInId = builtInPair.first;
    Type *builtInTy = builtInPair.second;

    assert(resUsage->inOutUsage.builtInOutputLocMap.find(builtInId) != resUsage->inOutUsage.builtInOutputLocMap.end());

    unsigned loc = resUsage->inOutUsage.builtInOutputLocMap[builtInId];
    Value *outputValue = loadValueFromGsVsRing(builtInTy, loc, 0, streamId, builder);
    exportBuiltInOutput(outputValue, builtInId, streamId, builder);
  }
}

// =====================================================================================================================
// Calculates GS to VS ring offset from input location
//
// @param location : Output location
// @param compIdx : Output component
// @param streamId : Output stream ID
// @param builder : BuilderBase to use for instruction constructing
Value *PatchCopyShader::calcGsVsRingOffsetForInput(unsigned location, unsigned compIdx, unsigned streamId,
                                                   BuilderBase &builder) {
  auto entryPoint = builder.GetInsertBlock()->getParent();
  Value *vertexOffset = getFunctionArgument(entryPoint, CopyShaderUserSgprIdxVertexOffset);

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);

  Value *ringOffset = nullptr;
  if (m_pipelineState->isGsOnChip()) {
    // ringOffset = esGsLdsSize + vertexOffset + location * 4 + compIdx
    ringOffset = builder.getInt32(resUsage->inOutUsage.gs.calcFactor.esGsLdsSize);
    ringOffset = builder.CreateAdd(ringOffset, vertexOffset);
    ringOffset = builder.CreateAdd(ringOffset, builder.getInt32(location * 4 + compIdx));
  } else {
    unsigned outputVertices = m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices;

    // ringOffset = vertexOffset * 4 + (location * 4 + compIdx) * 64 * maxVertices
    ringOffset = builder.CreateMul(vertexOffset, builder.getInt32(4));
    ringOffset = builder.CreateAdd(ringOffset, builder.getInt32((location * 4 + compIdx) * 64 * outputVertices));
  }

  return ringOffset;
}

// =====================================================================================================================
// Loads value from GS-VS ring (only accept 32-bit scalar, vector, or array).
//
// @param loadTy : Type of the load value
// @param location : Output location
// @param component : Output component
// @param streamId : Output stream ID
// @param builder : BuilderBase to use for instruction constructing
Value *PatchCopyShader::loadValueFromGsVsRing(Type *loadTy, unsigned location, unsigned component, unsigned streamId,
                                              BuilderBase &builder) {
  auto entryPoint = builder.GetInsertBlock()->getParent();

  unsigned elemCount = 1;
  Type *elemTy = loadTy;

  if (loadTy->isArrayTy()) {
    elemCount = cast<ArrayType>(loadTy)->getNumElements();
    elemTy = cast<ArrayType>(loadTy)->getElementType();
  } else if (loadTy->isVectorTy()) {
    elemCount = cast<FixedVectorType>(loadTy)->getNumElements();
    elemTy = cast<VectorType>(loadTy)->getElementType();
  }
  assert(elemTy->isIntegerTy(32) || elemTy->isFloatTy()); // Must be 32-bit type

  // NOTE: From Vulkan spec: If the 'Component' decoration is used on an 'OpVariable' that has a 'OpTypeVector' type
  // with a 'Component Type' with a 'Width' that is equal to 64, the sum of two times its 'Component Count' and the
  // 'Component' decoration value must be less than or equal to 4. Also, the 'Component' decorations must not be used
  // for a 64-bit vector type with more than two components.
  if (elemCount > 4)
    assert(component == 0);
  else
    assert(elemCount + component <= 4);

  if (m_pipelineState->getNggControl()->enableNgg) {
    // NOTE: For NGG, reading GS output from GS-VS ring is represented by a call and the call is replaced with
    // real instructions when when NGG primitive shader is generated.
    std::string callName(lgcName::NggReadGsOutput);
    callName += getTypeName(loadTy);
    return builder.CreateNamedCall(
        callName, loadTy, {builder.getInt32(location), builder.getInt32(component), builder.getInt32(streamId)},
        {Attribute::Speculatable, Attribute::ReadOnly, Attribute::WillReturn});
  }

  if (m_pipelineState->isGsOnChip()) {
    assert(m_lds);

    Value *ringOffset = calcGsVsRingOffsetForInput(location, component, streamId, builder);
    Value *loadPtr = builder.CreateGEP(m_lds->getValueType(), m_lds, {builder.getInt32(0), ringOffset});
    loadPtr = builder.CreateBitCast(loadPtr, PointerType::get(loadTy, m_lds->getType()->getPointerAddressSpace()));

    return builder.CreateAlignedLoad(loadTy, loadPtr, m_lds->getAlign());
  }

  CoherentFlag coherent = {};
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    coherent.bits.glc = true;
    coherent.bits.slc = true;
  }

  Value *loadValue = UndefValue::get(loadTy);

  for (unsigned i = 0; i < elemCount; ++i) {
    Value *ringOffset = calcGsVsRingOffsetForInput(location + i / 4, component + i % 4, streamId, builder);
    auto loadElem =
        builder.CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load, elemTy,
                                {
                                    m_pipelineSysValues.get(entryPoint)->getGsVsRingBufDesc(streamId), ringOffset,
                                    builder.getInt32(0),              // soffset
                                    builder.getInt32(coherent.u32All) // glc, slc
                                });

    if (loadTy->isArrayTy())
      loadValue = builder.CreateInsertValue(loadValue, loadElem, i);
    else if (loadTy->isVectorTy())
      loadValue = builder.CreateInsertElement(loadValue, loadElem, i);
    else {
      assert(elemCount == 1);
      loadValue = loadElem;
    }
  }

  return loadValue;
}

// =====================================================================================================================
// Exports generic outputs of geometry shader, inserting output-export calls.
//
// @param outputValue : Value exported to output
// @param location : Location of the output
// @param builder : BuilderBase to use for instruction constructing
void PatchCopyShader::exportGenericOutput(Value *outputValue, unsigned location, BuilderBase &builder) {
  auto outputTy = outputValue->getType();
  assert(outputTy->isSingleValueType());
  std::string instName(lgcName::OutputExportGeneric);
  instName += getTypeName(outputTy);
  builder.CreateNamedCall(instName, builder.getVoidTy(), {builder.getInt32(location), outputValue}, {});
}

// =====================================================================================================================
// Exports generic outputs of geometry shader, inserting output-export calls.
//
// @param outputValue : Value exported to output
// @param xfbOutInfo : The reference to a transform feedback output info
// @param builder : BuilderBase to use for instruction constructing
void PatchCopyShader::exportXfbOutput(Value *outputValue, const XfbOutInfo &xfbOutInfo, BuilderBase &builder) {
  if (xfbOutInfo.is16bit) {
    // NOTE: For 16-bit transform feedback output, the value is 32-bit dword loaded from GS-VS ring
    // buffer. The high word is always zero while the low word contains the data value. We have to
    // do some casting operations before store it to transform feedback buffer (tightly packed).
    auto outputTy = outputValue->getType();
    assert(outputTy->isFPOrFPVectorTy() && outputTy->getScalarSizeInBits() == 32);

    const unsigned compCount = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;
    if (compCount > 1) {
      outputValue = builder.CreateBitCast(outputValue, FixedVectorType::get(builder.getInt32Ty(), compCount));
      outputValue = builder.CreateTrunc(outputValue, FixedVectorType::get(builder.getInt16Ty(), compCount));
      outputValue = builder.CreateBitCast(outputValue, FixedVectorType::get(builder.getHalfTy(), compCount));
    } else {
      outputValue = builder.CreateBitCast(outputValue, builder.getInt32Ty());
      outputValue = new TruncInst(outputValue, builder.getInt16Ty());
      outputValue = new BitCastInst(outputValue, builder.getHalfTy());
    }
  }

  // Collect transform feedback export calls, used in SW-emulated stream-out.
  if (m_pipelineState->enableSwXfb()) {
    auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader)->inOutUsage;
    // A transform feedback export call is expected to be <4 x dword> at most
    inOutUsage.xfbExpCount += outputValue->getType()->getPrimitiveSizeInBits() > 128 ? 2 : 1;
  }

  Value *args[] = {builder.getInt32(xfbOutInfo.xfbBuffer), builder.getInt32(xfbOutInfo.xfbOffset),
                   builder.getInt32(xfbOutInfo.streamId), outputValue};

  std::string instName(lgcName::OutputExportXfb);
  addTypeMangling(nullptr, args, instName);
  builder.CreateNamedCall(instName, builder.getVoidTy(), args, {});
}

// =====================================================================================================================
// Exports built-in outputs of geometry shader, inserting output-export calls.
//
// @param outputValue : Value exported to output
// @param builtInId : ID of the built-in variable
// @param streamId : ID of output vertex stream
// @param builder : BuilderBase to use for instruction constructing
void PatchCopyShader::exportBuiltInOutput(Value *outputValue, BuiltInKind builtInId, unsigned streamId,
                                          BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);

  if (m_pipelineState->enableXfb()) {
    InOutLocationInfo outLocInfo;
    outLocInfo.setLocation(builtInId);
    outLocInfo.setBuiltIn(true);
    outLocInfo.setStreamId(streamId);

    auto &locInfoXfbOutInfoMap = resUsage->inOutUsage.locInfoXfbOutInfoMap;
    const auto &locInfoXfbOutInfoMapIt = locInfoXfbOutInfoMap.find(outLocInfo);
    if (locInfoXfbOutInfoMapIt != locInfoXfbOutInfoMap.end()) {
      // Collect transform feedback export calls, used in SW-emulated stream-out.
      if (m_pipelineState->enableSwXfb()) {
        auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader)->inOutUsage;
        // A transform feedback export call is expected to be <4 x dword> at most
        inOutUsage.xfbExpCount += outputValue->getType()->getPrimitiveSizeInBits() > 128 ? 2 : 1;
      }

      const auto &xfbOutInfo = locInfoXfbOutInfoMapIt->second;
      std::string instName(lgcName::OutputExportXfb);
      Value *args[] = {builder.getInt32(xfbOutInfo.xfbBuffer), builder.getInt32(xfbOutInfo.xfbOffset),
                       builder.getInt32(0), outputValue};
      addTypeMangling(nullptr, args, instName);
      builder.CreateNamedCall(instName, builder.getVoidTy(), args, {});
    }
  }

  if (resUsage->inOutUsage.gs.rasterStream == streamId) {
    std::string callName = lgcName::OutputExportBuiltIn;
    callName += PipelineState::getBuiltInName(builtInId);
    Value *args[] = {builder.getInt32(builtInId), outputValue};
    addTypeMangling(nullptr, args, callName);
    builder.CreateNamedCall(callName, builder.getVoidTy(), args, {});
  }
}
