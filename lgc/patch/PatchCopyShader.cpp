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
 * @file  PatchCopyShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchCopyShader.
 ***********************************************************************************************************************
 */
#include "BuilderImpl.h"
#include "Internal.h"
#include "IntrinsDefs.h"
#include "Patch.h"
#include "PipelineShaders.h"
#include "PipelineState.h"
#include "TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-patch-copy-shader"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Pass to generate copy shader if required
class PatchCopyShader : public Patch {
public:
  static char ID;
  PatchCopyShader() : Patch(ID) {}

  bool runOnModule(Module &module) override;

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    // Pass does not preserve PipelineShaders as it adds a new shader.
  }

private:
  PatchCopyShader(const PatchCopyShader &) = delete;
  PatchCopyShader &operator=(const PatchCopyShader &) = delete;

  void exportOutput(unsigned streamId, BuilderBase &builder);
  void collectGsGenericOutputInfo(Function *gsEntryPoint);

  Value *calcGsVsRingOffsetForInput(unsigned location, unsigned compIdx, unsigned streamId, BuilderBase &builder);

  Value *loadValueFromGsVsRing(Type *loadTy, unsigned location, unsigned streamId, BuilderBase &builder);

  Value *loadGsVsRingBufferDescriptor(BuilderBase &builder);

  void exportGenericOutput(Value *outputValue, unsigned location, unsigned streamId, BuilderBase &builder);
  void exportBuiltInOutput(Value *outputValue, BuiltInKind builtInId, unsigned streamId, BuilderBase &builder);

  // -----------------------------------------------------------------------------------------------------------------

  // Low part of global internal table pointer
  static const unsigned EntryArgIdxInternalTablePtrLow = 0;

  PipelineState *m_pipelineState;     // Pipeline state
  GlobalVariable *m_lds = nullptr;    // Global variable representing LDS
  Value *m_gsVsRingBufDesc = nullptr; // Descriptor for GS-VS ring
};

char PatchCopyShader::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create pass to generate copy shader if required.
ModulePass *lgc::createPatchCopyShader() {
  return new PatchCopyShader();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchCopyShader::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Copy-Shader\n");

  Patch::init(&module);
  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  auto pipelineShaders = &getAnalysis<PipelineShaders>();
  auto gsEntryPoint = pipelineShaders->getEntryPoint(ShaderStageGeometry);
  if (!gsEntryPoint) {
    // No geometry shader -- copy shader not required.
    return false;
  }

  // Gather GS generic export details.
  collectGsGenericOutputInfo(gsEntryPoint);

  // Create type of new function:
  // define void @copy_shader(
  //    i32 inreg,  ; Internal table
  //    i32 inreg,  ; Shader table
  //    i32 inreg,  ; Stream-out table (GFX6-GFX8) / ES-GS size (GFX9+)
  //    i32 inreg,  ; ES-GS size (GFX6-GFX8) / Stream-out table (GFX9+)
  //    i32 inreg,  ; Stream info
  //    i32 inreg,  ; Stream-out write index
  //    i32 inreg,  ; Stream offset0
  //    i32 inreg,  ; Stream offset1
  //    i32 inreg,  ; Stream offset2
  //    i32 inreg,  ; Stream offset3
  //    i32
  BuilderBase builder(*m_context);

  auto int32Ty = Type::getInt32Ty(*m_context);
  Type *argTys[] = {int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty, int32Ty};
  bool argInReg[] = {true, true, true, true, true, true, true, true, true, true, false};
  auto entryPointTy = FunctionType::get(builder.getVoidTy(), argTys, false);

  // Create function for the copy shader entrypoint, and insert it before the FS (if there is one).
  auto entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::CopyShaderEntryPoint);

  auto insertPos = module.getFunctionList().end();
  auto fsEntryPoint = pipelineShaders->getEntryPoint(ShaderStageFragment);
  if (fsEntryPoint)
    insertPos = fsEntryPoint->getIterator();
  module.getFunctionList().insert(insertPos, entryPoint);

  // Make the args "inreg" (passed in SGPR) as appropriate.
  for (unsigned i = 0; i < sizeof(argInReg) / sizeof(argInReg[0]); ++i) {
    if (argInReg[i])
      entryPoint->arg_begin()[i].addAttr(Attribute::InReg);
  }

  // Create ending basic block, and terminate it with return.
  auto endBlock = BasicBlock::Create(*m_context, "", entryPoint, nullptr);
  builder.SetInsertPoint(endBlock);
  builder.CreateRetVoid();

  // Create entry basic block
  auto entryBlock = BasicBlock::Create(*m_context, "", entryPoint, endBlock);
  builder.SetInsertPoint(entryBlock);

  auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageCopyShader);

  // For GFX6 ~ GFX8, streamOutTable SGPR index value should be less than esGsLdsSize
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 8) {
    intfData->userDataUsage.gs.copyShaderStreamOutTable = 2;
    intfData->userDataUsage.gs.copyShaderEsGsLdsSize = 3;
  }
  // For GFX9+, streamOutTable SGPR index value should be greater than esGsLdsSize
  else {
    intfData->userDataUsage.gs.copyShaderStreamOutTable = 3;
    intfData->userDataUsage.gs.copyShaderEsGsLdsSize = 2;
  }

  if (m_pipelineState->isGsOnChip())
    m_lds = Patch::getLdsVariable(m_pipelineState, &module);
  else
    m_gsVsRingBufDesc = loadGsVsRingBufferDescriptor(builder);

  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);

  unsigned outputStreamCount = 0;
  unsigned outputStreamId = InvalidValue;
  for (int i = 0; i < MaxGsStreams; ++i) {
    if (resUsage->inOutUsage.gs.outLocCount[i] > 0) {
      outputStreamCount++;
      if (outputStreamId == InvalidValue)
        outputStreamId = i;
    }
  }

  if (outputStreamCount > 1 && resUsage->inOutUsage.enableXfb) {
    // StreamId = streamInfo[25:24]
    auto streamInfo = getFunctionArgument(entryPoint, CopyShaderUserSgprIdxStreamInfo);

    Value *streamId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, builder.getInt32Ty(),
                                              {
                                                  streamInfo,
                                                  builder.getInt32(24),
                                                  builder.getInt32(2),
                                              });

    //
    // .entry:
    //      switch i32 %streamId, label %.end [ i32 0, lable %stream0
    //                                          i32 1, label %stream1
    //                                          i32 2, label %stream2
    //                                          i32 3, label %stream3 ]
    //
    // .stream0:
    //      export(0)
    //      br %label .end
    //
    // .stream1:
    //      export(1)
    //      br %label .end
    //
    // .stream2:
    //      export(2)
    //      br %label .end
    //
    // .stream3:
    //      export(3)
    //      br %label .end
    //
    // .end:
    //      ret void
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
    outputStreamId = outputStreamCount == 0 ? 0 : outputStreamId;
    exportOutput(outputStreamId, builder);
    builder.CreateBr(endBlock);
  }

  // Add execution model metadata to the function.
  auto execModelMeta = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), ShaderStageCopyShader));
  auto execModelMetaNode = MDNode::get(*m_context, execModelMeta);
  entryPoint->addMetadata(lgcName::ShaderStageMetadata, *execModelMetaNode);

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

  for (auto &func : *gsEntryPoint->getParent()) {
    if (func.getName().startswith(lgcName::OutputExportGeneric)) {
      for (auto user : func.users()) {
        auto callInst = dyn_cast<CallInst>(user);
        if (!callInst || callInst->getParent()->getParent() != gsEntryPoint)
          continue;

        assert(callInst->getNumArgOperands() == 4);
        Value *output = callInst->getOperand(callInst->getNumArgOperands() - 1); // Last argument
        auto outputTy = output->getType();

        unsigned value = cast<ConstantInt>(callInst->getOperand(0))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(callInst->getOperand(2))->getZExtValue();

        GsOutLocInfo outLocInfo = {};
        outLocInfo.location = value;
        outLocInfo.isBuiltIn = false;
        outLocInfo.streamId = streamId;

        auto locMapIt = resUsage->inOutUsage.outputLocMap.find(outLocInfo.u32All);
        if (locMapIt == resUsage->inOutUsage.outputLocMap.end())
          continue;

        unsigned location = locMapIt->second;
        const unsigned compIdx = cast<ConstantInt>(callInst->getOperand(1))->getZExtValue();

        unsigned compCount = 1;
        auto compTy = outputTy;
        auto outputVecTy = dyn_cast<VectorType>(outputTy);
        if (outputVecTy) {
          compCount = outputVecTy->getNumElements();
          compTy = outputVecTy->getElementType();
        }
        unsigned bitWidth = compTy->getScalarSizeInBits();
        // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend
        // BYTE/WORD to DWORD and store DWORD to GS-VS ring. So for 8-bit/16-bit data type, the actual byte size
        // is based on number of DWORDs.
        bitWidth = bitWidth < 32 ? 32 : bitWidth;
        unsigned byteSize = bitWidth / 8 * compCount;

        assert(compIdx < 4);
        auto &genericOutByteSizes =
            m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader)->inOutUsage.gs.genericOutByteSizes;
        genericOutByteSizes[streamId][location].resize(4);
        genericOutByteSizes[streamId][location][compIdx] = byteSize;
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
  const auto &genericOutByteSizes = resUsage->inOutUsage.gs.genericOutByteSizes;

  // Export generic outputs
  for (auto &byteSizeMap : genericOutByteSizes[streamId]) {
    // <location, <component, byteSize>>
    unsigned loc = byteSizeMap.first;

    unsigned byteSize = 0;
    for (unsigned i = 0; i < 4; ++i)
      byteSize += byteSizeMap.second[i];

    assert(byteSize % 4 == 0);
    unsigned dwordSize = byteSize / 4;
    Value *outputValue =
        loadValueFromGsVsRing(VectorType::get(builder.getFloatTy(), dwordSize), loc, streamId, builder);
    exportGenericOutput(outputValue, loc, streamId, builder);
  }

  // Export built-in outputs
  std::vector<std::pair<BuiltInKind, Type *>> builtInPairs;

  if (builtInUsage.position)
    builtInPairs.push_back(std::make_pair(BuiltInPosition, VectorType::get(builder.getFloatTy(), 4)));

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

  const auto enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
  if (builtInUsage.layer || enableMultiView) {
    // NOTE: If mult-view is enabled, always export gl_ViewIndex rather than gl_Layer.
    builtInPairs.push_back(std::make_pair(enableMultiView ? BuiltInViewIndex : BuiltInLayer, builder.getInt32Ty()));
  }

  if (builtInUsage.viewportIndex)
    builtInPairs.push_back(std::make_pair(BuiltInViewportIndex, builder.getInt32Ty()));

  for (auto &builtInPair : builtInPairs) {
    auto builtInId = builtInPair.first;
    Type *builtInTy = builtInPair.second;

    assert(resUsage->inOutUsage.builtInOutputLocMap.find(builtInId) != resUsage->inOutUsage.builtInOutputLocMap.end());

    unsigned loc = resUsage->inOutUsage.builtInOutputLocMap[builtInId];
    Value *outputValue = loadValueFromGsVsRing(builtInTy, loc, streamId, builder);
    exportBuiltInOutput(outputValue, builtInId, streamId, builder);
  }

  // Generate dummy gl_position vec4(0, 0, 0, 1) for the rasterization stream if transform feeback is enabled
  if (resUsage->inOutUsage.enableXfb && !static_cast<bool>(builtInUsage.position)) {
    auto zero = ConstantFP::get(builder.getFloatTy(), 0.0);
    auto one = ConstantFP::get(builder.getFloatTy(), 1.0);

    std::vector<Constant *> outputValues = {zero, zero, zero, one};
    exportBuiltInOutput(ConstantVector::get(outputValues), BuiltInPosition, streamId, builder);
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
// Loads value from GS-VS ring (only accept 32-bit scalar, vector, or arry).
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
    elemCount = loadTy->getArrayNumElements();
    elemTy = loadTy->getArrayElementType();
  } else if (loadTy->isVectorTy()) {
    elemCount = loadTy->getVectorNumElements();
    elemTy = loadTy->getVectorElementType();
  }
  assert(elemTy->isIntegerTy(32) || elemTy->isFloatTy()); // Must be 32-bit type

  if (m_pipelineState->getNggControl()->enableNgg) {
    // NOTE: For NGG, importing GS output from GS-VS ring is represented by a call and the call is replaced with
    // real instructions when when NGG primitive shader is generated.
    std::string callName(lgcName::NggGsOutputImport);
    callName += getTypeName(loadTy);
    return builder.createNamedCall(callName, loadTy,
                                   {builder.getInt32(location), builder.getInt32(0), builder.getInt32(streamId)}, {});
  }

  if (m_pipelineState->isGsOnChip()) {
    assert(m_lds);

    Value *ringOffset = calcGsVsRingOffsetForInput(location, 0, streamId, builder);
    Value *loadPtr = builder.CreateGEP(m_lds, {builder.getInt32(0), ringOffset});
    loadPtr = builder.CreateBitCast(loadPtr, PointerType::get(loadTy, m_lds->getType()->getPointerAddressSpace()));

    return builder.CreateAlignedLoad(loadPtr, MaybeAlign(m_lds->getAlignment()));
  } else {
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
}

// =====================================================================================================================
// Load GS-VS ring buffer descriptor.
//
// @param builder : BuilderBase to use for instruction constructing
Value *PatchCopyShader::loadGsVsRingBufferDescriptor(BuilderBase &builder) {
  Function *entryPoint = builder.GetInsertBlock()->getParent();
  Value *internalTablePtrLow = getFunctionArgument(entryPoint, EntryArgIdxInternalTablePtrLow);

  Value *pc = builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
  pc = builder.CreateBitCast(pc, VectorType::get(builder.getInt32Ty(), 2));

  auto internalTablePtrHigh = builder.CreateExtractElement(pc, 1);

  auto undef = UndefValue::get(VectorType::get(builder.getInt32Ty(), 2));
  Value *internalTablePtr = builder.CreateInsertElement(undef, internalTablePtrLow, uint64_t(0));
  internalTablePtr = builder.CreateInsertElement(internalTablePtr, internalTablePtrHigh, 1);
  internalTablePtr = builder.CreateBitCast(internalTablePtr, builder.getInt64Ty());

  auto gsVsRingBufDescPtr = builder.CreateAdd(internalTablePtr, builder.getInt64(SiDrvTableVsRingInOffs << 4));

  auto int32x4PtrTy = PointerType::get(VectorType::get(builder.getInt32Ty(), 4), ADDR_SPACE_CONST);
  gsVsRingBufDescPtr = builder.CreateIntToPtr(gsVsRingBufDescPtr, int32x4PtrTy);
  cast<Instruction>(gsVsRingBufDescPtr)
      ->setMetadata(MetaNameUniform, MDNode::get(gsVsRingBufDescPtr->getContext(), {}));

  auto gsVsRingBufDesc = builder.CreateLoad(gsVsRingBufDescPtr);
  gsVsRingBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(gsVsRingBufDesc->getContext(), {}));

  return gsVsRingBufDesc;
}

// =====================================================================================================================
// Exports generic outputs of geometry shader, inserting output-export calls.
//
// @param outputValue : Value exported to output
// @param location : Location of the output
// @param streamId : ID of output vertex stream
// @param builder : BuilderBase to use for instruction constructing
void PatchCopyShader::exportGenericOutput(Value *outputValue, unsigned location, unsigned streamId,
                                          BuilderBase &builder) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageCopyShader);
  if (resUsage->inOutUsage.enableXfb) {
    auto &outLocMap = resUsage->inOutUsage.outputLocMap;
    auto &xfbOutsInfo = resUsage->inOutUsage.gs.xfbOutsInfo;

    // Find original location in outLocMap which equals used location in copy shader
    auto locIter =
        find_if(outLocMap.begin(), outLocMap.end(), [location, streamId](const std::pair<unsigned, unsigned> &outLoc) {
          unsigned outLocInfo = outLoc.first;
          bool isStreamId = (reinterpret_cast<GsOutLocInfo *>(&outLocInfo))->streamId == streamId;
          return outLoc.second == location && isStreamId;
        });

    assert(locIter != outLocMap.end());
    if (xfbOutsInfo.find(locIter->first) != xfbOutsInfo.end()) {
      XfbOutInfo *xfbOutInfo = reinterpret_cast<XfbOutInfo *>(&xfbOutsInfo[locIter->first]);

      if (xfbOutInfo->is16bit) {
        // NOTE: For 16-bit transform feedback output, the value is 32-bit DWORD loaded from GS-VS ring
        // buffer. The high WORD is always zero while the low WORD contains the data value. We have to
        // do some casting operations before store it to transform feedback buffer (tightly packed).
        auto outputTy = outputValue->getType();
        assert(outputTy->isFPOrFPVectorTy() && outputTy->getScalarSizeInBits() == 32);

        const unsigned compCount = outputTy->isVectorTy() ? outputTy->getVectorNumElements() : 1;
        if (compCount > 1) {
          outputValue = builder.CreateBitCast(outputValue, VectorType::get(builder.getInt32Ty(), compCount));
          outputValue = builder.CreateTrunc(outputValue, VectorType::get(builder.getInt16Ty(), compCount));
          outputValue = builder.CreateBitCast(outputValue, VectorType::get(builder.getHalfTy(), compCount));
        } else {
          outputValue = builder.CreateBitCast(outputValue, builder.getInt32Ty());
          outputValue = new TruncInst(outputValue, builder.getInt16Ty());
          outputValue = new BitCastInst(outputValue, builder.getHalfTy());
        }
      }

      Value *args[] = {builder.getInt32(xfbOutInfo->xfbBuffer), builder.getInt32(xfbOutInfo->xfbOffset),
                       builder.getInt32(xfbOutInfo->xfbExtraOffset), outputValue};

      std::string instName(lgcName::OutputExportXfb);
      addTypeMangling(nullptr, args, instName);
      builder.createNamedCall(instName, builder.getVoidTy(), args, {});
    }
  }

  if (resUsage->inOutUsage.gs.rasterStream == streamId) {
    auto outputTy = outputValue->getType();
    assert(outputTy->isSingleValueType());

    std::string instName(lgcName::OutputExportGeneric);
    instName += getTypeName(outputTy);

    builder.createNamedCall(instName, builder.getVoidTy(), {builder.getInt32(location), outputValue}, {});
  }
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

  if (resUsage->inOutUsage.enableXfb) {
    GsOutLocInfo outLocInfo = {};
    outLocInfo.location = builtInId;
    outLocInfo.isBuiltIn = true;
    outLocInfo.streamId = streamId;

    auto &xfbOutsInfo = resUsage->inOutUsage.gs.xfbOutsInfo;
    auto locIter = xfbOutsInfo.find(outLocInfo.u32All);
    if (locIter != xfbOutsInfo.end()) {
      XfbOutInfo *xfbOutInfo = reinterpret_cast<XfbOutInfo *>(&xfbOutsInfo[locIter->first]);

      std::string instName(lgcName::OutputExportXfb);
      Value *args[] = {builder.getInt32(xfbOutInfo->xfbBuffer), builder.getInt32(xfbOutInfo->xfbOffset),
                       builder.getInt32(0), outputValue};
      addTypeMangling(nullptr, args, instName);
      builder.createNamedCall(instName, builder.getVoidTy(), args, {});
    }
  }

  if (resUsage->inOutUsage.gs.rasterStream == streamId) {
    std::string callName = lgcName::OutputExportBuiltIn;
    callName += BuilderImplInOut::getBuiltInName(builtInId);
    Value *args[] = {builder.getInt32(builtInId), outputValue};
    addTypeMangling(nullptr, args, callName);
    builder.createNamedCall(callName, builder.getVoidTy(), args, {});
  }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchCopyShader, DEBUG_TYPE, "Patch LLVM for copy shader generation", false, false)
