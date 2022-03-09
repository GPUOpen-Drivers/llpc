/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MeshTaskShader.cpp
 * @brief LLPC source file: contains implementation of class lgc::MeshTaskShader.
 ***********************************************************************************************************************
 */
#include "MeshTaskShader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-mesh-task-shader"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Constructor
//
// @param pipelineState : Pipeline state
MeshTaskShader::MeshTaskShader(PipelineState *pipelineState)
    : m_pipelineState(pipelineState), m_builder(std::make_unique<IRBuilder<>>(pipelineState->getContext())) {
  assert(pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  m_pipelineSysValues.initialize(m_pipelineState);
}

// =====================================================================================================================
// Destructor
MeshTaskShader::~MeshTaskShader() {
  m_pipelineSysValues.clear();
}

// =====================================================================================================================
// Process the mesh/task shader lowering.
//
// @param taskEntryPoint : Entry-point of task shader (could be null)
// @param meshEntryPoint : Entry-point of mesh shader (could be null)
void MeshTaskShader::process(Function *taskEntryPoint, Function *meshEntryPoint) {
  if (taskEntryPoint)
    processTaskShader(taskEntryPoint);

  if (meshEntryPoint)
    processMeshShader(meshEntryPoint);
}

// =====================================================================================================================
// Process task shader lowering.
//
// @param entryPoint : Entry-point of task shader
void MeshTaskShader::processTaskShader(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  //
  // NOTE: The processing is something like this:
  //
  // Task_Shader() {
  //   Initialize thread/wave info
  //
  //   Task shader main body (from API shader, lower task payload read/write)
  //
  //   Barrier
  //   if (threadIdInSubgroup == 0) {
  //     Write data to mesh pipeline statistics buffer
  //
  //     Lower EmitMeshTasks, write data to task draw data ring buffer
  //   }
  // }
  //
  m_builder->SetInsertPoint(&*entryPoint->front().getFirstInsertionPt());
  initWaveThreadInfo(entryPoint);

  SmallVector<CallInst *, 8> removedCalls;

  auto module = entryPoint->getParent();
  for (auto &func : module->functions()) {
    if (!func.isDeclaration())
      continue; // Not targets

    const bool isReadTaskPayload = func.getName().startswith(lgcName::MeshTaskReadTaskPayload);
    const bool isWriteTaskPayload = func.getName().startswith(lgcName::MeshTaskWriteTaskPayload);
    const bool isEmitMeshTasks = func.getName().startswith(lgcName::MeshTaskEmitMeshTasks);

    if (isReadTaskPayload || isWriteTaskPayload || isEmitMeshTasks) {
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);

        if (call->getFunction() != entryPoint)
          continue; // Not belong to task shader

        m_builder->SetInsertPoint(call);

        if (isReadTaskPayload) {
          // Read task payload
          assert(call->arg_size() == 1);
          auto byteOffset = call->getOperand(0);

          auto readValue = readTaskPayload(entryPoint, call->getType(), byteOffset);
          call->replaceAllUsesWith(readValue);
          m_accessTaskPayload = true;
        } else if (isWriteTaskPayload) {
          // Write task payload
          assert(call->arg_size() == 2);
          auto byteOffset = call->getOperand(0);
          auto writeValue = call->getOperand(1);

          writeTaskPayload(entryPoint, writeValue, byteOffset);
          m_accessTaskPayload = true;
        } else {
          // Emit mesh tasks
          assert(isEmitMeshTasks);
          assert(call->arg_size() == 3);
          auto groupCountX = call->getOperand(0);
          auto groupCountY = call->getOperand(1);
          auto groupCountZ = call->getOperand(2);

          emitTaskMeshs(entryPoint, groupCountX, groupCountY, groupCountZ);
        }

        removedCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Process mesh shader lowering.
//
// @param entryPoint : Entry-point of mesh shader
void MeshTaskShader::processMeshShader(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageMesh);

  // TODO: Add mesh shader support.
  llvm_unreachable("Not implemented!");
}

// =====================================================================================================================
// Process the read of task payload.
//
// @param entryPoint : Entry-point of task shader
// @param readTy : Type of value to read
// @param byteOffset : Byte offset within the payload entry
Value *MeshTaskShader::readTaskPayload(Function *entryPoint, Type *readTy, Value *byteOffset) {
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  CoherentFlag coherent = {};
  coherent.bits.glc = true;
  coherent.bits.dlc = true;

  return m_builder->CreateIntrinsic(
      Intrinsic::amdgcn_raw_buffer_load, readTy,
      {payloadRingBufDesc, byteOffset, payloadRingEntryOffset, m_builder->getInt32(coherent.u32All)});
}

// =====================================================================================================================
// Process the write of task payload.
//
// @param entryPoint : Entry-point of task shader
// @param writeValue : Value to write
// @param byteOffset : Byte offset within the payload entry
void MeshTaskShader::writeTaskPayload(Function *entryPoint, Value *writeValue, Value *byteOffset) {
  assert(getShaderStage(entryPoint) == ShaderStageTask);

  auto payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();
  auto payloadRingEntryOffset = getPayloadRingEntryOffset(entryPoint);

  CoherentFlag coherent = {};
  coherent.bits.glc = true;

  m_builder->CreateIntrinsic(
      Intrinsic::amdgcn_raw_buffer_store, writeValue->getType(),
      {writeValue, payloadRingBufDesc, byteOffset, payloadRingEntryOffset, m_builder->getInt32(coherent.u32All)});
}

// =====================================================================================================================
// Initialize the wave/thread info from the entry-point.
//
// @param entryPoint : Shader entry-point
void MeshTaskShader::initWaveThreadInfo(Function *entryPoint) {
  if (getShaderStage(entryPoint) == ShaderStageTask) {
    // Task shader
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTask)->entryArgIdxs.task;

    // waveId = dispatchInfo[24:20]
    m_waveThreadInfo.waveIdInSubgroup = m_builder->CreateAnd(
        m_builder->CreateLShr(getFunctionArgument(entryPoint, entryArgIdxs.multiDispatchInfo), 20), 0x1F);

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTask);

    m_waveThreadInfo.threadIdInWave =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder->getInt32(-1), m_builder->getInt32(0)});
    if (waveSize == 64) {
      m_waveThreadInfo.threadIdInWave = m_builder->CreateIntrinsic(
          Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder->getInt32(-1), m_waveThreadInfo.threadIdInWave});
    }

    m_waveThreadInfo.threadIdInSubgroup =
        m_builder->CreateAdd(m_builder->CreateMul(m_waveThreadInfo.waveIdInSubgroup, m_builder->getInt32(waveSize)),
                             m_waveThreadInfo.threadIdInWave);
  } else {
    // Mesh shader
    assert(getShaderStage(entryPoint) == ShaderStageMesh);

    // TODO: Add mesh shader support.
    llvm_unreachable("Not implemented!");
  }
}

// =====================================================================================================================
// Get shader ring entry index of current workgroup from the entry-point.
//
// @param entryPoint : Shader entry-point
// @returns : The shader ring entry index of current workgroup
Value *MeshTaskShader::getShaderRingEntryIndex(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  if (!m_shaderRingEntryIndex) {
    // NOTE: The calculation of shader ring entry index should be done at the beginning of the entry block. And the
    // value could be reused in subsequent operations.
    auto savedInsertPoint = m_builder->saveIP();
    m_builder->SetInsertPoint(&*entryPoint->getEntryBlock().getFirstInsertionPt());

    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageTask)->entryArgIdxs.task;

    auto workgroupId = getFunctionArgument(entryPoint, entryArgIdxs.workgroupId);
    auto dispatchDims = getFunctionArgument(entryPoint, entryArgIdxs.dispatchDims);

    // flattenWorkgroupId = workgroupId.z * dispatchDims.x * dispatchDims.y +
    //                      workgroupId.y * dispatchDims.x + workgroupId.x
    //                    = (workgroupId.z * dispatchDims.y + workgroupId.y) * dispatchDims.x + workgroupId.x
    auto flattenWorkgroupId = m_builder->CreateMul(m_builder->CreateExtractElement(workgroupId, 2),
                                                   m_builder->CreateExtractElement(dispatchDims, 1));
    flattenWorkgroupId = m_builder->CreateAdd(flattenWorkgroupId, m_builder->CreateExtractElement(workgroupId, 1));
    flattenWorkgroupId = m_builder->CreateMul(flattenWorkgroupId,
                                              m_builder->CreateExtractElement(dispatchDims, static_cast<uint64_t>(0)));
    flattenWorkgroupId = m_builder->CreateAdd(flattenWorkgroupId,
                                              m_builder->CreateExtractElement(workgroupId, static_cast<uint64_t>(0)));

    auto baseRingEntryIndex = getFunctionArgument(entryPoint, entryArgIdxs.baseRingEntryIndex);
    m_shaderRingEntryIndex = m_builder->CreateAdd(baseRingEntryIndex, flattenWorkgroupId);

    m_builder->restoreIP(savedInsertPoint);
  }

  return m_shaderRingEntryIndex;
}

// =====================================================================================================================
// Get the payload ring entry offset of current workgroup for task shader.
//
// @param entryPoint : Entry-point of task shader
// @returns : The payload ring entry offset of current workgroup
Value *MeshTaskShader::getPayloadRingEntryOffset(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  if (!m_payloadRingEntryOffset) {
    Value *ringEntryIndex = getShaderRingEntryIndex(entryPoint);
    Value *payloadRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskPayloadRingBufDesc();

    // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
    Value *numPayloadRingEntries = m_builder->CreateUDiv(m_builder->CreateExtractElement(payloadRingBufDesc, 2),
                                                         m_builder->getInt32(PayloadRingEntrySize));
    // wrappedRingEntryIndex = ringEntryIndex % numRingEntries = ringEntryIndex & (numRingEntries - 1)
    Value *wrappedRingEntryIndex =
        m_builder->CreateAnd(ringEntryIndex, m_builder->CreateSub(numPayloadRingEntries, m_builder->getInt32(1)));
    m_payloadRingEntryOffset = m_builder->CreateMul(wrappedRingEntryIndex, m_builder->getInt32(PayloadRingEntrySize));
  }

  return m_payloadRingEntryOffset;
}

// =====================================================================================================================
// Get the draw data ring entry offset of current workgroup for task shader.
//
// @param entryPoint : Entry-point of task shader
// @returns : The draw data ring entry offset of current workgroup
Value *MeshTaskShader::getDrawDataRingEntryOffset(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  if (!m_drawDataRingEntryOffset) {
    Value *ringEntryIndex = getShaderRingEntryIndex(entryPoint);
    Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();

    // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
    Value *numDrawDataRingEntries = m_builder->CreateUDiv(m_builder->CreateExtractElement(drawDataRingBufDesc, 2),
                                                          m_builder->getInt32(DrawDataRingEntrySize));
    // wrappedRingEntryIndex = ringEntryIndex % numRingEntries = ringEntryIndex & (numRingEntries - 1)
    Value *wrappedRingEntryIndex =
        m_builder->CreateAnd(ringEntryIndex, m_builder->CreateSub(numDrawDataRingEntries, m_builder->getInt32(1)));
    m_drawDataRingEntryOffset = m_builder->CreateMul(wrappedRingEntryIndex, m_builder->getInt32(DrawDataRingEntrySize));
  }

  return m_drawDataRingEntryOffset;
}

// =====================================================================================================================
// Get the draw data ready bit.
//
// @param entryPoint : Entry-point of task shader
// @returns : Flag (i1 typed) indicating whether the draw data is ready for command processor (CP) to fetch.
Value *MeshTaskShader::getDrawDataReadyBit(Function *entryPoint) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  if (!m_drawDataReadyBit) {
    Value *ringEntryIndex = getShaderRingEntryIndex(entryPoint);
    Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();

    // NUM_RECORDS = SQ_BUF_RSRC_WORD2[31:0]
    Value *numDrawDataRingEntries = m_builder->CreateUDiv(m_builder->CreateExtractElement(drawDataRingBufDesc, 2),
                                                          m_builder->getInt32(DrawDataRingEntrySize));
    // readyBit = ringEntryIndex & numRingEnties != 0
    m_drawDataReadyBit =
        m_builder->CreateICmpNE(m_builder->CreateAnd(ringEntryIndex, numDrawDataRingEntries), m_builder->getInt32(0));
  }

  return m_drawDataReadyBit;
}

// =====================================================================================================================
// Emit mesh tasks. Defines the dimension size of subsequent mesh shader workgroups to generate upon completion of the
// task shader workgroup
//
// @param entryPoint : Entry-point of task shader
// @param groupCountX : Number of local workgroups in X dimension for the launch of child mesh tasks
// @param groupCountX : Number of local workgroups in Y dimension for the launch of child mesh tasks
// @param groupCountX : Number of local workgroups in Z dimension for the launch of child mesh tasks
void MeshTaskShader::emitTaskMeshs(Function *entryPoint, Value *groupCountX, Value *groupCountY, Value *groupCountZ) {
  assert(getShaderStage(entryPoint) == ShaderStageTask); // Must be task shader

  auto emitMeshsCall = m_builder->GetInsertPoint();

  auto checkEmitMeshsBlock = m_builder->GetInsertBlock();
  auto emitMeshsBlock = checkEmitMeshsBlock->splitBasicBlock(emitMeshsCall, ".emitMeshs");
  auto endEmitMeshsBlock = emitMeshsBlock->splitBasicBlock(emitMeshsCall, ".endEmitMeshs");

  // Modify ".checkEmitMeshs" block
  {
    m_builder->SetInsertPoint(checkEmitMeshsBlock->getTerminator());

    if (m_accessTaskPayload) {
      // Make sure the task payload read/write access is completed
      m_builder->CreateFence(AtomicOrdering::Release, SyncScope::System);
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
    }

    auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_waveThreadInfo.threadIdInSubgroup, m_builder->getInt32(0));
    m_builder->CreateCondBr(firstThreadInSubgroup, emitMeshsBlock, endEmitMeshsBlock);
    checkEmitMeshsBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".emitTaskMeshs" block
  {
    m_builder->SetInsertPoint(emitMeshsBlock->getTerminator());

    //
    // Collect task statistics info
    //
    auto &computeMode =
        m_pipelineState->getShaderModes()->getComputeShaderMode(); // Task shader is actually a compute shader
    const uint64_t numTaskThreads =
        computeMode.workgroupSizeX * computeMode.workgroupSizeY * computeMode.workgroupSizeZ;

    Value *meshPipeStatsBufPtr = m_pipelineSysValues.get(entryPoint)->getMeshPipeStatsBufPtr();
    Value *meshPipeStatsBufEntryPtr = m_builder->CreateGEP(
        m_builder->getInt8Ty(), meshPipeStatsBufPtr, m_builder->getInt32(offsetof(MeshPipeStatsEntry, numTaskThreads)));
    meshPipeStatsBufEntryPtr = m_builder->CreateBitCast(meshPipeStatsBufEntryPtr,
                                                        PointerType::get(m_builder->getInt64Ty(), ADDR_SPACE_GLOBAL));

    // NOTE: LLVM backend will try to apply atomics optimization. But here, we only have one active thread to execute
    // the global_atomic_add instruction. Thus, the optimization is completely unnecessary. To avoid this, we try to
    // move the added value to VGPR to mark it as "divergent".
    Value *valueToAdd = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 2));
    valueToAdd = m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(numTaskThreads)),
                                                static_cast<uint64_t>(0));
    valueToAdd =
        m_builder->CreateInsertElement(valueToAdd, convertToDivergent(m_builder->getInt32(numTaskThreads >> 32)), 1);
    valueToAdd = m_builder->CreateBitCast(valueToAdd, m_builder->getInt64Ty());

    m_builder->CreateAtomicRMW(AtomicRMWInst::Add, meshPipeStatsBufEntryPtr, valueToAdd, MaybeAlign(),
                               AtomicOrdering::Monotonic, SyncScope::System);

    //
    // Write draw data
    //

    // Set X dimension to 0 if any of X, Y, Z dimension is 0:
    //   groupCountX = min(groupCountY, groupCountZ) == 0 ? 0 : groupCountX
    auto minGroupCountYZ =
        m_builder->CreateIntrinsic(Intrinsic::umin, groupCountY->getType(), {groupCountY, groupCountZ});
    groupCountX = m_builder->CreateSelect(m_builder->CreateICmpEQ(minGroupCountYZ, m_builder->getInt32(0)),
                                          m_builder->getInt32(0), groupCountX);

    Value *drawDataRingBufDesc = m_pipelineSysValues.get(entryPoint)->getTaskDrawDataRingBufDesc();
    Value *drawDataRingEntryOffset = getDrawDataRingEntryOffset(entryPoint);

    // Draw data (<4 x i32>) = <groupCountX, groupCountY, groupCountZ, readyBit>
    Value *drawData = UndefValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 4));
    drawData = m_builder->CreateInsertElement(drawData, groupCountX, static_cast<uint64_t>(0));
    drawData = m_builder->CreateInsertElement(drawData, groupCountY, 1);
    drawData = m_builder->CreateInsertElement(drawData, groupCountZ, 2);

    Value *readyBit = getDrawDataReadyBit(entryPoint);
    drawData = m_builder->CreateInsertElement(drawData, m_builder->CreateZExt(readyBit, m_builder->getInt32Ty()), 3);

    m_builder->CreateIntrinsic(
        Intrinsic::amdgcn_raw_buffer_store, drawData->getType(),
        {drawData, drawDataRingBufDesc, m_builder->getInt32(0), drawDataRingEntryOffset, m_builder->getInt32(0)});
  }

  // Construct ".endEmitTaskMeshs" block
  {
    m_builder->SetInsertPoint(endEmitMeshsBlock->getTerminator());

    // Currently, nothing to do
  }
}

// =====================================================================================================================
// Convert a i32 value to divergent one by inserting a "v_mov_b32" forcibly.
//
// @param value : Input i32 value
// @returns : A new i32 value that is considered to be divergent
Value *MeshTaskShader::convertToDivergent(Value *value) {
  assert(value->getType() == m_builder->getInt32Ty()); // Must be i32 typed
  auto inlineAsmTy = FunctionType::get(m_builder->getInt32Ty(), m_builder->getInt32Ty(), false);
  auto inlineAsm = InlineAsm::get(inlineAsmTy, "v_mov_b32 $0, $1", "=v,0", true);
  return m_builder->CreateCall(inlineAsm, value);
}

} // namespace lgc
