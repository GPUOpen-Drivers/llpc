/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerGpuRt.cpp
 * @brief LGC source file: contains implementation of class lgc::LowerGpuRt.
 ***********************************************************************************************************************
 */
#include "lgc/lowering/LowerGpuRt.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcContext.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-lower-gpurt"
using namespace lgc;
using namespace llvm;

namespace RtName {
static const char *LdsStack = "LdsStack";
} // namespace RtName

namespace lgc {
// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerGpuRt::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Gpurt\n");

  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  m_pipelineState = pipelineState;

  Builder builderImpl(pipelineState->getContext());
  m_builder = &builderImpl;
  createLdsStack(module);

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
                            .add(&LowerGpuRt::visitGetRayQueryDispatchId)
                            .add(&LowerGpuRt::visitGetStaticFlags)
                            .add(&LowerGpuRt::visitMakePc)
                            .add(&LowerGpuRt::visitGetTriangleCompressionMode)
                            .add(&LowerGpuRt::visitGetFlattenedGroupThreadId)
                            .add(&LowerGpuRt::visitFloatWithRoundMode)
                            .add(&LowerGpuRt::visitGpurtDispatchThreadIdFlatOp)
                            .add(&LowerGpuRt::visitWaveScanOp)
                            .add(&LowerGpuRt::visitGetKnownSetRayFlagsOp)
                            .add(&LowerGpuRt::visitGetKnownUnsetRayFlagsOp)
                            .add(&LowerGpuRt::visitInitStaticId)
                            .build();

  visitor.visit(*this, module);

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
// @param [in] Function : The function to retrieve shader information
unsigned LowerGpuRt::getWorkgroupSize(Function *func) const {
  unsigned workgroupSize = 0;
  auto stage = getShaderStage(func);
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(stage.value());
  if (stage == ShaderStage::Mesh) {
    auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
    workgroupSize = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
  } else if (stage == ShaderStage::Task || stage == ShaderStage::Compute) {
    ComputeShaderMode mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
    workgroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;
  } else {
    assert(m_pipelineState->isGraphics());
    workgroupSize = 64;
  }
  assert(workgroupSize != 0);

  workgroupSize = alignTo(workgroupSize, waveSize);

  return workgroupSize;
}

// =====================================================================================================================
// Get flat thread id in work group/wave
Value *LowerGpuRt::getThreadIdInGroup() const {
  auto stage = getShaderStage(m_builder->GetInsertBlock()->getParent());

  Value *laneId = m_builder->CreateReadBuiltInInput(BuiltInSubgroupLocalInvocationId, {}, nullptr, nullptr);
  if (stage != ShaderStage::Compute && stage != ShaderStage::Task && stage != ShaderStage::Mesh)
    return laneId;

  Value *waveId = m_builder->CreateReadBuiltInInput(BuiltInSubgroupId, {}, nullptr, nullptr);
  Value *tmp = m_builder->CreateMul(waveId, m_builder->getInt32(m_pipelineState->getShaderWaveSize(stage.value())));
  return m_builder->CreateAdd(tmp, laneId);
}

// =====================================================================================================================
// Update the workgroup size from different functions
// @param func : Function to get WorkgroupSize from
void LowerGpuRt::updateWorkgroupSize(Function *func) {
  unsigned funcWorkSize = getWorkgroupSize(func);
  m_workGroupSize = m_workGroupSize > funcWorkSize ? m_workGroupSize : funcWorkSize;
}

// =====================================================================================================================
// Create global variable for the lds stack
// @param [in/out] module : LLVM module to be run on
void LowerGpuRt::createLdsStack(Module &module) {
  struct Payload {
    bool needLdsStack;
    bool needExtraStack;
    LowerGpuRt *lowerRt;
  };
  Payload payload = {false, false, this};
  m_workGroupSize = 0;
  static auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<GpurtStackWriteOp>([](auto &payload, auto &op) {
                              payload.needLdsStack = true;
                              payload.needExtraStack |= op.getUseExtraStack();
                              payload.lowerRt->updateWorkgroupSize(op.getFunction());
                            })
                            .add<GpurtStackReadOp>([](auto &payload, auto &op) {
                              payload.needLdsStack = true;
                              payload.needExtraStack |= op.getUseExtraStack();
                              payload.lowerRt->updateWorkgroupSize(op.getFunction());
                            })
                            .add<GpurtLdsStackInitOp>([](auto &payload, auto &op) {
                              payload.needLdsStack = true;
                              payload.needExtraStack |= op.getUseExtraStack();
                              payload.lowerRt->updateWorkgroupSize(op.getFunction());
                            })
                            .build();
  visitor.visit(payload, module);

  if (payload.needLdsStack) {
    assert(m_workGroupSize > 0);
    auto ldsStackSize = m_workGroupSize * MaxLdsStackEntries;
    // Double LDS size when any operations requires to perform on extra stack.
    if (payload.needExtraStack)
      ldsStackSize = ldsStackSize << 1;

    m_stackTy = ArrayType::get(m_builder->getInt32Ty(), ldsStackSize);
    auto ldsStack = new GlobalVariable(module, m_stackTy, false, GlobalValue::ExternalLinkage, nullptr,
                                       RtName::LdsStack, nullptr, GlobalValue::NotThreadLocal, 3);

    ldsStack->setAlignment(MaybeAlign(4));
    m_stack = ldsStack;
  }
}

// =====================================================================================================================
// Visit "GpurtGetStackSizeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetStackSize(GpurtGetStackSizeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *size = nullptr;
  size = m_builder->getInt32(MaxLdsStackEntries * m_workGroupSize);
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
  Value *stride = m_builder->getInt32(m_workGroupSize);
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
  if (inst.getUseExtraStack()) {
    auto ldsStackSize = m_builder->getInt32(m_workGroupSize * MaxLdsStackEntries);
    stackIndex = m_builder->CreateAdd(stackIndex, ldsStackSize);
  }

  Value *stackAddr = m_builder->CreateGEP(m_builder->getInt32Ty(), m_stack, {stackIndex});
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
  if (inst.getUseExtraStack()) {
    auto ldsStackSize = m_builder->getInt32(m_workGroupSize * MaxLdsStackEntries);
    stackIndex = m_builder->CreateAdd(stackIndex, ldsStackSize);
  }

  auto stackArrayAddr = m_builder->CreateGEP(m_builder->getInt32Ty(), m_stack, {stackIndex});
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
  if (m_workGroupSize > 32) {
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

  if (inst.getUseExtraStack()) {
    auto ldsStackSize = m_builder->getInt32(m_workGroupSize * MaxLdsStackEntries);
    stackBasePerThread = m_builder->CreateAdd(stackBasePerThread, ldsStackSize);
  }

  Value *stackBaseAsInt = m_builder->CreatePtrToInt(
      m_builder->CreateGEP(m_stackTy, m_stack, {m_builder->getInt32(0), stackBasePerThread}), m_builder->getInt32Ty());

  Value *stackAddr;
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 12) {
    // stack_addr[29:15] = stack_base[15:2]
    // stack_addr[14:10] = stack_index[5:0]
    // Note that this relies on stackAddr being a multiple of 4, so that bits 15 and 14 are 0.
    // stackAddrDw = (stackAddr >> 2) << 15.
    stackAddr = m_builder->CreateShl(stackBaseAsInt, 13);
  } else {
    // stack_addr[31:18] = stack_base[15:2]
    // stack_addr[17:0] = stack_index[17:0]
    // The low 18 bits of stackAddr contain stackIndex which we always initialize to 0.
    // Note that this relies on stackAddr being a multiple of 4, so that bits 17 and 16 are 0.
    // stackAddrDw = (stackAddr >> 2) << 18.
    stackAddr = m_builder->CreateShl(stackBaseAsInt, 16);
  }

  inst.replaceAllUsesWith(stackAddr);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtFloatWithRoundModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitFloatWithRoundMode(lgc::GpurtFloatWithRoundModeOp &inst) {
  m_builder->SetInsertPoint(&inst);

  enum OperationType : uint32_t { Add = 0, Sub, Mul };
  auto func = inst.getCalledFunction();
  auto retType = func->getReturnType();
  Value *src0 = inst.getSrc0();
  Value *src1 = inst.getSrc1();
  uint32_t rm = cast<ConstantInt>(inst.getRoundMode())->getZExtValue();
  uint32_t op = cast<ConstantInt>(inst.getOperation())->getZExtValue();

  static constexpr RoundingMode rmTable[4] = {RoundingMode::NearestTiesToEven, RoundingMode::TowardPositive,
                                              RoundingMode::TowardNegative, RoundingMode::TowardZero};

  // Use llvm.set.rounding to modify rounding mode.
  m_builder->CreateIntrinsic(m_builder->getVoidTy(), Intrinsic::set_rounding,
                             {m_builder->getInt32(static_cast<unsigned>(rmTable[rm]))});

  Value *result = PoisonValue::get(retType);
  if (op == OperationType::Add)
    result = m_builder->CreateFAdd(src0, src1);
  else if (op == OperationType::Sub)
    result = m_builder->CreateFSub(src0, src1);
  else
    result = m_builder->CreateFMul(src0, src1);

  // Set back to RoundTiesToEven.
  m_builder->CreateIntrinsic(m_builder->getVoidTy(), Intrinsic::set_rounding,
                             {m_builder->getInt32(static_cast<unsigned>(RoundingMode::NearestTiesToEven))});

  inst.replaceAllUsesWith(result);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(func);
}

// =====================================================================================================================
// Visit "GpurtWaveScanOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitWaveScanOp(lgc::GpurtWaveScanOp &inst) {
  m_builder->SetInsertPoint(&inst);

  constexpr unsigned int Inclusive = 0x1;
  constexpr unsigned int Exclusive = 0x2;

  const BuilderDefs::GroupArithOp WaveScanOpTable[] = {
      BuilderDefs::Nop,  BuilderDefs::FAdd, BuilderDefs::IAdd, BuilderDefs::IAdd, BuilderDefs::FMul, BuilderDefs::IMul,
      BuilderDefs::IMul, BuilderDefs::FMin, BuilderDefs::SMin, BuilderDefs::UMin, BuilderDefs::FMax, BuilderDefs::SMax,
      BuilderDefs::UMax, BuilderDefs::Nop,  BuilderDefs::Nop,  BuilderDefs::Nop,
  };

  auto waveOpCode = cast<ConstantInt>(inst.getOperation())->getZExtValue();
  auto waveOpFlags = cast<ConstantInt>(inst.getFlags())->getZExtValue();
  Value *src0 = inst.getSrc0();

  BuilderDefs::GroupArithOp opCode = WaveScanOpTable[waveOpCode];

  assert((waveOpFlags == Inclusive) || (waveOpFlags == Exclusive));
  assert(opCode != BuilderDefs::Nop);

  Value *result = nullptr;
  if (waveOpFlags == Inclusive)
    result = m_builder->CreateSubgroupClusteredInclusive(opCode, src0, m_builder->CreateGetWaveSize());
  else if (waveOpFlags == Exclusive)
    result = m_builder->CreateSubgroupClusteredExclusive(opCode, src0, m_builder->CreateGetWaveSize());

  inst.replaceAllUsesWith(result);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtLdsStackStoreOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitLdsStackStore(GpurtLdsStackStoreOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *stackAddrVal = inst.getOldPos();
  Value *lastVisited = inst.getLastNode();
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
  // return struct {newNode, newStackAddr}
  Value *result =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_ds_bvh_stack_rtn, {}, {stackAddrVal, lastVisited, data, offset});

  inst.replaceAllUsesWith(result);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetBoxSortHeuristicModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetBoxSortHeuristicMode(GpurtGetBoxSortHeuristicModeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *boxSortHeuristicMode = m_builder->getInt32(m_pipelineState->getOptions().rtBoxSortHeuristicMode);
  inst.replaceAllUsesWith(boxSortHeuristicMode);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetRayQueryDispatchIdOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetRayQueryDispatchId(GpurtGetRayQueryDispatchIdOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto stage = getShaderStage(m_builder->GetInsertBlock()->getParent());
  // Local thread ID for graphics shader Stage, global thread ID for compute/raytracing shader stage
  Value *dispatchId = nullptr;
  if (stage != ShaderStage::Compute) {
    auto subThreadId = m_builder->CreateReadBuiltInInput(lgc::BuiltInSubgroupLocalInvocationId);
    Value *zero = m_builder->getInt32(0);
    dispatchId = m_builder->CreateBuildVector({subThreadId, zero, zero});
  } else {
    dispatchId = m_builder->CreateReadBuiltInInput(lgc::BuiltInGlobalInvocationId);
  }
  dispatchId->takeName(&inst);
  inst.replaceAllUsesWith(dispatchId);
  inst.eraseFromParent();
}

// =====================================================================================================================
// Visit "GpurtGetStaticFlagsOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetStaticFlags(GpurtGetStaticFlagsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *staticPipelineFlags = m_builder->getInt32(m_pipelineState->getOptions().rtStaticPipelineFlags);
  inst.replaceAllUsesWith(staticPipelineFlags);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtMakePcOp" instruction
//
// @param inst : The dialect instruction to process
// This generates the following IR for a call to @lgc.gpurt.make.pc(i32 %in32):
// clang-format off
// %pc64 = call i64 @llvm.amdgcn.s.getpc()
// %lshr = lshr i64 %pc64, 32
// %high32 = trunc i64 %lshr to i32
// %tmp1 = insertelement <2 x i32> poison, i32 %in32, i64 0
// %tmp2 = insertelement <2 x i32> %tmp1, i32 %high32, i64 1
// %tmp3 = bitcast <2 x i32> to i64
// If the return type of the dialect op is an <2 x i32>, it bitcasts the result again:
// %bc = bitcast i64 %tmp3 to <2 x i32>
// clang-format on
// The original call is then replaced with either %tmp3 or %bc, dependent on the return type of the original dialect op.
void LowerGpuRt::visitMakePc(GpurtMakePcOp &inst) {
  m_builder->SetInsertPoint(&inst);

  Value *highPc = m_builder->CreateIntrinsic(m_builder->getInt64Ty(), Intrinsic::amdgcn_s_getpc, {});
  highPc = m_builder->CreateTrunc(m_builder->CreateLShr(highPc, m_builder->getInt64(32)), m_builder->getInt32Ty());

  BasicBlock *bb = inst.getParent();
  AddressExtender addressExtender(bb->getParent(), bb);

  Value *addr32 = inst.getVa();
  Value *replacement = nullptr;
  replacement = addressExtender.extend(addr32, highPc, nullptr, *m_builder);

  // AddressExtender returns an i64 bitcast. Reconvert that to the vector return type if appropriate.
  replacement = m_builder->CreateBitCast(replacement, inst.getType());
  inst.replaceAllUsesWith(replacement);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetTriangleCompressionModeOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetTriangleCompressionMode(GpurtGetTriangleCompressionModeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *triCompressMode = m_builder->getInt32(m_pipelineState->getOptions().rtTriCompressMode);
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
// Visit "GpurtDispatchThreadIdFlatOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGpurtDispatchThreadIdFlatOp(GpurtDispatchThreadIdFlatOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto stage = getShaderStage(m_builder->GetInsertBlock()->getParent());
  Value *flatDispatchId = nullptr;
  if (stage == ShaderStage::Compute) {
    auto numGroup = m_builder->CreateReadBuiltInInput(lgc::BuiltInNumWorkgroups);
    auto groupSize = m_builder->CreateReadBuiltInInput(lgc::BuiltInWorkgroupSize);
    auto dispatchSize = m_builder->CreateMul(numGroup, groupSize);
    auto sizeX = m_builder->CreateExtractElement(dispatchSize, uint64_t(0));
    auto sizeY = m_builder->CreateExtractElement(dispatchSize, 1);
    auto sizeXY = m_builder->CreateMul(sizeX, sizeY);
    auto dispatchId = m_builder->CreateReadBuiltInInput(lgc::BuiltInGlobalInvocationId);
    auto dispatchX = m_builder->CreateExtractElement(dispatchId, uint64_t(0));
    auto dispatchY = m_builder->CreateExtractElement(dispatchId, 1);
    auto dispatchZ = m_builder->CreateExtractElement(dispatchId, 2);
    flatDispatchId = m_builder->CreateMul(dispatchZ, sizeXY);
    flatDispatchId = m_builder->CreateAdd(flatDispatchId, m_builder->CreateMul(dispatchY, sizeX));
    flatDispatchId = m_builder->CreateAdd(flatDispatchId, dispatchX);
  } else {
    flatDispatchId = getThreadIdInGroup();
  }

  inst.replaceAllUsesWith(flatDispatchId);
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetKnownSetRayFlagsOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetKnownSetRayFlagsOp(lgc::GpurtGetKnownSetRayFlagsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto flags = lgc::gpurt::getKnownSetRayFlags(*inst.getModule());
  inst.replaceAllUsesWith(m_builder->getInt32(flags));
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtGetKnownUnsetRayFlagsOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitGetKnownUnsetRayFlagsOp(lgc::GpurtGetKnownUnsetRayFlagsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto flags = lgc::gpurt::getKnownUnsetRayFlags(*inst.getModule());
  inst.replaceAllUsesWith(m_builder->getInt32(flags));
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visit "GpurtInitStaticIdOp" instruction
//
// @param inst : The dialect instruction to process
void LowerGpuRt::visitInitStaticId(lgc::GpurtInitStaticIdOp &inst) {
  inst.replaceAllUsesWith(m_builder->getInt32(
      llvm::hash_combine(m_pipelineState->getOptions().hash, inst.getModule()->getName(), m_rayStaticId++)));
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

} // namespace lgc
