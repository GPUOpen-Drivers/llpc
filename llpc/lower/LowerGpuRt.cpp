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
 * @file  LowerGpuRt.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerGpuRt.
 ***********************************************************************************************************************
 */
#include "LowerGpuRt.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "llpc-gpurt"
using namespace lgc;
using namespace llvm;
using namespace Llpc;

namespace RtName {
static const char *LdsStack = "LdsStack";
} // namespace RtName

namespace Llpc {
// =====================================================================================================================
LowerGpuRt::LowerGpuRt() : m_stack(nullptr), m_stackTy(nullptr), m_lowerStack(false), m_rayStaticId(nullptr) {
}
// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerGpuRt::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-gpurt\n");
  SpirvLower::init(&module);
  auto gfxip = m_context->getPipelineContext()->getGfxIpVersion();
  // NOTE: rayquery of sect and ahit can reuse lds.
  m_lowerStack = (m_entryPoint->getName().startswith("_ahit") || m_entryPoint->getName().startswith("_sect")) &&
                 (gfxip.major < 11);
  createGlobalStack();
  createRayStaticIdValue();

  static auto visitor = llvm_dialects::VisitorBuilder<LowerGpuRt>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add(&LowerGpuRt::visitGetStackSize)
                            .add(&LowerGpuRt::visitGetStackBase)
                            .add(&LowerGpuRt::visitGetStackStride)
                            .add(&LowerGpuRt::visitStackWrite)
                            .add(&LowerGpuRt::visitStackRead)
                            .add(&LowerGpuRt::visitLdsStackInit)
                            .add(&LowerGpuRt::visitLdsStackStore)
                            .add(&LowerGpuRt::visitGetBoxSortHeuristicMode)
                            .add(&LowerGpuRt::visitGetStaticFlags)
                            .add(&LowerGpuRt::visitGetTriangleCompressionMode)
                            .add(&LowerGpuRt::visitGetFlattenedGroupThreadId)
                            .add(&LowerGpuRt::visitSetRayStaticId)
                            .add(&LowerGpuRt::visitGetRayStaticId)
                            .build();

  visitor.visit(*this, *m_module);

  for (Instruction *call : m_callsToLower) {
    call->dropAllReferences();
    call->eraseFromParent();
  }

  for (Function *func : m_funcsToLower) {
    func->dropAllReferences();
    func->eraseFromParent();
  }

  if (m_callsToLower.size())
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Get pipeline workgroup size for stack size calculation
unsigned LowerGpuRt::getWorkgroupSize() const {
  unsigned workgroupSize = 0;
  if (m_context->getPipelineType() == PipelineType::Graphics) {
    workgroupSize = m_context->getPipelineContext()->getRayTracingWaveSize();
  } else {
    ComputeShaderMode mode = lgc::Pipeline::getComputeShaderMode(*m_module);
    workgroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;
  }
  assert(workgroupSize != 0);
  if (m_context->getPipelineContext()->getGfxIpVersion().major >= 11) {
    // Round up to multiple of 32, as the ds_bvh_stack swizzle as 32 threads
    workgroupSize = alignTo(workgroupSize, 32);
  }
  return workgroupSize;
}

// =====================================================================================================================
// Get flat thread id in work group/wave
Value *LowerGpuRt::getThreadIdInGroup() const {
  // Todo: for graphics shader, subgroupId * waveSize + subgroupLocalInvocationId()
  unsigned builtIn = m_context->getPipelineType() == PipelineType::Graphics ? lgc::BuiltInSubgroupLocalInvocationId
                                                                            : lgc::BuiltInLocalInvocationIndex;
  lgc::InOutInfo inputInfo = {};
  return m_builder->CreateReadBuiltInInput(static_cast<lgc::BuiltInKind>(builtIn), inputInfo, nullptr, nullptr, "");
}

// =====================================================================================================================
// Create global variable for the stack
void LowerGpuRt::createGlobalStack() {
  auto ldsStackSize = getWorkgroupSize() * MaxLdsStackEntries;
  // Double anyhit and intersection shader lds size, these shader use lower part of stack to read/write value
  if (m_lowerStack)
    ldsStackSize = ldsStackSize << 1;

  m_stackTy = ArrayType::get(m_builder->getInt32Ty(), ldsStackSize);
  auto ldsStack = new GlobalVariable(*m_module, m_stackTy, false, GlobalValue::ExternalLinkage, nullptr,
                                     RtName::LdsStack, nullptr, GlobalValue::NotThreadLocal, 3);

  ldsStack->setAlignment(MaybeAlign(4));
  m_stack = ldsStack;
}

// =====================================================================================================================
// Create ray static ID value
void LowerGpuRt::createRayStaticIdValue() {
  m_builder->SetInsertPointPastAllocas(m_entryPoint);
  m_rayStaticId = m_builder->CreateAlloca(m_builder->getInt32Ty());
}

// =====================================================================================================================
// Visit "GpurtGetStackSizeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetStackSize(GpurtGetStackSizeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *size = nullptr;
  size = m_builder->getInt32(MaxLdsStackEntries * getWorkgroupSize());
  inst.replaceAllUsesWith(size);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetStackBaseOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetStackBase(GpurtGetStackBaseOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *base = getThreadIdInGroup();
  inst.replaceAllUsesWith(base);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetStackStrideOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetStackStride(GpurtGetStackStrideOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *stride = m_builder->getInt32(getWorkgroupSize());
  inst.replaceAllUsesWith(stride);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtStackReadOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitStackRead(GpurtStackReadOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *stackIndex = inst.getIndex();
  Type *stackTy = PointerType::get(m_builder->getInt32Ty(), 3);
  if (m_lowerStack) {
    auto ldsStackSize = m_builder->getInt32(getWorkgroupSize() * MaxLdsStackEntries);
    stackIndex = m_builder->CreateAdd(stackIndex, ldsStackSize);
  }

  Value *stackAddr = m_builder->CreateGEP(stackTy, m_stack, {stackIndex});
  Value *stackData = m_builder->CreateLoad(m_builder->getInt32Ty(), stackAddr);

  inst.replaceAllUsesWith(stackData);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtStackWriteOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitStackWrite(GpurtStackWriteOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *stackIndex = inst.getIndex();
  Value *stackData = inst.getValue();
  Type *stackTy = PointerType::get(m_builder->getInt32Ty(), 3);
  if (m_lowerStack) {
    auto ldsStackSize = m_builder->getInt32(getWorkgroupSize() * MaxLdsStackEntries);
    stackIndex = m_builder->CreateAdd(stackIndex, ldsStackSize);
  }

  auto stackArrayAddr = m_builder->CreateGEP(stackTy, m_stack, {stackIndex});
  m_builder->CreateStore(stackData, stackArrayAddr);

  inst.replaceAllUsesWith(m_builder->getInt32(0));
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtLdsStackInitOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitLdsStackInit(GpurtLdsStackInitOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *stackBasePerThread = getThreadIdInGroup();

  // From Navi3x on, Hardware has decided that the stacks are only swizzled across every 32 threads,
  // with stacks for every set of 32 threads stored after all the stack data for the previous 32 threads.
  if (getWorkgroupSize() > 32) {
    // localThreadId = (LinearLocalThreadID%32)
    // localGroupId = (LinearLocalThreadID/32)
    // stackSize = STACK_SIZE * 32 = m_stackEntries * 32
    // groupOf32ThreadSize = (LinearLocalThreadID/32) * stackSize
    // stackBasePerThread (in DW) = (LinearLocalThreadID%32)+(LinearLocalThreadID/32)*STACK_SIZE*32
    //                            = localThreadId + groupOf32ThreadSize
    Value *localThreadId = m_builder->CreateAnd(stackBasePerThread, m_builder->getInt32(31));
    Value *localGroupId = m_builder->CreateLShr(stackBasePerThread, m_builder->getInt32(5));
    Value *stackSize = m_builder->getInt32(MaxLdsStackEntries * 32);
    Value *groupOf32ThreadSize = m_builder->CreateMul(localGroupId, stackSize);
    stackBasePerThread = m_builder->CreateAdd(localThreadId, groupOf32ThreadSize);
  }

  Value *stackBaseAsInt = m_builder->CreatePtrToInt(
      m_builder->CreateGEP(m_stackTy, m_stack, {m_builder->getInt32(0), stackBasePerThread}), m_builder->getInt32Ty());

  // stack_addr[31:18] = stack_base[15:2]
  // stack_addr[17:0] = stack_index[17:0]
  // The low 18 bits of stackAddr contain stackIndex which we always initialize to 0.
  // Note that this relies on stackAddr being a multiple of 4, so that bits 17 and 16 are 0.
  Value *stackAddr = m_builder->CreateShl(stackBaseAsInt, 16);
  inst.replaceAllUsesWith(stackAddr);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtLdsStackStoreOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitLdsStackStore(GpurtLdsStackStoreOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *stackAddr = inst.getNewPos();
  Value *stackAddrVal = m_builder->CreateLoad(m_builder->getInt32Ty(), stackAddr);
  Value *lastVisited = inst.getOldPos();
  Value *data = inst.getData();
  // OFFSET = {OFFSET1, OFFSET0}
  // stack_size[1:0] = OFFSET1[5:4]
  // Stack size is encoded in the offset argument as:
  // 8 -> {0x00, 0x00}
  // 16 -> {0x10, 0x00}
  // 32 -> {0x20, 0x00}
  // 64 -> {0x30, 0x00}
  assert(MaxLdsStackEntries == 16);
  Value *offset = m_builder->getInt32((Log2_32(MaxLdsStackEntries) - 3) << 12);

  Value *result =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_ds_bvh_stack_rtn, {}, {stackAddrVal, lastVisited, data, offset});

  m_builder->CreateStore(m_builder->CreateExtractValue(result, 1), stackAddr);
  Value *ret = m_builder->CreateExtractValue(result, 0);
  inst.replaceAllUsesWith(ret);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetBoxSortHeuristicModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetBoxSortHeuristicMode(GpurtGetBoxSortHeuristicModeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rtState = m_context->getPipelineContext()->getRayTracingState();
  Value *boxSortHeuristicMode = m_builder->getInt32(rtState->boxSortHeuristicMode);
  inst.replaceAllUsesWith(boxSortHeuristicMode);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetStaticFlagsOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetStaticFlags(GpurtGetStaticFlagsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rtState = m_context->getPipelineContext()->getRayTracingState();
  Value *staticPipelineFlags = m_builder->getInt32(rtState->staticPipelineFlags);
  inst.replaceAllUsesWith(staticPipelineFlags);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetTriangleCompressionModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetTriangleCompressionMode(GpurtGetTriangleCompressionModeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rtState = m_context->getPipelineContext()->getRayTracingState();
  Value *triCompressMode = m_builder->getInt32(rtState->triCompressMode);
  inst.replaceAllUsesWith(triCompressMode);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetFlattenedGroupThreadIdOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetFlattenedGroupThreadId(GpurtGetFlattenedGroupThreadIdOp &inst) {
  m_builder->SetInsertPoint(&inst);
  inst.replaceAllUsesWith(getThreadIdInGroup());
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtSetRayStaticIdOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitSetRayStaticId(GpurtSetRayStaticIdOp &inst) {
  m_builder->SetInsertPoint(&inst);

  assert(m_rayStaticId);
  auto rayStaticId = inst.getId();
  auto storeInst = m_builder->CreateStore(rayStaticId, m_rayStaticId);

  inst.replaceAllUsesWith(storeInst);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetRayStaticIdOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetRayStaticId(GpurtGetRayStaticIdOp &inst) {
  m_builder->SetInsertPoint(&inst);

  assert(m_rayStaticId);
  auto rayStaticId = m_builder->CreateLoad(m_builder->getInt32Ty(), m_rayStaticId);
  inst.replaceAllUsesWith(rayStaticId);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

} // namespace Llpc