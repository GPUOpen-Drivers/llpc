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
  auto gsEntryPoint = pipelineShaders.getEntryPoint(ShaderStageGeometry);
  if (!gsEntryPoint) {
    // No geometry shader -- copy shader not required.
    return false;
  }

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
  else
    m_gsVsRingBufDesc = loadGsVsRingBufferDescriptor(builder);

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

  // Set the shader stage on the new function.
  setShaderStage(entryPoint, ShaderStageCopyShader);

  // Tell pipeline state there is a copy shader.
  m_pipelineState->setShaderStageMask(m_pipelineState->getShaderStageMask() | (1U << ShaderStageCopyShader));

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
        origLocInfo.setComponent(cast<ConstantInt>(callInst->getOperand(1))->getZExtValue());
        origLocInfo.setStreamId(cast<ConstantInt>(callInst->getOperand(2))->getZExtValue());

        const auto locInfoMapIt = outputLocInfoMap.find(origLocInfo);
        if (locInfoMapIt == outputLocInfoMap.end() || visitedLocInfos.count(origLocInfo) > 0)
          continue;
        visitedLocInfos.insert(origLocInfo);

        // Each output call is scalarized and exports 4 bytes for packing
        unsigned byteSize = 4;
        auto &newLocByteSizesMap = m_newLocByteSizesMapArray[origLocInfo.getStreamId()];
        const unsigned newLoc = locInfoMapIt->second.getLocation();
        if (m_pipelineState->canPackOutput(ShaderStageGeometry)) {
          newLocByteSizesMap[newLoc] += byteSize;
        } else {
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
          byteSize = bitWidth / 8 * compCount;
          newLocByteSizesMap[newLoc] = byteSize;
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

  // Build the map between new location and output value
  DenseMap<unsigned, Value *> newLocValueMap;
  // Collect the output value at each mapped location in the given stream
  const auto &newLocByteSizesMap = m_newLocByteSizesMapArray[streamId];
  for (const auto &locByteSizePair : newLocByteSizesMap) {
    const unsigned newLoc = locByteSizePair.first;
    const unsigned byteSize = locByteSizePair.second;
    assert(byteSize % 4 == 0 && byteSize <= 32);
    const unsigned dwordSize = byteSize / 4;
    Value *outputValue = loadValueFromGsVsRing(dwordSize > 1 ? FixedVectorType::get(builder.getFloatTy(), dwordSize)
                                                             : builder.getFloatTy(),
                                               newLoc, streamId, builder);
    newLocValueMap[newLoc] = outputValue;
  }

  if (m_pipelineState->enableXfb()) {
    // Export XFB output
    if (m_pipelineState->canPackOutput(ShaderStageGeometry)) {
      // With packing locations, we should collect the XFB output value at an original location
      DenseMap<unsigned, std::pair<unsigned, SmallVector<Value *, 4>>> origLocCompElemsMap;
      for (const auto &locInfoXfbInfoPair : locInfoXfbOutInfoMap) {
        const InOutLocationInfo &origLocInfo = locInfoXfbInfoPair.first;
        if (origLocInfo.getStreamId() != streamId || origLocInfo.isBuiltIn())
          continue;
        // Get each component from the packed output value
        assert(outputLocInfoMap.count(origLocInfo));
        auto &newLocInfo = outputLocInfoMap[origLocInfo];
        assert(newLocValueMap.count(newLocInfo.getLocation()) > 0);
        Value *const packedOutValue = newLocValueMap[newLocInfo.getLocation()];
        Value *elem = packedOutValue->getType()->isVectorTy()
                          ? builder.CreateExtractElement(packedOutValue, newLocInfo.getComponent())
                          : packedOutValue;

        auto &elements = origLocCompElemsMap[origLocInfo.getLocation()].second;
        elements.push_back(elem);
        origLocCompElemsMap[origLocInfo.getLocation()].first = origLocInfo.getComponent();
      }
      // Construct original XFB output value and export it
      for (const auto &entry : origLocCompElemsMap) {
        auto &elements = entry.second.second;
        const unsigned elemCount = elements.size();
        Value *xfbOutValue = nullptr;
        if (elemCount > 1) {
          xfbOutValue = UndefValue::get(FixedVectorType::get(builder.getFloatTy(), elemCount));
          for (unsigned idx = 0; idx < elemCount; ++idx)
            xfbOutValue = builder.CreateInsertElement(xfbOutValue, elements[idx], idx);
        } else {
          xfbOutValue = elements[0];
        }

        // Get the XFB out info at the original location info
        InOutLocationInfo origLocInfo;
        origLocInfo.setLocation(entry.first);
        origLocInfo.setComponent(entry.second.first);
        origLocInfo.setStreamId(streamId);
        assert(locInfoXfbOutInfoMap.count(origLocInfo) > 0);
        auto &xfbInfo = locInfoXfbOutInfoMap[origLocInfo];
        exportXfbOutput(xfbOutValue, xfbInfo, builder);
      }
    } else {
      // Without packing locations, we could directly export output value and its related XFB out info
      for (const auto &locInfoXfbInfoPair : locInfoXfbOutInfoMap) {
        const InOutLocationInfo &origLocInfo = locInfoXfbInfoPair.first;
        if (origLocInfo.getStreamId() != streamId || origLocInfo.isBuiltIn())
          continue;
        const auto &xfbOutInfo = locInfoXfbInfoPair.second;
        assert(outputLocInfoMap.count(origLocInfo) > 0);
        auto &newLocInfo = outputLocInfoMap[origLocInfo];
        const unsigned newLoc = newLocInfo.getLocation();
        if (newLocValueMap.count(newLoc) == 0) {
          // In locInfoXfbOutInfoMap, .z/.w component of 64-bit occupied a subsequent location
          // In newLocValueMap, all components are exported in one location
          continue;
        }
        Value *const xbfOutputValue = newLocValueMap[newLoc];
        exportXfbOutput(xbfOutputValue, xfbOutInfo, builder);
      }
    }
  }

  // Following non-XFB output exports are only for rasterization stream
  if (resUsage->inOutUsage.gs.rasterStream != streamId)
    return;

  for (const auto &locValuePair : newLocValueMap)
    exportGenericOutput(locValuePair.second, locValuePair.first, builder);

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
    Value *outputValue = loadValueFromGsVsRing(builtInTy, loc, streamId, builder);
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
// @param streamId : Output stream ID
// @param builder : BuilderBase to use for instruction constructing
Value *PatchCopyShader::loadValueFromGsVsRing(Type *loadTy, unsigned location, unsigned streamId,
                                              BuilderBase &builder) {
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

  if (m_pipelineState->getNggControl()->enableNgg) {
    // NOTE: For NGG, reading GS output from GS-VS ring is represented by a call and the call is replaced with
    // real instructions when when NGG primitive shader is generated.
    std::string callName(lgcName::NggReadGsOutput);
    callName += getTypeName(loadTy);
    return builder.CreateNamedCall(callName, loadTy, {builder.getInt32(location), builder.getInt32(streamId)},
                                   {Attribute::Speculatable, Attribute::ReadOnly, Attribute::WillReturn});
  }

  if (m_pipelineState->isGsOnChip()) {
    assert(m_lds);

    Value *ringOffset = calcGsVsRingOffsetForInput(location, 0, streamId, builder);
    Value *loadPtr = builder.CreateGEP(m_lds->getValueType(), m_lds, {builder.getInt32(0), ringOffset});
    loadPtr = builder.CreateBitCast(loadPtr, PointerType::get(loadTy, m_lds->getType()->getPointerAddressSpace()));

    return builder.CreateAlignedLoad(loadTy, loadPtr, m_lds->getAlign());
  }
  assert(m_gsVsRingBufDesc);

  CoherentFlag coherent = {};
  coherent.bits.glc = true;
  coherent.bits.slc = true;

  Value *loadValue = UndefValue::get(loadTy);

  for (unsigned i = 0; i < elemCount; ++i) {
    Value *ringOffset = calcGsVsRingOffsetForInput(location + i / 4, i % 4, streamId, builder);
    auto loadElem = builder.CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load, elemTy,
                                            {
                                                m_gsVsRingBufDesc, ringOffset,
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
// Load GS-VS ring buffer descriptor.
//
// @param builder : BuilderBase to use for instruction constructing
Value *PatchCopyShader::loadGsVsRingBufferDescriptor(BuilderBase &builder) {
  Function *entryPoint = builder.GetInsertBlock()->getParent();
  Value *internalTablePtrLow = getFunctionArgument(entryPoint, EntryArgIdxInternalTablePtrLow);

  Value *pc = builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
  pc = builder.CreateBitCast(pc, FixedVectorType::get(builder.getInt32Ty(), 2));

  auto internalTablePtrHigh = builder.CreateExtractElement(pc, 1);

  auto undef = UndefValue::get(FixedVectorType::get(builder.getInt32Ty(), 2));
  Value *internalTablePtr = builder.CreateInsertElement(undef, internalTablePtrLow, uint64_t(0));
  internalTablePtr = builder.CreateInsertElement(internalTablePtr, internalTablePtrHigh, 1);
  internalTablePtr = builder.CreateBitCast(internalTablePtr, builder.getInt64Ty());

  auto gsVsRingBufDescPtr = builder.CreateAdd(internalTablePtr, builder.getInt64(SiDrvTableVsRingInOffs << 4));

  auto int32x4Ty = FixedVectorType::get(builder.getInt32Ty(), 4);
  auto int32x4PtrTy = PointerType::get(int32x4Ty, ADDR_SPACE_CONST);
  gsVsRingBufDescPtr = builder.CreateIntToPtr(gsVsRingBufDescPtr, int32x4PtrTy);
  cast<Instruction>(gsVsRingBufDescPtr)
      ->setMetadata(MetaNameUniform, MDNode::get(gsVsRingBufDescPtr->getContext(), {}));

  auto gsVsRingBufDesc = builder.CreateLoad(int32x4Ty, gsVsRingBufDescPtr);
  gsVsRingBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(gsVsRingBufDesc->getContext(), {}));

  return gsVsRingBufDesc;
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
