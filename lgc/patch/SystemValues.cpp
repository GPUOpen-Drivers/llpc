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
 * @file  SystemValues.cpp
 * @brief LLPC source file: per-shader per-pass generating and cache of shader system pointers
 ***********************************************************************************************************************
 */
#include "lgc/patch/SystemValues.h"
#include "ShaderMerger.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#define DEBUG_TYPE "lgc-system-values"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Initialize this ShaderSystemValues if it was previously uninitialized.
//
// @param pipelineState : Pipeline state
// @param entryPoint : Shader entrypoint
void ShaderSystemValues::initialize(PipelineState *pipelineState, Function *entryPoint) {
  if (!m_entryPoint) {
    m_entryPoint = entryPoint;
    m_shaderStage = getShaderStage(entryPoint);
    m_context = &entryPoint->getParent()->getContext();
    m_pipelineState = pipelineState;

    assert(m_shaderStage != ShaderStageInvalid);
    if (m_shaderStage != ShaderStageCopyShader) {
      // NOTE: For shader stages other than copy shader, make sure their entry-points are mutated with proper arguments.
      // For copy shader, we don't need such check because entry-point mutation is not applied to copy shader. Copy
      // shader is completely generated.
      assert(m_pipelineState->getShaderInterfaceData(m_shaderStage)->entryArgIdxs.initialized);
    }
  }
}

// =====================================================================================================================
// Get ES-GS ring buffer descriptor (for VS/TES output or GS input)
Value *ShaderSystemValues::getEsGsRingBufDesc() {
  if (!m_esGsRingBufDesc) {
    unsigned tableOffset = 0;
    switch (m_shaderStage) {
    case ShaderStageVertex:
    case ShaderStageTessEval:
      tableOffset = SiDrvTableEsRingOutOffs;
      break;
    case ShaderStageGeometry:
      tableOffset = SiDrvTableGsRingInOffs;
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }

    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());
    m_esGsRingBufDesc = loadDescFromDriverTable(tableOffset, builder);
    if (m_shaderStage != ShaderStageGeometry && m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 8) {
      // NOTE: For GFX8+, we have to explicitly set DATA_FORMAT for GS-VS ring buffer descriptor for
      // VS/TES output.
      m_esGsRingBufDesc = setRingBufferDataFormat(m_esGsRingBufDesc, BUF_DATA_FORMAT_32, builder);
    }
  }
  return m_esGsRingBufDesc;
}

// =====================================================================================================================
// Get the descriptor for tessellation factor (TF) buffer (TCS output)
Value *ShaderSystemValues::getTessFactorBufDesc() {
  assert(m_shaderStage == ShaderStageTessControl);
  if (!m_tfBufDesc) {
    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());
    m_tfBufDesc = loadDescFromDriverTable(SiDrvTableTfBufferOffs, builder);
  }
  return m_tfBufDesc;
}

// =====================================================================================================================
// Get the descriptor for vertex attribute ring buffer (for VS, TES, and copy shader output)
Value *ShaderSystemValues::getAttribRingBufDesc() {
  // Vertex attributes through memory is for GFX11+
  assert(m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11);
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader || m_shaderStage == ShaderStageMesh);
  if (!m_attribRingBufDesc) {
    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());
    m_attribRingBufDesc = loadDescFromDriverTable(SiDrvTableOffChipParamCache, builder);
  }
  return m_attribRingBufDesc;
}

// =====================================================================================================================
// Get the descriptor for task payload ring buffer (for task and mesh shader)
Value *ShaderSystemValues::getTaskPayloadRingBufDesc() {
  assert(m_pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  assert(m_shaderStage == ShaderStageTask || m_shaderStage == ShaderStageMesh);
  if (!m_taskPayloadRingBufDesc) {
    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());
    m_taskPayloadRingBufDesc = loadDescFromDriverTable(SiDrvTableTaskPayloadRingOffs, builder);
  }
  return m_taskPayloadRingBufDesc;
}

// =====================================================================================================================
// Get the descriptor for task draw data ring buffer (for task shader)
Value *ShaderSystemValues::getTaskDrawDataRingBufDesc() {
  assert(m_pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  assert(m_shaderStage == ShaderStageTask);
  if (!m_taskDrawDataRingBufDesc) {
    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());
    m_taskDrawDataRingBufDesc = loadDescFromDriverTable(SiDrvTableTaskDrawDataRingOffs, builder);
  }
  return m_taskDrawDataRingBufDesc;
}

// =====================================================================================================================
// Extract value of primitive ID (TCS)
Value *ShaderSystemValues::getPrimitiveId() {
  assert(m_shaderStage == ShaderStageTessControl);
  if (!m_primitiveId) {
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
    m_primitiveId = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tcs.patchId, "patchId");
  }
  return m_primitiveId;
}

// =====================================================================================================================
// Get invocation ID (TCS)
Value *ShaderSystemValues::getInvocationId() {
  assert(m_shaderStage == ShaderStageTessControl);
  if (!m_invocationId) {
    auto insertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

    // invocationId = relPatchId[12:8]
    Value *args[] = {getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tcs.relPatchId, "relPatchId"),
                     ConstantInt::get(Type::getInt32Ty(*m_context), 8),
                     ConstantInt::get(Type::getInt32Ty(*m_context), 5)};
    m_invocationId =
        emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, Attribute::ReadNone, insertPos);
  }
  return m_invocationId;
}

// =====================================================================================================================
// Get relative patchId (TCS)
Value *ShaderSystemValues::getRelativeId() {
  assert(m_shaderStage == ShaderStageTessControl);
  if (!m_relativeId) {
    auto insertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
    auto relPatchId = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tcs.relPatchId, "relPatchId");

    // relativeId = relPatchId[7:0]
    m_relativeId =
        BinaryOperator::CreateAnd(relPatchId, ConstantInt::get(Type::getInt32Ty(*m_context), 0xFF), "", insertPos);
  }
  return m_relativeId;
}

// =====================================================================================================================
// Get offchip LDS descriptor (TCS and TES)
Value *ShaderSystemValues::getOffChipLdsDesc() {
  assert(m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval);
  if (!m_offChipLdsDesc) {
    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());
    m_offChipLdsDesc = loadDescFromDriverTable(SiDrvTableHsBuffeR0Offs, builder);
  }
  return m_offChipLdsDesc;
}

// =====================================================================================================================
// Get tessellated coordinate (TES)
Value *ShaderSystemValues::getTessCoord() {
  assert(m_shaderStage == ShaderStageTessEval);
  if (!m_tessCoord) {
    auto insertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

    Value *tessCoordX = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tes.tessCoordX, "tessCoordX");
    Value *tessCoordY = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.tes.tessCoordY, "tessCoordY");
    Value *tessCoordZ = BinaryOperator::CreateFAdd(tessCoordX, tessCoordY, "", insertPos);

    tessCoordZ =
        BinaryOperator::CreateFSub(ConstantFP::get(Type::getFloatTy(*m_context), 1.0f), tessCoordZ, "", insertPos);

    auto primitiveMode = m_pipelineState->getShaderModes()->getTessellationMode().primitiveMode;
    tessCoordZ =
        primitiveMode == PrimitiveMode::Triangles ? tessCoordZ : ConstantFP::get(Type::getFloatTy(*m_context), 0.0f);

    m_tessCoord = PoisonValue::get(FixedVectorType::get(Type::getFloatTy(*m_context), 3));
    m_tessCoord = InsertElementInst::Create(m_tessCoord, tessCoordX, ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                            "", insertPos);
    m_tessCoord = InsertElementInst::Create(m_tessCoord, tessCoordY, ConstantInt::get(Type::getInt32Ty(*m_context), 1),
                                            "", insertPos);
    m_tessCoord = InsertElementInst::Create(m_tessCoord, tessCoordZ, ConstantInt::get(Type::getInt32Ty(*m_context), 2),
                                            "", insertPos);
  }
  return m_tessCoord;
}

// =====================================================================================================================
// Get ES -> GS offsets (GS in)
Value *ShaderSystemValues::getEsGsOffsets() {
  assert(m_shaderStage == ShaderStageGeometry);
  if (!m_esGsOffsets) {
    auto insertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);

    m_esGsOffsets = PoisonValue::get(FixedVectorType::get(Type::getInt32Ty(*m_context), 6));
    for (unsigned i = 0; i < InterfaceData::MaxEsGsOffsetCount; ++i) {
      auto esGsOffset =
          getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.gs.esGsOffsets[i], Twine("esGsOffset") + Twine(i));
      m_esGsOffsets = InsertElementInst::Create(m_esGsOffsets, esGsOffset,
                                                ConstantInt::get(Type::getInt32Ty(*m_context), i), "", insertPos);
    }
  }
  return m_esGsOffsets;
}

// =====================================================================================================================
// Get GS -> VS ring buffer descriptor (GS out and copy shader in)
//
// @param streamId : Stream ID, always 0 for copy shader
Value *ShaderSystemValues::getGsVsRingBufDesc(unsigned streamId) {
  assert(m_shaderStage == ShaderStageGeometry || m_shaderStage == ShaderStageCopyShader);
  if (m_gsVsRingBufDescs.size() <= streamId)
    m_gsVsRingBufDescs.resize(streamId + 1);
  if (!m_gsVsRingBufDescs[streamId]) {
    // Ensure we have got the global table pointer first, and insert new code after that.
    BuilderBase builder(getInternalGlobalTablePtr()->getNextNode());

    if (m_shaderStage == ShaderStageGeometry) {
      const auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

      // Geometry shader, using GS-VS ring for output.
      Value *desc = loadDescFromDriverTable(SiDrvTableGsRingOuT0Offs + streamId, builder);

      unsigned outLocStart = 0;
      for (int i = 0; i < streamId; ++i)
        outLocStart += resUsage->inOutUsage.gs.outLocCount[i];

      // streamSize[streamId] = outLocCount[streamId] * 4 * sizeof(unsigned)
      // streamOffset = (streamSize[0] + ... + streamSize[streamId - 1]) * 64 * outputVertices
      unsigned baseAddr = outLocStart * m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices *
                          sizeof(unsigned) * 4 * 64;

      // Patch GS-VS ring buffer descriptor base address for GS output
      Value *gsVsOutRingBufDescElem0 = builder.CreateExtractElement(desc, (uint64_t)0);
      gsVsOutRingBufDescElem0 = builder.CreateAdd(gsVsOutRingBufDescElem0, builder.getInt32(baseAddr));
      desc = builder.CreateInsertElement(desc, gsVsOutRingBufDescElem0, (uint64_t)0);

      // Patch GS-VS ring buffer descriptor stride for GS output
      Value *gsVsRingBufDescElem1 = builder.CreateExtractElement(desc, (uint64_t)1);

      // Clear stride in SRD dword1
      SqBufRsrcWord1 strideClearMask = {};
      strideClearMask.u32All = UINT32_MAX;
      strideClearMask.bits.stride = 0;
      gsVsRingBufDescElem1 = builder.CreateAnd(gsVsRingBufDescElem1, builder.getInt32(strideClearMask.u32All));

      // Calculate and set stride in SRD dword1
      unsigned gsVsStride = m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices *
                            resUsage->inOutUsage.gs.outLocCount[streamId] * sizeof(unsigned) * 4;

      SqBufRsrcWord1 strideSetValue = {};
      strideSetValue.bits.stride = gsVsStride;
      gsVsRingBufDescElem1 = builder.CreateOr(gsVsRingBufDescElem1, builder.getInt32(strideSetValue.u32All));

      desc = builder.CreateInsertElement(desc, gsVsRingBufDescElem1, (uint64_t)1);

      if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 8) {
        // NOTE: For GFX8+, we have to explicitly set DATA_FORMAT for GS-VS ring buffer descriptor.
        desc = setRingBufferDataFormat(desc, BUF_DATA_FORMAT_32, builder);
      }

      m_gsVsRingBufDescs[streamId] = desc;
    } else {
      // Copy shader, using GS-VS ring for input.
      m_gsVsRingBufDescs[streamId] = loadDescFromDriverTable(SiDrvTableVsRingInOffs, builder);
    }
  }
  return m_gsVsRingBufDescs[streamId];
}

// =====================================================================================================================
// Get pointers to emit counters (GS)
std::pair<Type *, ArrayRef<Value *>> ShaderSystemValues::getEmitCounterPtr() {
  assert(m_shaderStage == ShaderStageGeometry);
  auto *emitCounterTy = Type::getInt32Ty(*m_context);
  if (m_emitCounterPtrs.empty()) {
    // TODO: We should only insert those offsets required by the specified input primitive.

    // Setup GS emit vertex counter
    auto &dataLayout = m_entryPoint->getParent()->getDataLayout();
    auto insertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();
    for (int i = 0; i < MaxGsStreams; ++i) {
      auto emitCounterPtr = new AllocaInst(emitCounterTy, dataLayout.getAllocaAddrSpace(), "", insertPos);
      new StoreInst(ConstantInt::get(emitCounterTy, 0), emitCounterPtr, insertPos);
      m_emitCounterPtrs.push_back(emitCounterPtr);
    }
  }
  return std::make_pair(emitCounterTy, ArrayRef<Value *>(m_emitCounterPtrs));
}

// =====================================================================================================================
// Get internal global table pointer as pointer to i8.
Instruction *ShaderSystemValues::getInternalGlobalTablePtr() {
  if (!m_internalGlobalTablePtr) {
    auto ptrTy = Type::getInt8Ty(*m_context)->getPointerTo(ADDR_SPACE_CONST);
    // Global table is always the first function argument (separate shader) or the eighth function argument (merged
    // shader). And mesh shader is actually mapped to ES-GS merged shader.
    m_internalGlobalTablePtr = makePointer(
        getFunctionArgument(m_entryPoint, getShaderStage(m_entryPoint) == ShaderStageMesh ? NumSpecialSgprInputs : 0,
                            "globalTable"),
        ptrTy, InvalidValue);
  }
  return m_internalGlobalTablePtr;
}

// =====================================================================================================================
// Get the mesh pipeline statistics buffer pointer as pointer to i8
Value *ShaderSystemValues::getMeshPipeStatsBufPtr() {
  assert(m_pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  assert(m_shaderStage == ShaderStageTask || m_shaderStage == ShaderStageMesh);
  if (!m_meshPipeStatsBufPtr) {
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
    unsigned entryArgIdx = 0;

    // Get the SGPR number of the mesh pipeline statistics buffer pointer.
    switch (m_shaderStage) {
    case ShaderStageTask:
      entryArgIdx = intfData->entryArgIdxs.task.pipeStatsBuf;
      break;
    case ShaderStageMesh:
      entryArgIdx = intfData->entryArgIdxs.mesh.pipeStatsBuf;
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }
    assert(entryArgIdx != 0);

    auto ptrTy = Type::getInt8Ty(*m_context)->getPointerTo(ADDR_SPACE_GLOBAL);
    m_meshPipeStatsBufPtr =
        makePointer(getFunctionArgument(m_entryPoint, entryArgIdx, "meshPipeStatsBuf"), ptrTy, InvalidValue);
  }
  return m_meshPipeStatsBufPtr;
}

// =====================================================================================================================
// Get stream-out buffer descriptor
//
// @param xfbBuffer : Transform feedback buffer ID
Value *ShaderSystemValues::getStreamOutBufDesc(unsigned xfbBuffer) {
  assert(xfbBuffer < MaxTransformFeedbackBuffers);
  if (m_streamOutBufDescs.size() <= xfbBuffer)
    m_streamOutBufDescs.resize(xfbBuffer + 1);

  if (!m_streamOutBufDescs[xfbBuffer]) {
    auto streamOutTablePair = getStreamOutTablePtr();
    auto streamOutTablePtr = streamOutTablePair.second;
    auto insertPos = streamOutTablePtr->getNextNode();

    Value *idxs[] = {ConstantInt::get(Type::getInt64Ty(*m_context), 0),
                     ConstantInt::get(Type::getInt64Ty(*m_context), xfbBuffer)};

    auto streamOutTableType = streamOutTablePair.first;
    auto streamOutBufDescPtr = GetElementPtrInst::Create(streamOutTableType, streamOutTablePtr, idxs, "", insertPos);
    streamOutBufDescPtr->setMetadata(MetaNameUniform, MDNode::get(streamOutBufDescPtr->getContext(), {}));
    auto streamOutBufDescTy = GetElementPtrInst::getIndexedType(streamOutTableType, idxs);

    auto streamOutBufDesc = new LoadInst(streamOutBufDescTy, streamOutBufDescPtr, "", false, Align(16), insertPos);
    streamOutBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(streamOutBufDesc->getContext(), {}));

    m_streamOutBufDescs[xfbBuffer] = streamOutBufDesc;
  }
  return m_streamOutBufDescs[xfbBuffer];
}

// =====================================================================================================================
// Get stream-out buffer table pointer
std::pair<Type *, Instruction *> ShaderSystemValues::getStreamOutTablePtr() {
  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageCopyShader);

  auto *streamOutTableTy =
      ArrayType::get(FixedVectorType::get(Type::getInt32Ty(*m_context), 4), MaxTransformFeedbackBuffers);
  if (!m_streamOutTablePtr) {
    auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
    unsigned entryArgIdx = InvalidValue;

    // Get the SGPR number of the stream-out table pointer.
    switch (m_shaderStage) {
    case ShaderStageVertex:
      entryArgIdx = intfData->entryArgIdxs.vs.streamOutData.tablePtr;
      break;
    case ShaderStageTessEval:
      entryArgIdx = intfData->entryArgIdxs.tes.streamOutData.tablePtr;
      break;
    case ShaderStageCopyShader:
      entryArgIdx = intfData->userDataUsage.gs.copyShaderStreamOutTable;
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }
    assert(entryArgIdx != InvalidValue);

    // Get the 64-bit extended node value.
    auto streamOutTablePtrLow = getFunctionArgument(m_entryPoint, entryArgIdx, "streamOutTable");
    auto streamOutTablePtrTy = PointerType::get(streamOutTableTy, ADDR_SPACE_CONST);
    m_streamOutTablePtr = makePointer(streamOutTablePtrLow, streamOutTablePtrTy, InvalidValue);
  }
  return std::make_pair(streamOutTableTy, m_streamOutTablePtr);
}

// =====================================================================================================================
// Make 64-bit pointer of specified type from 32-bit int, extending with the specified value, or PC if InvalidValue
//
// @param lowValue : 32-bit int value to extend
// @param ptrTy : Type that result pointer needs to be
// @param highValue : Value to use for high part, or InvalidValue to use PC
Instruction *ShaderSystemValues::makePointer(Value *lowValue, Type *ptrTy, unsigned highValue) {
  // Insert extending code after lowValue if it is an instruction.
  Instruction *insertPos = nullptr;
  auto lowValueInst = dyn_cast<Instruction>(lowValue);
  if (lowValueInst)
    insertPos = lowValueInst->getNextNode();
  else
    insertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();

  Value *extendedPtrValue = nullptr;
  if (highValue == InvalidValue) {
    // Use PC.
    if (!m_pc || isa<Instruction>(lowValue)) {
      // Either
      // 1. there is no existing code to s_getpc and cast it, or
      // 2. there is existing code, but lowValue is an instruction, so it is more complex to figure
      //    out whether it is before or after lowValue in the code. We generate new s_getpc code anyway
      //    and rely on subsequent CSE to common it up.
      // Insert the s_getpc code at the start of the function, so a later call into here knows it can
      // reuse this PC if its lowValue is an arg rather than an instruction.
      auto pcInsertPos = &*m_entryPoint->front().getFirstNonPHIOrDbgOrAlloca();
      Value *pc = emitCall("llvm.amdgcn.s.getpc", Type::getInt64Ty(*m_context), ArrayRef<Value *>(), {}, pcInsertPos);
      m_pc = new BitCastInst(pc, FixedVectorType::get(Type::getInt32Ty(*m_context), 2), "", insertPos);
    } else
      insertPos = m_pc->getNextNode();
    extendedPtrValue = m_pc;
  } else {
    // Use constant highValue value.
    Constant *elements[] = {PoisonValue::get(lowValue->getType()), ConstantInt::get(lowValue->getType(), highValue)};
    extendedPtrValue = ConstantVector::get(elements);
  }
  extendedPtrValue = InsertElementInst::Create(extendedPtrValue, lowValue,
                                               ConstantInt::get(Type::getInt32Ty(*m_context), 0), "", insertPos);
  extendedPtrValue =
      CastInst::Create(Instruction::BitCast, extendedPtrValue, Type::getInt64Ty(*m_context), "", insertPos);
  return CastInst::Create(Instruction::IntToPtr, extendedPtrValue, ptrTy, "", insertPos);
}

// =====================================================================================================================
// Load descriptor from driver table.
// If the caller sets builder's insert point to the start of the function, it should ensure that it first calls
// getInternalGlobalTablePtr(). Otherwise there is a danger that code is inserted in the wrong order, giving
// invalid IR.
//
// @param tableOffset : Byte offset in driver table
// @param builder : Builder to use for insertion
Instruction *ShaderSystemValues::loadDescFromDriverTable(unsigned tableOffset, BuilderBase &builder) {
  auto globalTable = getInternalGlobalTablePtr();
  Type *descTy = FixedVectorType::get(builder.getInt32Ty(), 4);
  globalTable = cast<Instruction>(builder.CreateBitCast(globalTable, descTy->getPointerTo(ADDR_SPACE_CONST)));
  Value *descPtr = builder.CreateGEP(descTy, globalTable, builder.getInt32(tableOffset));
  LoadInst *desc = builder.CreateLoad(descTy, descPtr);
  return desc;
}

// =====================================================================================================================
// Explicitly set the DATA_FORMAT of ring buffer descriptor.
//
// @param bufDesc : Buffer Descriptor
// @param dataFormat : Data format
// @param builder : Builder to use for inserting instructions
Value *ShaderSystemValues::setRingBufferDataFormat(Value *bufDesc, unsigned dataFormat, BuilderBase &builder) const {
  Value *elem3 = builder.CreateExtractElement(bufDesc, (uint64_t)3);

  SqBufRsrcWord3 dataFormatClearMask;
  dataFormatClearMask.u32All = UINT32_MAX;
  // TODO: This code needs to be fixed for gfx10; buffer format is handled differently.
  dataFormatClearMask.gfx6.dataFormat = 0;
  elem3 = builder.CreateAnd(elem3, builder.getInt32(dataFormatClearMask.u32All));

  SqBufRsrcWord3 dataFormatSetValue = {};
  dataFormatSetValue.gfx6.dataFormat = dataFormat;
  elem3 = builder.CreateOr(elem3, builder.getInt32(dataFormatSetValue.u32All));

  return builder.CreateInsertElement(bufDesc, elem3, (uint64_t)3);
}

// =====================================================================================================================
// Find resource node by descriptor set ID
//
// @param descSet : Descriptor set to find
unsigned ShaderSystemValues::findResourceNodeByDescSet(unsigned descSet) {
  auto userDataNodes = m_pipelineState->getUserDataNodes();
  for (unsigned i = 0; i < userDataNodes.size(); ++i) {
    auto node = &userDataNodes[i];
    if (node->concreteType == ResourceNodeType::DescriptorTableVaPtr && node->innerTable[0].set == descSet)
      return i;
  }
  return InvalidValue;
}

// =====================================================================================================================
// Test if shadow descriptor table is enabled
bool ShaderSystemValues::isShadowDescTableEnabled() const {
  return m_pipelineState->getOptions().highAddrOfFmask != ShadowDescriptorTableDisable;
}
