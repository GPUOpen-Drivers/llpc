/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayTracing.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerRayTracing.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerRayTracing.h"
#include "SPIRVInternal.h"
#include "gpurt-compiler.h"
#include "lgcrt/LgcRtDialect.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/Pipeline.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-spirv-lower-ray-tracing"

namespace llvm {
namespace cl {
extern opt<bool> TrimDebugInfo;
} // namespace cl
} // namespace llvm

using namespace llvm;
using namespace Llpc;
using namespace lgc::rt;

namespace SPIRV {
extern const char *MetaNameSpirvOp;
} // namespace SPIRV

namespace RtName {
const char *TraceRayKHR = "_cs_";
const char *TraceRaySetTraceParams = "TraceRaySetTraceParams";
const char *ShaderTable = "ShaderTable";
static const char *HitAttribute = "HitAttribute";
static const char *ShaderRecordBuffer = "ShaderRecordBuffer";
static const char *GlobalPayload = "GlobalPayload";
static const char *GlobalCallableData = "GlobalCallableData";
static const char *IncomingPayLoad = "IncomingRayPayloadKHR";
static const char *IncomingCallableData = "IncomingCallableDataKHR";
static const char *CallAnyHitShader = "AmdTraceRayCallAnyHitShader";
static const char *FetchTrianglePositionFromNodePointer = "FetchTrianglePositionFromNodePointer";
static const char *RemapCapturedVaToReplayVa = "AmdTraceRayRemapCapturedVaToReplayVa";
} // namespace RtName

namespace Llpc {
static const unsigned TraceRayDescriptorSet = 93;                   // Descriptor set ID in traceRay binary
static const unsigned RayTracingResourceIndexDispatchRaysInfo = 17; // Dispatch ray info (constant buffer)
// TraceParams Type size in DWORD
static unsigned TraceParamsTySize[] = {
    1, // 1, rayFlags
    1, // 2, instanceInclusionMask
    3, // 3, origin
    1, // 4, tMin
    3, // 5, dir
    1, // 6, tMax
    1, // 7, tCurrent
    1, // 8, kind
    1, // 9, status
    1, // 10, instanceId
    1, // 11, instanceCusto
    1, // 12, primitiveInde
    1, // 13, duplicateAnyH
    1, // 14, geometryIndex
    8, // 15, hit attribute
    1, // 16, parentId
    9, // 16, HitTriangleVertexPositions
};

// =====================================================================================================================
SpirvLowerRayTracing::SpirvLowerRayTracing() : SpirvLowerRayQuery(false) {
}

// =====================================================================================================================
// Process a trace ray call by creating (or get if created) an implementation function and replace the call to it.
//
// @param inst : The original call instruction
void SpirvLowerRayTracing::processTraceRayCall(BaseTraceRayOp *inst) {
  std::string mangledName = inst->getCalledFunction()->getName().str() + ".impl";

  SmallVector<Value *, 12> implCallArgs(inst->args());

  implCallArgs.push_back(m_dispatchRaysInfoDesc);
  m_builder->SetInsertPoint(inst);

  auto newCall = m_builder->CreateNamedCall(mangledName, inst->getFunctionType()->getReturnType(), implCallArgs,
                                            {Attribute::NoUnwind, Attribute::AlwaysInline});

  inst->replaceAllUsesWith(newCall);

  auto func = m_module->getFunction(mangledName);

  if (func->isDeclaration()) {
    func->setLinkage(GlobalVariable::InternalLinkage);
    func->addFnAttr(Attribute::AlwaysInline);

    auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
    bool indirect = rayTracingContext->getIndirectStageMask() & ShaderStageComputeBit;

    auto entryBlock = BasicBlock::Create(*m_context, ".entry", func);
    m_builder->SetInsertPoint(entryBlock);

    auto payloadTy = rayTracingContext->getPayloadType(m_builder);
    Value *localPayload = m_builder->CreateAlloca(payloadTy, SPIRAS_Private);

    auto payloadArg = func->getArg(TraceRayParam::Payload);
    auto paqArray = func->getArg(TraceRayParam::Paq);
    auto bufferDesc = func->arg_end() - 1;
    auto payloadArgSize = m_builder->CreateExtractValue(paqArray, 0);
    const Align align = Align(4);
    m_builder->CreateMemCpy(localPayload, align, payloadArg, align, payloadArgSize);

    SmallVector<Value *, 8> args;
    args.push_back(m_builder->CreateLoad(payloadTy, localPayload));

    // For trace ray entry, we decided to use <2 x i32> to pass acceleration structure so that we can easily retrieve
    // high/low part by extractelement.
    args.push_back(m_builder->CreateBitCast(func->getArg(TraceRayParam::AccelStruct),
                                            FixedVectorType::get(m_builder->getInt32Ty(), 2)));

    for (unsigned i = TraceRayParam::RayFlags; i < TraceRayParam::Payload; i++)
      args.push_back(func->getArg(i));

    Value *currentParentRayId = nullptr;
    if (rayTracingContext->getRayTracingState()->enableRayTracingCounters) {
      generateTraceRayStaticId();
      // RayGen shaders are non-recursive, initialize parent ray ID to -1 here.
      if (m_shaderStage == ShaderStageRayTracingRayGen)
        m_builder->CreateStore(m_builder->getInt32(InvalidValue), m_traceParams[TraceParam::ParentRayId]);
      currentParentRayId = m_builder->CreateLoad(m_builder->getInt32Ty(), m_traceParams[TraceParam::ParentRayId]);
      args.push_back(currentParentRayId);
      args.push_back(m_builder->CreateLoad(m_builder->getInt32Ty(), m_traceRayStaticId));
    }
    CallInst *result = nullptr;
    auto funcTy = getTraceRayFuncTy();
    if (indirect) {
      Value *traceRayGpuVa = loadShaderTableVariable(ShaderTable::TraceRayGpuVirtAddr, bufferDesc);
      auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);
      auto funcPtr = m_builder->CreateIntToPtr(traceRayGpuVa, funcPtrTy);
      // Create the indirect function call
      result = m_builder->CreateCall(funcTy, funcPtr, args);
      result->setCallingConv(CallingConv::SPIR_FUNC);
    } else {
      result =
          m_builder->CreateNamedCall(RtName::TraceRayKHR, funcTy->getReturnType(), args, {Attribute::AlwaysInline});
    }
    if (rayTracingContext->getRayTracingState()->enableRayTracingCounters) {
      // Restore parent ray ID after call
      m_builder->CreateStore(currentParentRayId, m_traceParams[TraceParam::ParentRayId]);
    }
    // Save the return value to the input payloads for memcpy of type conversion
    m_builder->CreateStore(result, localPayload);
    m_builder->CreateMemCpy(payloadArg, align, localPayload, align, payloadArgSize);
    m_builder->CreateRetVoid();
  }

  m_callsToLower.push_back(inst);
  m_funcsToLower.insert(inst->getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.call.callable.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallCallableShaderOp(CallCallableShaderOp &inst) {
  std::string mangledName = inst.getCalledFunction()->getName().str() + ".impl";

  auto shaderIndex = inst.getShaderIndex();
  auto param = inst.getParam();
  auto paramDataSizeBytes = inst.getParamDataSizeBytes();

  m_builder->SetInsertPoint(&inst);
  auto newCall =
      m_builder->CreateNamedCall(mangledName, m_builder->getVoidTy(),
                                 {shaderIndex, param, m_builder->getInt32(paramDataSizeBytes), m_dispatchRaysInfoDesc},
                                 {Attribute::NoUnwind, Attribute::AlwaysInline});

  inst.replaceAllUsesWith(newCall);

  auto func = m_module->getFunction(mangledName);

  if (func->isDeclaration()) {
    func->setLinkage(GlobalVariable::InternalLinkage);
    func->addFnAttr(Attribute::AlwaysInline);

    auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
    bool indirect = rayTracingContext->getIndirectStageMask() & ShaderStageRayTracingCallableBit;

    // Create the end block
    BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);
    Instruction *funcRet = ReturnInst::Create(*m_context, endBlock);

    // Create the entry block
    BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func, endBlock);
    m_builder->SetInsertPoint(entryBlock);
    auto inputResultTy = rayTracingContext->getCallableDataType(m_builder);
    Value *inputResult = m_builder->CreateAlloca(inputResultTy, SPIRAS_Private);
    Value *shaderRecordIndexValue = func->arg_begin();

    // Copy callable data variable to the global callable variable
    Value *callableData = func->getArg(1);
    Value *callableDataSize = func->getArg(2);
    Value *buffDesc = func->getArg(3);
    const Align align = Align(4);
    m_builder->CreateMemCpy(inputResult, align, callableData, align, callableDataSize);
    SmallVector<Value *, 8> args;
    // Assemble the argument from callabledata
    args.push_back(m_builder->CreateLoad(inputResultTy, inputResult));

    // Assemble the argument from shader record index
    args.push_back(shaderRecordIndexValue);

    auto shaderIdentifier = getShaderIdentifier(ShaderStageRayTracingCallable, shaderRecordIndexValue, buffDesc);
    if (indirect) {
      auto funcTy = getCallableShaderEntryFuncTy();
      auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);
      if (rayTracingContext->isReplay()) {
        auto remapFunc = getOrCreateRemapCapturedVaToReplayVaFunc();
        shaderIdentifier = m_builder->CreateCall(remapFunc->getFunctionType(), remapFunc, shaderIdentifier);
      }
      auto funcPtr = m_builder->CreateIntToPtr(shaderIdentifier, funcPtrTy);
      CallInst *result = m_builder->CreateCall(funcTy, funcPtr, args);
      result->setCallingConv(CallingConv::SPIR_FUNC);
      m_builder->CreateStore(result, inputResult);
      m_builder->CreateBr(endBlock);
    } else {
      shaderIdentifier = m_builder->CreateTrunc(shaderIdentifier, m_builder->getInt32Ty());
      // Create Shader selection
      createShaderSelection(func, entryBlock, endBlock, shaderIdentifier, RayTracingContext::InvalidShaderId,
                            ShaderStageRayTracingCallable, args, inputResult, inputResultTy);
    }
    m_builder->SetInsertPoint(funcRet);
    m_builder->CreateMemCpy(callableData, align, inputResult, align, callableDataSize);
  }

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.report.hit" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitReportHitOp(ReportHitOp &inst) {
  std::string mangledName = inst.getCalledFunction()->getName().str() + ".impl";

  auto hitT = inst.getThit();
  auto hitKind = inst.getHitKind();

  m_builder->SetInsertPoint(&inst);
  auto newCall = m_builder->CreateNamedCall(mangledName, m_builder->getInt1Ty(),
                                            {hitT, hitKind, m_dispatchRaysInfoDesc, m_shaderRecordIndex},
                                            {Attribute::NoUnwind, Attribute::AlwaysInline});

  inst.replaceAllUsesWith(newCall);

  auto func = m_module->getFunction(mangledName);

  if (func->isDeclaration()) {
    // .entry
    //    %checkStatus = icmp ne i32 %status, %AcceptAndEndSearch
    //    store i1 1, i1 addrspace(5)* @funcRetFlag
    //    br i1 %checkStatus, label %.notAcceptAndSearch, label %.end
    //
    // .notAcceptAndSearch:
    //    %shift = fsub float %paramHitT, %tMin
    //    %tCurrentGeShift = fcmp float %tCurrent, %shift
    //    %shiftGeZero = fcmp float %shift, 0.0
    //    %checkStatus  = and i1 %tCurrentGeShift, %shiftGeZero
    //    br i1 %checkStatus, label %.accept, label %.end
    //
    // .accept:
    //    store float %tCurrentValue, float addrspace(5)* %tCurrentLocal
    //    store float %tMaxValue, float addrspace(5)* %tMaxLocal
    //    store i32 %kindValue, i32 addrspace(5)* %kindLocal
    //    store i32 %statusValue, i32 addrspace(5)* %statusLocal
    //
    //    store float %shift, float addrspace(5)* @tCurrent
    //    store float %paramHitT, float addrspace(5)* @tMax
    //    store i32 %paramKind, i32 addrspace(5)* @kind
    //    call void @AmdTraceRayCallAnyHitShader()
    //    %checkStatus = icmp ne i32 %status, 0
    //    br i1 %checkStatus, label %.notIgnore, label %.ignore
    //
    // .notIgnore:
    //    %and = and i32 %rayflag, 4
    //    %checkStatus = icmp ne i32 %and, 0
    //    %checkAcceptHitAndSearch = icmp eq i32 %status, %AcceptAndEndSearch
    //    %checkStatus = or i1 %checkStatus, %checkAcceptHitAndSearch
    //    br i1 %checkStatus, label %.acceptHitAndSearch, label %.funcRet
    //
    // .acceptHitAndSearch:
    //    store i32 AcceptAndEndSearch, i32 addrspace(5)* @status
    //    br label %.end
    //
    // .ignore:
    //    store float %tCurrentLocalValue, float addrspace(5)* @tCurrent
    //    store float %tMaxLocalValue, float addrspace(5)* @tMax
    //    store i32 %kindLocalValue, i32 addrspace(5)* @kind
    //    store i32 %statusLocalValue, i32 addrspace(5)* @status
    //    br label %.funcRet
    //
    //.funcRet:
    //   store i1 0, i1 addrspace(5)* @funcRetFlag
    //   br label %.end
    //
    //.end:
    //    %result = icmp ne i32 %status, %Ignore
    //    ret i1 %result
    assert(m_shaderStage == ShaderStageRayTracingIntersect);
    func->setLinkage(GlobalVariable::InternalLinkage);
    func->addFnAttr(Attribute::AlwaysInline);

    // Function input parameters
    Value *paramHitT = func->arg_begin();
    Value *paramHitKind = func->arg_begin() + 1;
    Value *bufferDesc = func->arg_begin() + 2;
    Value *shaderRecordIndex = func->arg_begin() + 3;

    // Create the entry block
    auto entryBlock = BasicBlock::Create(*m_context, ".entry", func);
    // Create notAcceptAndSearch
    auto notAcceptAndSearchBlock = BasicBlock::Create(*m_context, ".notAcceptAndSearch", func);
    // Create acceptBlock
    auto acceptBlock = BasicBlock::Create(*m_context, ".accept", func);
    // Create not ignore block
    auto notIgnoreBlock = BasicBlock::Create(*m_context, ".notIgnore", func);
    // Create accept hit end block
    auto acceptHitEndBlock = BasicBlock::Create(*m_context, ".acceptHitEnd", func);
    // Create ignore block
    auto ignoreBlock = BasicBlock::Create(*m_context, ".ignore", func);
    // Create funcRet block to set funcRetFlag
    auto funcRetBlock = BasicBlock::Create(*m_context, ".funcRet", func);
    // Create end block
    auto endBlock = BasicBlock::Create(*m_context, ".end", func);

    // Construct entry block
    m_builder->SetInsertPoint(entryBlock);
    m_builder->CreateStore(m_builder->getTrue(), m_funcRetFlag);

    // Create local copies
    auto tCurrentLocal = m_builder->CreateAlloca(m_builder->getFloatTy(), SPIRAS_Private);
    auto tMaxLocal = m_builder->CreateAlloca(m_builder->getFloatTy(), SPIRAS_Private);
    auto hitKindLocal = m_builder->CreateAlloca(m_builder->getInt32Ty(), SPIRAS_Private);
    auto statusLocal = m_builder->CreateAlloca(m_builder->getInt32Ty(), SPIRAS_Private);

    const static std::string ModuleNamePrefix =
        std::string("_") + getShaderStageAbbreviation(ShaderStageRayTracingIntersect) + "_";

    unsigned intersectId = 0;
    m_module->getName().substr(ModuleNamePrefix.size()).consumeInteger(0, intersectId);

    std::vector<unsigned> anyHitIds;
    auto context = static_cast<RayTracingContext *>(m_context->getPipelineContext());
    context->getStageModuleIds(ShaderStageRayTracingAnyHit, intersectId, anyHitIds);

    Value *status = m_traceParams[TraceParam::Status];
    Type *statusTy = m_traceParamsTys[TraceParam::Status];
    Value *statusValue = m_builder->CreateLoad(statusTy, status);
    Value *checkStatus = m_builder->CreateICmpNE(statusValue, m_builder->getInt32(RayHitStatus::AcceptAndEndSearch));
    m_builder->CreateCondBr(checkStatus, notAcceptAndSearchBlock, endBlock);

    // Construct notAcceptAndSearch block
    m_builder->SetInsertPoint(notAcceptAndSearchBlock);
    Value *tMin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMin], m_traceParams[TraceParam::TMin]);
    Value *tMax = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMax], m_traceParams[TraceParam::TMax]);
    Value *kind = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Kind], m_traceParams[TraceParam::Kind]);

    Value *shift = m_builder->CreateFSub(paramHitT, tMin);
    Value *tCurrent =
        m_builder->CreateLoad(m_traceParamsTys[TraceParam::TCurrent], m_traceParams[TraceParam::TCurrent]);
    Value *shiftGeZero = m_builder->CreateFCmpOGE(shift, ConstantFP::get(shift->getType(), 0.0));
    Value *tCurrentGeShift = m_builder->CreateFCmpOGE(tCurrent, shift);
    checkStatus = m_builder->CreateAnd(shiftGeZero, tCurrentGeShift);
    m_builder->CreateCondBr(checkStatus, acceptBlock, endBlock);

    // Construct accept block
    m_builder->SetInsertPoint(acceptBlock);

    // Backup tCurrent, tMax, hitKind, hitStatus
    m_builder->CreateStore(tCurrent, tCurrentLocal);
    m_builder->CreateStore(tMax, tMaxLocal);
    m_builder->CreateStore(kind, hitKindLocal);
    m_builder->CreateStore(statusValue, statusLocal);

    // Replace tCurrent with tShift
    m_builder->CreateStore(shift, m_traceParams[TraceParam::TCurrent]);
    // Replace tMax with paramHit
    m_builder->CreateStore(paramHitT, m_traceParams[TraceParam::TMax]);
    // Replace hitKind with paramHitKind
    m_builder->CreateStore(paramHitKind, m_traceParams[TraceParam::Kind]);
    m_builder->CreateStore(m_builder->getInt32(RayHitStatus::Accept), status);
    if (!anyHitIds.empty() || context->hasLibraryStage(shaderStageToMask(ShaderStageRayTracingAnyHit))) {
      auto shaderIdentifier = getShaderIdentifier(ShaderStageRayTracingAnyHit, shaderRecordIndex, bufferDesc);
      auto curPos = m_builder->saveIP();
      createAnyHitFunc(shaderIdentifier, shaderRecordIndex);
      m_builder->restoreIP(curPos);
      m_builder->CreateNamedCall(RtName::CallAnyHitShader, m_builder->getVoidTy(),
                                 {shaderIdentifier, shaderRecordIndex}, {Attribute::NoUnwind, Attribute::AlwaysInline});
    }
    // Update the status value after callAnyHit function
    statusValue = m_builder->CreateLoad(statusTy, status);
    checkStatus = m_builder->CreateICmpNE(statusValue, m_builder->getInt32(RayHitStatus::Ignore));
    m_builder->CreateCondBr(checkStatus, notIgnoreBlock, ignoreBlock);

    // Construct notIgnore block
    m_builder->SetInsertPoint(notIgnoreBlock);
    Value *rayFlags =
        m_builder->CreateLoad(m_traceParamsTys[TraceParam::RayFlags], m_traceParams[TraceParam::RayFlags]);
    auto checkRayFlags = m_builder->CreateAnd(rayFlags, m_builder->getInt32(RayFlag::AcceptFirstHitAndEndSearch));
    checkRayFlags = m_builder->CreateICmpEQ(checkRayFlags, m_builder->getInt32(RayFlag::AcceptFirstHitAndEndSearch));
    checkStatus = m_builder->CreateICmpEQ(statusValue, m_builder->getInt32(RayHitStatus::AcceptAndEndSearch));
    checkStatus = m_builder->CreateOr(checkRayFlags, checkStatus);
    m_builder->CreateCondBr(checkStatus, acceptHitEndBlock, funcRetBlock);

    // Construct acceptHitEnd block
    m_builder->SetInsertPoint(acceptHitEndBlock);
    // Set status value to the AcceptAndEndSearch
    m_builder->CreateStore(m_builder->getInt32(RayHitStatus::AcceptAndEndSearch), status);
    m_builder->CreateBr(endBlock);

    // Construct ignore block
    m_builder->SetInsertPoint(ignoreBlock);
    // Restore local copies to tCurrent, tMax, kind
    auto tCurrentLocalValue = m_builder->CreateLoad(m_builder->getFloatTy(), tCurrentLocal);
    auto tMaxLocalValue = m_builder->CreateLoad(m_builder->getFloatTy(), tMaxLocal);
    auto kindLocalValue = m_builder->CreateLoad(m_builder->getInt32Ty(), hitKindLocal);
    auto statusLocalValue = m_builder->CreateLoad(m_builder->getInt32Ty(), statusLocal);

    m_builder->CreateStore(tCurrentLocalValue, m_traceParams[TraceParam::TCurrent]);
    m_builder->CreateStore(tMaxLocalValue, m_traceParams[TraceParam::TMax]);
    m_builder->CreateStore(kindLocalValue, m_traceParams[TraceParam::Kind]);
    m_builder->CreateStore(statusLocalValue, m_traceParams[TraceParam::Status]);
    m_builder->CreateBr(funcRetBlock);

    // Construct funcRet block
    m_builder->SetInsertPoint(funcRetBlock);
    m_builder->CreateStore(m_builder->getFalse(), m_funcRetFlag);
    m_builder->CreateBr(endBlock);

    // Construct end block
    m_builder->SetInsertPoint(endBlock);
    Value *result =
        m_builder->CreateICmpNE(m_builder->CreateLoad(statusTy, status), m_builder->getInt32(RayHitStatus::Ignore));
    m_builder->CreateRet(result);
  }

  processPostReportIntersection(m_entryPoint, newCall);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerRayTracing::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerRayTracing::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Ray-Tracing\n");

  SpirvLower::init(&module);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  memset(m_traceParams, 0, sizeof(m_traceParams));
  initTraceParamsTy(rayTracingContext->getAttributeDataSize());
  initGlobalPayloads();
  initShaderBuiltIns();
  initGlobalCallableData();
  createGlobalTraceRayStaticId();
  createGlobalTraceParams();

  // Create empty raygen main module
  if (module.empty()) {
    m_shaderStage = ShaderStageRayTracingRayGen;
    createRayGenEntryFunc();
    rayTracingContext->setEntryName("main");
    return true;
  }
  Instruction *insertPos = nullptr;
  if (m_shaderStage == ShaderStageRayTracingClosestHit || m_shaderStage == ShaderStageRayTracingAnyHit ||
      m_shaderStage == ShaderStageRayTracingIntersect || m_shaderStage == ShaderStageRayTracingMiss) {
    insertPos = createEntryFunc(m_entryPoint);
  } else if (m_shaderStage == ShaderStageRayTracingCallable) {
    insertPos = createCallableShaderEntryFunc(m_entryPoint);
  } else {
    insertPos = &*(m_entryPoint->begin()->getFirstNonPHIOrDbgOrAlloca());
    m_shaderRecordIndex = m_builder->getInt32(0);
  }
  // Process traceRays module
  if (m_shaderStage == ShaderStageCompute) {
    static auto visitor = llvm_dialects::VisitorBuilder<SpirvLowerRayTracing>()
                              .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                              .add(&SpirvLowerRayTracing::visitGetHitAttributes)
                              .add(&SpirvLowerRayTracing::visitSetHitAttributes)
                              .add(&SpirvLowerRayTracing::visitSetTraceParams)
                              .add(&SpirvLowerRayTracing::visitCallClosestHitShader)
                              .add(&SpirvLowerRayTracing::visitCallMissShader)
                              .add(&SpirvLowerRayTracing::visitCallTriangleAnyHitShader)
                              .add(&SpirvLowerRayTracing::visitCallIntersectionShader)
                              .add(&SpirvLowerRayTracing::visitSetTriangleIntersectionAttributes)
                              .add(&SpirvLowerRayTracing::visitSetHitTriangleNodePointer)
                              .add(&SpirvLowerRayTracing::visitGetParentId)
                              .add(&SpirvLowerRayTracing::visitSetParentId)
                              .add(&SpirvLowerRayTracing::visitDispatchRayIndex)
                              .build();

    visitor.visit(*this, *m_module);

    // Create traceRay module entry function
    createTraceRay();

  } else { // Process ray tracing modules
    m_entryPoint->setName(module.getName());
    m_entryPoint->addFnAttr(Attribute::AlwaysInline);
    m_builder->SetInsertPoint(insertPos);
    createDispatchRaysInfoDesc();
    m_spirvOpMetaKindId = m_context->getMDKindID(MetaNameSpirvOp);

    if (m_shaderStage == ShaderStageRayTracingAnyHit || m_shaderStage == ShaderStageRayTracingClosestHit ||
        m_shaderStage == ShaderStageRayTracingIntersect) {
      m_worldToObjMatrix = nullptr;

      if (m_shaderStage == ShaderStageRayTracingIntersect) {
        m_funcRetFlag = new GlobalVariable(*m_module, m_builder->getInt1Ty(), false, GlobalValue::ExternalLinkage,
                                           nullptr, "", nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
      }
    }

    for (auto globalIt = m_module->global_begin(); globalIt != m_module->global_end();) {
      GlobalVariable *global = &*globalIt++;
      if (global->getName().startswith(RtName::HitAttribute)) {
        replaceGlobal(global, m_traceParams[TraceParam::HitAttributes]);
        continue;
      } else if (global->getName().startswith(RtName::IncomingPayLoad)) {
        replaceGlobal(global, m_globalPayload);
        continue;
      } else if (global->getName().startswith(RtName::IncomingCallableData)) {
        replaceGlobal(global, m_globalCallableData);
        continue;
      } else if (global->getName().startswith(RtName::ShaderRecordBuffer)) {
        processShaderRecordBuffer(global, m_dispatchRaysInfoDesc, m_shaderRecordIndex, insertPos);
        continue;
      }

      if (global->getType()->getAddressSpace() != SPIRAS_Input)
        continue;

      ShaderInOutMetadata inputMeta = {};
      MDNode *metaNode = global->getMetadata(gSPIRVMD::InOut);
      Type *globalTy = global->getValueType();
      auto meta = mdconst::dyn_extract<Constant>(metaNode->getOperand(0));

      unsigned startOperand = 0;
      if (globalTy->isArrayTy()) {
        assert(meta->getNumOperands() == 4);
        startOperand += 2;
      }

      inputMeta.U64All[0] = cast<ConstantInt>(meta->getOperand(startOperand))->getZExtValue();
      inputMeta.U64All[1] = cast<ConstantInt>(meta->getOperand(startOperand + 1))->getZExtValue();
      assert(inputMeta.IsBuiltIn);
      unsigned builtInId = inputMeta.Value;

      m_builder->SetInsertPoint(insertPos);
      Value *input = processBuiltIn(builtInId, insertPos);
      if (input)
        m_builder->CreateStore(input, global);
    }

    static auto visitor = llvm_dialects::VisitorBuilder<SpirvLowerRayTracing>()
                              .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                              .add(&SpirvLowerRayTracing::visitAcceptHitAndEndSearchOp)
                              .add(&SpirvLowerRayTracing::visitIgnoreHitOp)
                              .add(&SpirvLowerRayTracing::visitCallCallableShaderOp)
                              .add(&SpirvLowerRayTracing::visitReportHitOp)
                              .add(&SpirvLowerRayTracing::visitTraceRayOp)
                              .build();

    visitor.visit(*this, *m_module);
  }
  if (m_shaderStage == ShaderStageRayTracingClosestHit || m_shaderStage == ShaderStageRayTracingAnyHit ||
      m_shaderStage == ShaderStageRayTracingIntersect || m_shaderStage == ShaderStageRayTracingMiss) {
    createEntryTerminator(m_entryPoint);
  }
  if (m_shaderStage == ShaderStageRayTracingCallable) {
    createCallableShaderEntryTerminator(m_entryPoint);
  }

  for (Instruction *call : m_callsToLower) {
    call->dropAllReferences();
    call->eraseFromParent();
  }

  for (Function *func : m_funcsToLower) {
    func->dropAllReferences();
    func->eraseFromParent();
  }

  LLVM_DEBUG(dbgs() << "After the pass Spirv-Lower-Ray-Tracing " << module);

  return true;
}

// =====================================================================================================================
// Create global variable for the TraceParam
void SpirvLowerRayTracing::createGlobalTraceParams() {
  for (unsigned i = 0; i < TraceParam::Count; ++i) {
    m_traceParams[i] = new GlobalVariable(*m_module, m_traceParamsTys[i], false, GlobalValue::ExternalLinkage, nullptr,
                                          Twine(RtName::TraceRaySetTraceParams) + std::to_string(i), nullptr,
                                          GlobalValue::NotThreadLocal, SPIRAS_Private);
  }
}

// =====================================================================================================================
// Create function to set HitAttributes
//
// @param func : Function to create
void SpirvLowerRayTracing::createSetHitAttributes(Function *func) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  auto argIt = func->arg_begin();
  assert(func->arg_size() == (TraceParam::GeometryIndex - TraceParam::TCurrent + 1));

  for (unsigned i = 0; i < func->arg_size(); ++i) {
    Value *storeValue = argIt++;
    m_builder->CreateStore(storeValue, m_traceParams[i + TraceParam::TCurrent]);
  }

  auto tCurrent = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TCurrent], m_traceParams[TraceParam::TCurrent]);
  auto tMin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMin], m_traceParams[TraceParam::TMin]);
  auto tMax = m_builder->CreateFAdd(tCurrent, tMin);
  m_builder->CreateStore(tMax, m_traceParams[TraceParam::TMax]);

  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Create function to set TraceParams
//
// @param func : Function to create
void SpirvLowerRayTracing::createSetTraceParams(Function *func) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 12
  assert(func->arg_size() == 9);
#else
  assert(func->arg_size() == 8);
#endif

  auto argIt = func->arg_begin();

  Value *rayFlags = argIt++;
  m_builder->CreateStore(rayFlags, m_traceParams[TraceParam::RayFlags]);

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 12
  Value *instanceInclusionMask = argIt++;
  m_builder->CreateStore(instanceInclusionMask, m_traceParams[TraceParam::InstanceInclusionMask]);
#endif

  Value *originX = argIt++;
  Value *originY = argIt++;
  Value *originZ = argIt++;
  Value *origin = PoisonValue::get(FixedVectorType::get(Type::getFloatTy(*m_context), 3));
  origin = m_builder->CreateInsertElement(origin, originX, uint64_t(0));
  origin = m_builder->CreateInsertElement(origin, originY, 1);
  origin = m_builder->CreateInsertElement(origin, originZ, 2);
  m_builder->CreateStore(origin, m_traceParams[TraceParam::Origin]);

  Value *tMin = argIt++;
  m_builder->CreateStore(tMin, m_traceParams[TraceParam::TMin]);

  Value *dirX = argIt++;
  Value *dirY = argIt++;
  Value *dirZ = argIt++;
  Value *dir = PoisonValue::get(FixedVectorType::get(Type::getFloatTy(*m_context), 3));
  dir = m_builder->CreateInsertElement(dir, dirX, uint64_t(0));
  dir = m_builder->CreateInsertElement(dir, dirY, 1);
  dir = m_builder->CreateInsertElement(dir, dirZ, 2);
  m_builder->CreateStore(dir, m_traceParams[TraceParam::Dir]);

  Value *zero = ConstantFP::get(m_builder->getFloatTy(), 0.0);
  m_builder->CreateStore(zero, m_traceParams[TraceParam::TMax]);

  m_builder->CreateRetVoid();
}

// =======================================================================================================================
// Create function to process hook function between traceray and intersection shaders: ClosestHit, AnyHit, Miss,
// Intersect
//
// @param func : Function to create
// @param stage : Ray tracing shader stage
// @param intersectId : Module ID of intersection shader
// @param retVal : Function return value
void SpirvLowerRayTracing::createCallShaderFunc(Function *func, ShaderStage stage, unsigned intersectId,
                                                Value *retVal) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  auto shaderStageMask = rayTracingContext->getShaderStageMask();

  eraseFunctionBlocks(func);
  // Create the end block
  BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);
  auto stageMask = shaderStageToMask(stage);
  // Skip shader call instructions if there is no actual shader for the given shader stage
  if ((shaderStageMask & stageMask) || rayTracingContext->hasLibraryStage(stageMask)) {
    // Create the entry block
    BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func, endBlock);
    m_builder->SetInsertPoint(entryBlock);
    Value *inputResult = m_builder->CreateAlloca(getShaderReturnTy(stage), SPIRAS_Private);
    updateGlobalFromCallShaderFunc(func, stage);
    // Table Index is second parameter for non-intersect shader and third for intersect shader
    Value *tableIndexValue =
        stage != ShaderStageRayTracingIntersect ? (func->arg_begin() + 1) : (func->arg_begin() + 2);

    Value *shaderId = func->arg_begin();
    shaderId = m_builder->CreateBitCast(shaderId, m_builder->getInt64Ty());
    createCallShader(func, stage, intersectId, shaderId, tableIndexValue, inputResult, entryBlock, endBlock);
  } else {
    m_builder->SetInsertPoint(endBlock);
  }

  m_builder->CreateRet(retVal);
}

// =====================================================================================================================
// Create indirect call/inline call
//
// @param func : Function to insert shader selection
// @param stage : Shader stage
// @param intersectId : Module ID of intersection shader
// @param shaderId : Shader ID to select shader
// @param shaderRecordIndex : Shader record index/ table index
// @param inputResult : input result to get shader selection return value
// @param entryBlock : Entry block
// @param endBlock : End block
void SpirvLowerRayTracing::createCallShader(Function *func, ShaderStage stage, unsigned intersectId, Value *shaderId,
                                            Value *shaderRecordIndex, Value *inputResult, BasicBlock *entryBlock,
                                            BasicBlock *endBlock) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  auto indirectStageMask = rayTracingContext->getIndirectStageMask();
  bool indirectShader = indirectStageMask & shaderStageToMask(stage);

  // Current m_builder inside entry block
  if (indirectShader) {
    // Create indirect call block
    BasicBlock *callBlock = BasicBlock::Create(*m_context, ".call", func, endBlock);
    // If the shaderId is zero, goto the endblock, or else, go to the call block
    auto checkShaderId = m_builder->CreateICmpNE(shaderId, m_builder->getInt64(0));
    m_builder->CreateCondBr(checkShaderId, callBlock, endBlock);
    m_builder->SetInsertPoint(callBlock);
  }

  SmallVector<Value *, 8> args;

  Value *traceParams[TraceParam::Count] = {};
  const auto payloadType = rayTracingContext->getPayloadType(m_builder);
  Value *payload = m_builder->CreateLoad(payloadType, m_globalPayload);

  // Assemble the arguments from payloads in traceray module
  args.push_back(payload);

  // Assemble the arguments from builtIns
  for (auto builtIn : m_builtInParams) {
    traceParams[builtIn] = m_builder->CreateLoad(m_traceParamsTys[builtIn], m_traceParams[builtIn]);
    args.push_back(traceParams[builtIn]);
  }

  // Assemble the extra arguments for specific shader stage
  for (auto param : getShaderExtraInputParams(stage)) {
    traceParams[param] = m_builder->CreateLoad(m_traceParamsTys[param], m_traceParams[param]);
    args.push_back(traceParams[param]);
  }

  args.push_back(shaderRecordIndex);

  if (indirectShader) {
    auto funcTy = getShaderEntryFuncTy(stage);
    auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);

    if (rayTracingContext->isReplay()) {
      auto remapFunc = getOrCreateRemapCapturedVaToReplayVaFunc();
      shaderId = m_builder->CreateCall(remapFunc->getFunctionType(), remapFunc, shaderId);
    }

    auto funcPtr = m_builder->CreateIntToPtr(shaderId, funcPtrTy);
    CallInst *result = m_builder->CreateCall(funcTy, funcPtr, args);
    result->setCallingConv(CallingConv::SPIR_FUNC);
    storeFunctionCallResult(stage, result);
    m_builder->CreateBr(endBlock);
    m_builder->SetInsertPoint(endBlock);
  } else {
    initInputResult(stage, payload, traceParams, inputResult);
    shaderId = m_builder->CreateTrunc(shaderId, m_builder->getInt32Ty());
    Type *inputResultTy = getShaderReturnTy(stage);
    createShaderSelection(func, entryBlock, endBlock, shaderId, intersectId, stage, args, inputResult, inputResultTy);
    m_builder->SetInsertPoint(endBlock);
    inputResult = m_builder->CreateLoad(inputResultTy, inputResult);
    storeFunctionCallResult(stage, inputResult);
  }
}

// =====================================================================================================================
// Process global variable HitAttribute
//
// @param global : Global variable HitAttribute
// @param replacedGlobalglobal : Replaced global variable
void SpirvLowerRayTracing::replaceGlobal(GlobalVariable *global, GlobalVariable *replacedGlobal) {
  removeConstantExpr(m_context, global);
  for (auto userIt = global->user_begin(); userIt != global->user_end();) {
    User *user = *userIt++;
    Instruction *inst = dyn_cast<Instruction>(user);
    if (inst) {
      m_builder->SetInsertPoint(inst);
      Value *castHitAttribute = m_builder->CreateBitCast(replacedGlobal, global->getType());
      user->replaceUsesOfWith(global, castHitAttribute);
    }
  }
  global->dropAllReferences();
  global->eraseFromParent();
}

// =====================================================================================================================
// Patch library AmdTraceRaySetTriangleIntersectionAttributes function
//
// @param func : Function to create
void SpirvLowerRayTracing::createSetTriangleInsection(Function *func) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);
  Value *barycentrics = func->arg_begin();
  auto zero = m_builder->getInt32(0);
  auto one = m_builder->getInt32(1);

  Value *attribValue0 = m_builder->CreateExtractElement(barycentrics, uint64_t(0));
  Type *attribHitEltTy = m_traceParamsTys[TraceParam::HitAttributes];
  Value *attribDestPtr = m_builder->CreateGEP(attribHitEltTy, m_traceParams[TraceParam::HitAttributes], {zero, zero});
  m_builder->CreateStore(attribValue0, attribDestPtr);

  Value *attribValue1 = m_builder->CreateExtractElement(barycentrics, 1);
  attribDestPtr = m_builder->CreateGEP(attribHitEltTy, m_traceParams[TraceParam::HitAttributes], {zero, one});
  m_builder->CreateStore(attribValue1, attribDestPtr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Processes ray tracing "call" builtIn instruction
//
// @param builtInId : ID of the built-in variable
// @param insertPos : Where to insert instructions
Value *SpirvLowerRayTracing::processBuiltIn(unsigned builtInId, Instruction *insertPos) {
  Value *input = nullptr;

  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  auto instNodeAddrTy = m_traceParamsTys[TraceParam::InstNodeAddrLo];

  m_builder->SetInsertPoint(insertPos);
  switch (builtInId) {
  case BuiltInLaunchIdKHR:
  case BuiltInPrimitiveId:
  case BuiltInHitKindKHR:
  case BuiltInIncomingRayFlagsKHR:
  case BuiltInRayTminKHR:
  case BuiltInHitTNV:
  case BuiltInRayTmaxKHR:
  case BuiltInWorldRayOriginKHR:
  case BuiltInWorldRayDirectionKHR:
  case BuiltInRayGeometryIndexKHR: {
    break;
  }
  case BuiltInCullMaskKHR: {
    break;
  }
  case BuiltInLaunchSizeKHR: {
    input = loadShaderTableVariable(ShaderTable::LaunchSize, m_dispatchRaysInfoDesc);
    break;
  }
  case BuiltInObjectToWorldKHR: {
    input = createLoadRayTracingMatrix(builtInId, insertPos);
    break;
  }
  case BuiltInWorldToObjectKHR: {
    m_worldToObjMatrix = !m_worldToObjMatrix ? createLoadRayTracingMatrix(builtInId, insertPos) : m_worldToObjMatrix;

    input = m_worldToObjMatrix;
    break;
  }
  case BuiltInObjectRayOriginKHR: {
    Value *origin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Origin], m_traceParams[TraceParam::Origin]);

    m_worldToObjMatrix =
        !m_worldToObjMatrix ? createLoadRayTracingMatrix(BuiltInWorldToObjectKHR, insertPos) : m_worldToObjMatrix;

    // one = vec3(1.0f)
    Value *one = ConstantFP::get(m_worldToObjMatrix->getType()->getArrayElementType(), 1.0);
    // vec3 -> vec4, origin = vec4(origin.xyz, 1.0>
    origin = m_builder->CreateShuffleVector(origin, one, ArrayRef<int>{0, 1, 2, 3});
    // Transform origin
    origin = m_builder->CreateMatrixTimesVector(m_worldToObjMatrix, origin);
    // vec4 -> vec3
    input = m_builder->CreateShuffleVector(origin, origin, ArrayRef<int>{0, 1, 2});
    break;
  }
  case BuiltInObjectRayDirectionKHR: {
    Value *dir = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Dir], m_traceParams[TraceParam::Dir]);
    m_worldToObjMatrix =
        !m_worldToObjMatrix ? createLoadRayTracingMatrix(BuiltInWorldToObjectKHR, insertPos) : m_worldToObjMatrix;

    // zero = vec3(0.0f)
    Value *zero = ConstantFP::get(m_worldToObjMatrix->getType()->getArrayElementType(), 0.0);
    // vec3 -> vec4, vec4 dir = vec4(dir.xyz, 0.0)
    dir = m_builder->CreateShuffleVector(dir, zero, ArrayRef<int>{0, 1, 2, 3});
    // Transform dir
    dir = m_builder->CreateMatrixTimesVector(m_worldToObjMatrix, dir);
    // vec4 -> vec3
    input = m_builder->CreateShuffleVector(dir, dir, ArrayRef<int>{0, 1, 2});
    break;
  }
  case BuiltInInstanceCustomIndexKHR:
  case BuiltInInstanceId: {
    Value *instNodeAddrLo = m_builder->CreateLoad(instNodeAddrTy, m_traceParams[TraceParam::InstNodeAddrLo]);
    Value *instNodeAddrHi = m_builder->CreateLoad(instNodeAddrTy, m_traceParams[TraceParam::InstNodeAddrHi]);

    Value *instNodeAddr = PoisonValue::get(int32x2Ty);
    instNodeAddr = m_builder->CreateInsertElement(instNodeAddr, instNodeAddrLo, uint64_t(0));
    instNodeAddr = m_builder->CreateInsertElement(instNodeAddr, instNodeAddrHi, 1u);

    if (builtInId == BuiltInInstanceCustomIndexKHR) {
      input = createLoadInstanceId(instNodeAddr);
    } else {
      assert(builtInId == BuiltInInstanceId);
      input = createLoadInstanceIndex(instNodeAddr);
    }
    break;
  }
  default: {
    // Nothing to do
    break;
  }
  }

  return input;
}

// =====================================================================================================================
// Load shader table variable
//
// @param tableKind : Kind of shader table variable to create
// @param bufferDesc : Dispatch ray buffer descriptor
Value *SpirvLowerRayTracing::loadShaderTableVariable(ShaderTable tableKind, Value *bufferDesc) {
  assert(tableKind < ShaderTable::Count);
  switch (tableKind) {
  case ShaderTable::RayGenTableAddr: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, rayGenerationTable);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, rayGenerationTableAddressLo);
    static_assert(
        offsetof(GpuRt::DispatchRaysConstantData, rayGenerationTableAddressHi) ==
            offsetof(GpuRt::DispatchRaysConstantData, rayGenerationTableAddressLo) + 4,
        "GpuRt::DispatchRaysConstantData: rayGenerationTableAddressLo and rayGenerationTableAddressHi mismatch!");
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt64Ty(), valuePtr);
  }
  case ShaderTable::MissTableAddr: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, missTable.baseAddress);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, missTableBaseAddressLo);
    static_assert(offsetof(GpuRt::DispatchRaysConstantData, missTableBaseAddressHi) ==
                      offsetof(GpuRt::DispatchRaysConstantData, missTableBaseAddressLo) + 4,
                  "GpuRt::DispatchRaysConstantData: missTableBaseAddressLo and missTableBaseAddressHi mismatch!");
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt64Ty(), valuePtr);
  }
  case ShaderTable::HitGroupTableAddr: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, hitGroupTable.baseAddress);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, hitGroupTableBaseAddressLo);
    static_assert(
        offsetof(GpuRt::DispatchRaysConstantData, hitGroupTableBaseAddressHi) ==
            offsetof(GpuRt::DispatchRaysConstantData, hitGroupTableBaseAddressLo) + 4,
        "GpuRt::DispatchRaysConstantData: hitGroupTableBaseAddressLo and hitGroupTableBaseAddressHi mismatch!");
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt64Ty(), valuePtr);
  }
  case ShaderTable::CallableTableAddr: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, callableTable.baseAddress);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, callableTableBaseAddressLo);
    static_assert(
        offsetof(GpuRt::DispatchRaysConstantData, callableTableBaseAddressHi) ==
            offsetof(GpuRt::DispatchRaysConstantData, callableTableBaseAddressLo) + 4,
        "GpuRt::DispatchRaysConstantData: callableTableBaseAddressLo and callableTableBaseAddressHi mismatch!");
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt64Ty(), valuePtr);
  }
  case ShaderTable::MissTableStride: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, missTable.strideInBytes);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, missTableStrideInBytes);
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt32Ty(), valuePtr);
  }
  case ShaderTable::HitGroupTableStride: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, hitGroupTable.strideInBytes);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, hitGroupTableStrideInBytes);
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt32Ty(), valuePtr);
  }
  case ShaderTable::CallableTableStride: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, callableTable.strideInBytes);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, callableTableStrideInBytes);
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt32Ty(), valuePtr);
  }
  case ShaderTable::TraceRayGpuVirtAddr: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, traceRayGpuVa);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, traceRayGpuVaLo);
    static_assert(offsetof(GpuRt::DispatchRaysConstantData, traceRayGpuVaHi) ==
                      offsetof(GpuRt::DispatchRaysConstantData, traceRayGpuVaLo) + 4,
                  "GpuRt::DispatchRaysConstantData: traceRayGpuVaLo and traceRayGpuVaHi mismatch!");
#endif
    Value *valuePtr = m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, m_builder->getInt32(offset), "");
    return m_builder->CreateLoad(m_builder->getInt64Ty(), valuePtr);
  }
  case ShaderTable::LaunchSize: {
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
    auto offset = offsetof(GpuRt::DispatchRaysInfoData, rayDispatchWidth);
#else
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, rayDispatchWidth);
#endif
    Value *offsetOfRayDispatchWidth = m_builder->getInt32(offset);
    Value *rayDispatchWidthPtr =
        m_builder->CreateInBoundsGEP(m_builder->getInt8Ty(), bufferDesc, offsetOfRayDispatchWidth, "");
    Type *int32x3Ty = FixedVectorType::get(m_builder->getInt32Ty(), 3);
    return m_builder->CreateLoad(int32x3Ty, rayDispatchWidthPtr);
  }
  default: {
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
  }
}

// =====================================================================================================================
// Create switch case of shader selection
//
// @param func : Function to insert shader selection
// @param entryBlock : Entry block
// @param endBlock : End block
// @param shaderId : Shader ID to select shader
// @param intersectId : Module ID of intersection shader
// @param stage : Shader stage
// @param args : Argument list of function call
// @param inResult : Allocated value to store function return value
// @param inResultTy : Base type of inResult param
void SpirvLowerRayTracing::createShaderSelection(Function *func, BasicBlock *entryBlock, BasicBlock *endBlock,
                                                 Value *shaderId, unsigned intersectId, ShaderStage stage,
                                                 const SmallVector<Value *, 8> &args, Value *inResult,
                                                 Type *inResultTy) {
  // .entry:
  // switch i32 %shaderId, label % .end[
  //    i32 2, label % .shader2
  //    i32 3, label % .shader3]
  //
  // .shader2:
  //    call void @llpc.closesthit.2() #0
  //    br label % .end
  // .shader3:
  //    call void @llpc.closesthit.3() #0
  //    br label % .end

  // .end:
  //   ret i1 true

  auto context = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  std::vector<unsigned> moduleIds;
  context->getStageModuleIds(stage, intersectId, moduleIds);
  if (moduleIds.size() == 0) {
    BranchInst::Create(endBlock, entryBlock);
    return;
  }

  auto switchInst = SwitchInst::Create(shaderId, endBlock, moduleIds.size(), entryBlock);
  for (unsigned i = 0; i < moduleIds.size(); ++i) {
    auto moduleIdStr = std::to_string(moduleIds[i]);
    std::string branchName = ".shader" + moduleIdStr;
    BasicBlock *shaderBlock = BasicBlock::Create(*m_context, branchName, func, endBlock);
    switchInst->addCase(m_builder->getInt32(moduleIds[i]), shaderBlock);
    m_builder->SetInsertPoint(shaderBlock);
    auto funcName = std::string("_") + getShaderStageAbbreviation(stage) + "_" + moduleIdStr;
    Value *result =
        m_builder->CreateNamedCall(funcName, inResultTy, args, {Attribute::NoUnwind, Attribute::AlwaysInline});
    if (inResult)
      m_builder->CreateStore(result, inResult);

    BranchInst::Create(endBlock, shaderBlock);
  }
}

// =====================================================================================================================
// Process global variable shader record buffer
//
// @param global : Global variable corresponding to shader record buffer
// @param bufferDesc: Dispatch ray buffer descriptor
// @param tableIndex : Shader table record index
// @param insertPos : Where to insert instruction
void SpirvLowerRayTracing::processShaderRecordBuffer(GlobalVariable *global, Value *bufferDesc, Value *tableIndex,
                                                     Instruction *insertPos) {
  m_builder->SetInsertPoint(insertPos);
  Value *tableAddr = nullptr;
  Value *tableStride = nullptr;

  switch (m_shaderStage) {
  case ShaderStageRayTracingRayGen: {
    tableAddr = loadShaderTableVariable(ShaderTable::RayGenTableAddr, bufferDesc);
    tableStride = m_builder->getInt32(0);
    break;
  }
  case ShaderStageRayTracingClosestHit:
  case ShaderStageRayTracingAnyHit:
  case ShaderStageRayTracingIntersect: {
    tableAddr = loadShaderTableVariable(ShaderTable::HitGroupTableAddr, bufferDesc);
    tableStride = loadShaderTableVariable(ShaderTable::HitGroupTableStride, bufferDesc);
    break;
  }
  case ShaderStageRayTracingCallable: {
    tableAddr = loadShaderTableVariable(ShaderTable::CallableTableAddr, bufferDesc);
    tableStride = loadShaderTableVariable(ShaderTable::CallableTableStride, bufferDesc);
    break;
  }
  case ShaderStageRayTracingMiss: {
    tableAddr = loadShaderTableVariable(ShaderTable::MissTableAddr, bufferDesc);
    tableStride = loadShaderTableVariable(ShaderTable::MissTableStride, bufferDesc);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  assert(tableAddr);
  assert(tableStride);

  // ShaderIdsSize should be 4 * 8 bytes = 32 bytes
  const unsigned shaderIdsSize = sizeof(Vkgc::RayTracingShaderIdentifier);
  Value *shaderIdsSizeVal = m_builder->getInt32(shaderIdsSize);

  // Byte offset = (tableStride * tableIndex) + shaderIdsSize
  Value *offset = m_builder->CreateMul(tableIndex, tableStride);
  offset = m_builder->CreateAdd(offset, shaderIdsSizeVal);

  // Zero-extend offset value to 64 bit
  offset = m_builder->CreateZExt(offset, m_builder->getInt64Ty());

  // Final addr
  tableAddr = m_builder->CreateAdd(tableAddr, offset);

  // Convert to the global shader record buffer type pointer
  assert(global->getAddressSpace() == SPIRAS_Global);
  tableAddr = m_builder->CreateIntToPtr(tableAddr, global->getType());

  removeConstantExpr(m_context, global);
  global->replaceAllUsesWith(tableAddr);
  global->eraseFromParent();
}

// =====================================================================================================================
// Get shader identifier
//
// @param stage : Shader stage
// @param shaderRecordIndex : Shader table record index
// @param bufferDesc : DispatchRay descriptor
Value *SpirvLowerRayTracing::getShaderIdentifier(ShaderStage stage, Value *shaderRecordIndex, Value *bufferDesc) {
  ShaderTable tableAddr = ShaderTable::Count;
  ShaderTable tableStride = ShaderTable::Count;
  unsigned offset = 0;
  switch (stage) {
  case ShaderStageRayTracingRayGen: {
    tableAddr = ShaderTable::RayGenTableAddr;
    break;
  }
  case ShaderStageRayTracingMiss: {
    tableAddr = ShaderTable::MissTableAddr;
    tableStride = ShaderTable::MissTableStride;
    break;
  }
  case ShaderStageRayTracingClosestHit: {
    tableAddr = ShaderTable::HitGroupTableAddr;
    tableStride = ShaderTable::HitGroupTableStride;
    break;
  }
  case ShaderStageRayTracingAnyHit: {
    tableAddr = ShaderTable::HitGroupTableAddr;
    tableStride = ShaderTable::HitGroupTableStride;
    offset = 8;
    break;
  }
  case ShaderStageRayTracingIntersect: {
    tableAddr = ShaderTable::HitGroupTableAddr;
    tableStride = ShaderTable::HitGroupTableStride;
    offset = 16;
    break;
  }
  case ShaderStageRayTracingCallable: {
    tableAddr = ShaderTable::CallableTableAddr;
    tableStride = ShaderTable::CallableTableStride;
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }

  assert(tableAddr != ShaderTable::Count);
  Value *tableAddrVal = loadShaderTableVariable(tableAddr, bufferDesc);

  Value *stride = m_builder->getInt32(0);
  if (tableStride != ShaderTable::Count) {
    stride = loadShaderTableVariable(tableStride, bufferDesc);
  }

  // Table offset sbtIndex * stride + offset
  Value *offsetVal = m_builder->CreateMul(shaderRecordIndex, stride);
  offsetVal = m_builder->CreateAdd(offsetVal, m_builder->getInt32(offset));

  // DWord_Load(TableAddr, offset)
  Type *gpuAddrAsPtrTy = Type::getInt8PtrTy(*m_context, SPIRAS_Global);
  auto shaderIdentifierAsPtr = m_builder->CreateIntToPtr(tableAddrVal, gpuAddrAsPtrTy);
  Value *shaderIdentifier = m_builder->CreateGEP(m_builder->getInt8Ty(), shaderIdentifierAsPtr, offsetVal);
  auto loadPtrTy = m_builder->getInt64Ty()->getPointerTo(SPIRAS_Global);
  shaderIdentifier = m_builder->CreateBitCast(shaderIdentifier, loadPtrTy);
  shaderIdentifier = m_builder->CreateLoad(m_builder->getInt64Ty(), shaderIdentifier);

  return shaderIdentifier;
}

// =====================================================================================================================
// Create AnyHit shaders call function for use reportIntersection
//
// @param shaderIdentifier : Input shader identifier for the function
// @param shaderRecordIndex : Shader record index
void SpirvLowerRayTracing::createAnyHitFunc(Value *shaderIdentifier, Value *shaderRecordIndex) {
  Function *func = dyn_cast_or_null<Function>(m_module->getFunction(RtName::CallAnyHitShader));
  if (!func) {
    auto funcTy =
        FunctionType::get(m_builder->getVoidTy(), {shaderIdentifier->getType(), shaderRecordIndex->getType()}, false);
    func = Function::Create(funcTy, GlobalValue::InternalLinkage, RtName::CallAnyHitShader, m_module);
    func->addFnAttr(Attribute::NoUnwind);
    func->addFnAttr(Attribute::AlwaysInline);

    // Create the entry block
    BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func);

    // Create the shader block to call anyhit shader
    BasicBlock *shaderBlock = BasicBlock::Create(*m_context, ".shader", func);

    // Create duplicate block to set the anyhit duplicate visit flag
    BasicBlock *duplicateBlock = BasicBlock::Create(*m_context, ".duplicate", func);

    // Create the end block with return instruction
    BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);
    auto zero = m_builder->getInt32(0);

    m_builder->SetInsertPoint(entryBlock);
    Value *shaderId = func->arg_begin();
    Value *tableIndex = func->arg_begin() + 1;
    Value *inputResult = m_builder->CreateAlloca(getShaderReturnTy(ShaderStageRayTracingAnyHit), SPIRAS_Private);
    Value *anyHitCallATypeAddr = m_traceParams[TraceParam::DuplicateAnyHit];
    Value *anyHitCallType = m_builder->CreateLoad(m_traceParamsTys[TraceParam::DuplicateAnyHit], anyHitCallATypeAddr);
    Value *checkCallType = m_builder->CreateICmpEQ(anyHitCallType, zero);
    m_builder->CreateCondBr(checkCallType, endBlock, shaderBlock);

    m_builder->SetInsertPoint(shaderBlock);
    createCallShader(func, ShaderStageRayTracingAnyHit, RayTracingContext::InvalidShaderId, shaderId, tableIndex,
                     inputResult, shaderBlock, duplicateBlock);

    m_builder->SetInsertPoint(duplicateBlock);
    checkCallType = m_builder->CreateICmpEQ(anyHitCallType, m_builder->getInt32(1));
    anyHitCallType = m_builder->CreateSelect(checkCallType, zero, anyHitCallType);
    m_builder->CreateStore(anyHitCallType, anyHitCallATypeAddr);
    m_builder->CreateBr(endBlock);

    m_builder->SetInsertPoint(endBlock);
    m_builder->CreateRetVoid();
  }
}

// =====================================================================================================================
// Create global built-in variable
//
// @param builtInId : ID of the global built-in  variable
GlobalVariable *SpirvLowerRayTracing::createGlobalBuiltIn(unsigned builtInId) {
  assert(builtInId == BuiltInLaunchSizeKHR);

  GlobalVariable *global = new GlobalVariable(*m_module, FixedVectorType::get(m_builder->getInt32Ty(), 3), false,
                                              GlobalValue::ExternalLinkage, nullptr, "launchSize", nullptr,
                                              GlobalValue::NotThreadLocal, SPIRAS_Input);

  ShaderInOutMetadata inputMeta = {};
  inputMeta.IsBuiltIn = true;
  inputMeta.Value = builtInId;
  std::vector<Type *> metaTys = {m_builder->getInt64Ty(), m_builder->getInt64Ty()};
  auto metaTy = StructType::get(*m_context, metaTys);

  std::vector<Constant *> metaDataValues = {ConstantInt::get(m_builder->getInt64Ty(), inputMeta.U64All[0]),
                                            ConstantInt::get(m_builder->getInt64Ty(), inputMeta.U64All[1])};

  Constant *metaDataValue = ConstantStruct::get(metaTy, metaDataValues);

  std::vector<Metadata *> metaDatas = {ConstantAsMetadata::get(metaDataValue)};
  auto metaDataNode = MDNode::get(*m_context, metaDatas);
  global->addMetadata(gSPIRVMD::InOut, *metaDataNode);

  return global;
}

// =====================================================================================================================
// Process ray gen functions, threads of launchId should not exceed the launchSize
void SpirvLowerRayTracing::createRayGenEntryFunc() {
  // .entry
  //    %xgreat = icmp ge i32 %launchId.x, %launchSize.x
  //    %ygreat = icmp ge i32 %launchId.y, %launchSize.y
  //    %xygreat = or i1 %xgreat, %ygreat
  //    br i1 %xygreate, label %.earlyRet, %.main
  //
  // .earlyRet
  //    ret void
  //
  // .main
  //    switch i32 %regenid, label % .end[
  //      i32 1, label % .shader1
  //      i32 2, label % .shader2]
  //
  // .shader1:
  //    call void @llpcraygen1() #0
  //    br label % .end
  //
  // .shader2:
  //    call void @llpcraygen2() #0
  //    br label % .end
  //
  // .end:
  //    ret void
  //

  assert(m_shaderStage == ShaderStageRayTracingRayGen);

  // Create main function to call raygen entry functions
  auto funcTy = FunctionType::get(m_builder->getVoidTy(), {}, false);
  auto func = Function::Create(funcTy, GlobalValue::ExternalLinkage, "main", m_module);
  func->addFnAttr(Attribute::NoUnwind);

  // Currently PAL does not support the debug section in the elf file
  if (!cl::TrimDebugInfo)
    createDbgInfo(*m_module, func);

  // Create function blocks
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", func);
  auto earlyRetBlock = BasicBlock::Create(*m_context, ".earlyRet", func);
  auto mainBlock = BasicBlock::Create(*m_context, ".main", func);
  auto endBlock = BasicBlock::Create(*m_context, ".end", func);

  lgc::Pipeline::markShaderEntryPoint(func, lgc::ShaderStageCompute);

  // Construct entry block guard the launchId from launchSize
  m_builder->SetInsertPoint(entryBlock);
  createDispatchRaysInfoDesc();
  Value *launchSize = loadShaderTableVariable(ShaderTable::LaunchSize, m_dispatchRaysInfoDesc);
  auto builtIn = lgc::BuiltInGlobalInvocationId;
  lgc::InOutInfo inputInfo = {};
  auto launchlId = m_builder->CreateReadBuiltInInput(builtIn, inputInfo, nullptr, nullptr, "");
  auto launchSizeX = m_builder->CreateExtractElement(launchSize, uint64_t(0));
  auto launchSizeY = m_builder->CreateExtractElement(launchSize, 1);
  auto launchIdX = m_builder->CreateExtractElement(launchlId, uint64_t(0));
  auto launchIdY = m_builder->CreateExtractElement(launchlId, 1);
  auto idOutOfRangX = m_builder->CreateICmpUGE(launchIdX, launchSizeX);
  auto idOutOfRangY = m_builder->CreateICmpUGE(launchIdY, launchSizeY);
  auto idOutOfRange = m_builder->CreateOr(idOutOfRangX, idOutOfRangY);
  m_builder->CreateCondBr(idOutOfRange, earlyRetBlock, mainBlock);

  // Construct early return block
  m_builder->SetInsertPoint(earlyRetBlock);
  m_builder->CreateRetVoid();

  // Construct main block
  m_builder->SetInsertPoint(mainBlock);
  auto rayGenId = getShaderIdentifier(m_shaderStage, m_builder->getInt32(0), m_dispatchRaysInfoDesc);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  bool indirect = rayTracingContext->getIndirectStageMask() & shaderStageToMask(m_shaderStage);
  if (!indirect) {
    // Create Shader selection
    rayGenId = m_builder->CreateTrunc(rayGenId, m_builder->getInt32Ty());
    createShaderSelection(func, mainBlock, endBlock, rayGenId, RayTracingContext::InvalidShaderId, m_shaderStage, {},
                          nullptr, m_builder->getVoidTy());
  } else {
    auto funcTy = FunctionType::get(m_builder->getVoidTy(), {}, false);
    auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);
    if (rayTracingContext->isReplay()) {
      auto remapFunc = getOrCreateRemapCapturedVaToReplayVaFunc();
      rayGenId = m_builder->CreateCall(remapFunc->getFunctionType(), remapFunc, rayGenId);
    }
    auto funcPtr = m_builder->CreateIntToPtr(rayGenId, funcPtrTy);
    CallInst *call = m_builder->CreateCall(funcTy, funcPtr, {});
    call->setCallingConv(CallingConv::SPIR_FUNC);
    m_builder->CreateBr(endBlock);
  }
  // Construct end block
  m_builder->SetInsertPoint(endBlock);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Create DICompileUnit and DISubprogram
//
// @param module : LLVM module to be used by the DIBuilder
// @param func : Function to process
void SpirvLowerRayTracing::createDbgInfo(Module &module, Function *func) {
  DIBuilder builder(module);
  DIFile *file = builder.createFile(func->getName(), ".");
  builder.createCompileUnit(dwarf::DW_LANG_C99, file, "llvmIR", false, "", 0, "", DICompileUnit::LineTablesOnly);
  // Create the DISubprogram for the module entry function
  auto *funcTy = builder.createSubroutineType(builder.getOrCreateTypeArray({}));
  auto spFlags = DISubprogram::SPFlagDefinition;
  auto subProgram =
      builder.createFunction(file, func->getName(), module.getName(), file, 0, funcTy, 0, DINode::FlagZero, spFlags);
  auto dbgInfoLoc = DILocation::get(subProgram->getContext(), 0, 0, subProgram);
  func->setSubprogram(subProgram);
  // Builder finalize to remove temporary node
  builder.finalize();
  m_builder->SetCurrentDebugLocation(dbgInfoLoc);
}

// =====================================================================================================================
// Clone DISubprogram to the new function
//
// @param func : Old Function to be deprecated
// @param newFunc : New Function to be processed
void SpirvLowerRayTracing::cloneDbgInfoSubgrogram(llvm::Function *func, llvm::Function *newFunc) {
  if (auto subprogram = func->getSubprogram()) {
    auto metadata = MDString::get(*m_context, newFunc->getName());
    // Replace DISubProgram name and linkname to the new function name
    subprogram->replaceOperandWith(2, metadata); // DISubProgram Name
    subprogram->replaceOperandWith(3, metadata); // DISubProgram Link name
    newFunc->setSubprogram(subprogram);
    auto dbgInfoLoc = DILocation::get(subprogram->getContext(), 0, 0, subprogram);
    m_builder->SetCurrentDebugLocation(dbgInfoLoc);
  }
}

// =====================================================================================================================
// Process OpTerminateRay and OpIgnoreIntersection
//
// @param func : Processed function
// @param callInst : CallInst of terminal op
// @param hitStatus : Ray hit Status
void SpirvLowerRayTracing::processTerminalFunc(Function *func, CallInst *callInst, RayHitStatus hitStatus) {

  // .entry:
  // ...
  //    call void @TerminateRayKHR()
  // ...
  //    ret void
  //
  // ==>
  //
  // .entry:
  // ...
  //    store i32 2, i32 addrspace(7)* %HitAttibute2
  //    ret void
  // .split:
  // ...

  // Set the status
  m_builder->SetInsertPoint(callInst);
  m_builder->CreateStore(m_builder->getInt32(hitStatus), m_traceParams[TraceParam::Status]);
  m_builder->CreateRetVoid();

  // Split the basic block at the instruction Call TerminatorRay/IgnoreIntersection
  BasicBlock *block = callInst->getParent();
  block->splitBasicBlock(callInst, ".split");
  block->getTerminator()->eraseFromParent();
  m_callsToLower.push_back(callInst);
}

// =====================================================================================================================
// Process termination after reportIntersection
//
// @param func : Processed function
// @param callInst : CallInst of reportInspection
void SpirvLowerRayTracing::processPostReportIntersection(Function *func, CallInst *callInst) {
  // .entry:
  // ...
  //    %check = call spir_func i1 @ReportIntersectionKHR
  // ...
  //    ret void
  //
  // ===>
  //
  // .entry:
  //     call spir_func i1 @ReportIntersectionKHR
  //     %check = load i1, i1 addrspace(5)* funcRetFlag
  //     br i1 %check, label %.ret, label %.split
  // .ret:
  //     ret void
  // .split:
  // ...

  auto currentBlock = callInst->getParent();
  auto splitBlock = currentBlock->splitBasicBlock(callInst->getNextNonDebugInstruction(), ".split");
  auto retBlock = BasicBlock::Create(*m_context, ".ret", func, splitBlock);
  m_builder->SetInsertPoint(retBlock);
  m_builder->CreateRetVoid();

  auto terminator = currentBlock->getTerminator();
  m_builder->SetInsertPoint(terminator);
  m_builder->CreateCondBr(m_builder->CreateLoad(m_builder->getInt1Ty(), m_funcRetFlag), retBlock, splitBlock);

  terminator->dropAllReferences();
  terminator->eraseFromParent();
}

// =====================================================================================================================
// Create traceray module entry function
void SpirvLowerRayTracing::createTraceRay() {
  assert(m_shaderStage == ShaderStageCompute);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  bool indirect = rayTracingContext->getIndirectStageMask() & ShaderStageComputeBit;

  auto funcTy = getTraceRayFuncTy();
  StringRef funcName = RtName::TraceRayKHR;
  Function *func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, m_module);
  func->setCallingConv(CallingConv::SPIR_FUNC);
  if (!indirect)
    func->addFnAttr(Attribute::AlwaysInline);

  func->addFnAttr(Attribute::NoUnwind);

  // Currently PAL does not support the debug section in the elf file
  if (!cl::TrimDebugInfo)
    createDbgInfo(*m_module, func);

  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  // traceRaysInline argument types
  Type *funcArgTys[] = {
      m_builder->getInt32Ty(), // 0, Scene Addr low
      m_builder->getInt32Ty(), // 1, Scene Addr high
      m_builder->getInt32Ty(), // 2, Ray flags
      m_builder->getInt32Ty(), // 3, InstanceInclusionMask
      m_builder->getInt32Ty(), // 4, RayContributionToHitGroupIndex
      m_builder->getInt32Ty(), // 5, MultiplierForGeometryContributionToShaderIndex
      m_builder->getInt32Ty(), // 6, MissShaderIndex
      m_builder->getFloatTy(), // 7, OriginX
      m_builder->getFloatTy(), // 8, OriginY
      m_builder->getFloatTy(), // 9, OriginZ
      m_builder->getFloatTy(), // 10, TMin
      m_builder->getFloatTy(), // 11, DirX
      m_builder->getFloatTy(), // 12, DirY
      m_builder->getFloatTy(), // 13, DirZ
      m_builder->getFloatTy()  // 14, TMax
  };

  SmallVector<Value *, 8> traceRaysArgs(TraceRayLibFuncParam::Count);
  for (size_t i = 0; i < traceRaysArgs.size(); ++i)
    traceRaysArgs[i] = m_builder->CreateAlloca(funcArgTys[i], SPIRAS_Private);

  auto argIt = func->arg_begin();
  // Payload
  Value *arg = argIt++;
  m_builder->CreateStore(arg, m_globalPayload);

  // 0, Scene Addr low  1, Scene Addr high
  arg = argIt++;

  Value *sceneAddLow = m_builder->CreateExtractElement(arg, uint64_t(0));
  Value *sceneAddHigh = m_builder->CreateExtractElement(arg, 1);

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 34
  {
    // For GPURT major version < 34, GPURT expect base address of acceleration structure being passed, which is stored
    // at offset 0 of the resource.
    auto gpuLowAddr = m_builder->CreateZExt(sceneAddLow, m_builder->getInt64Ty());
    auto gpuHighAddr = m_builder->CreateZExt(sceneAddHigh, m_builder->getInt64Ty());
    gpuHighAddr = m_builder->CreateShl(gpuHighAddr, m_builder->getInt64(32));
    auto gpuAddr = m_builder->CreateOr(gpuLowAddr, gpuHighAddr);

    Type *gpuAddrAsPtrTy = PointerType::get(*m_context, SPIRAS_Global);
    auto loadPtr = m_builder->CreateIntToPtr(gpuAddr, gpuAddrAsPtrTy);
    auto loadTy = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);

    Value *loadValue = nullptr;

    if (m_context->getPipelineContext()->getPipelineOptions()->extendedRobustness.nullDescriptor) {
      // We should not load from a null descriptor (if it is allowed).
      // We do:
      // .entry:
      //   ...
      //   %gpuAddr = ...
      //   %loadPtr = inttoptr %gpuAddr
      //   %isDescValid = icmp ne %gpuAddr, 0
      //   br %isDescValid, label %.loadDescriptor, label %.continue
      //
      // .loadDescriptor:
      //   %AS = load %loadPtr
      //
      // .continue:
      //   %loadVal = phi [ %AS, %.loadDescriptor ], [ 0, %.entry ]

      BasicBlock *loadDescriptorBlock = BasicBlock::Create(*m_context, ".loadDescriptor", func);
      BasicBlock *continueBlock = BasicBlock::Create(*m_context, ".continue", func);

      auto isDescValid = m_builder->CreateICmpNE(gpuAddr, m_builder->getInt64(0));
      m_builder->CreateCondBr(isDescValid, loadDescriptorBlock, continueBlock);

      m_builder->SetInsertPoint(loadDescriptorBlock);
      auto accelerationStructureAddr = m_builder->CreateLoad(loadTy, loadPtr);
      m_builder->CreateBr(continueBlock);

      m_builder->SetInsertPoint(continueBlock);
      auto phi = m_builder->CreatePHI(loadTy, 2);
      phi->addIncoming(accelerationStructureAddr, loadDescriptorBlock);
      auto zero = m_builder->getInt32(0);
      phi->addIncoming(ConstantVector::get({zero, zero}), entryBlock);
      loadValue = phi;
    } else {
      loadValue = m_builder->CreateLoad(loadTy, loadPtr);
    }

    sceneAddLow = m_builder->CreateExtractElement(loadValue, uint64_t(0));
    sceneAddHigh = m_builder->CreateExtractElement(loadValue, 1);
  }
#endif

  m_builder->CreateStore(sceneAddLow, traceRaysArgs[TraceRayLibFuncParam::AcceleStructLo]);
  m_builder->CreateStore(sceneAddHigh, traceRaysArgs[TraceRayLibFuncParam::AcceleStructHi]);

  // 2, Ray flags
  arg = argIt++;
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::RayTracingFlags]);

  // 3, InstanceInclusionMask, Cull Mask, only 8 bits used for this value, other bits are ignored
  arg = argIt++;
  arg = m_builder->CreateAnd(arg, m_builder->getInt32(255));
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::InstanceInclusionMask]);

  // 4, RayContributionToHitGroupIndex, SBT_OFFSET, only 4 bits used for this value, other bits are ignored
  arg = argIt++;
  arg = m_builder->CreateAnd(arg, m_builder->getInt32(15));
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::RayContributionToHitGroupIndex]);

  // 5, MultiplierForGeometryContributionToShaderIndex, SBT_STRIDE, only 4 bits used for this value
  arg = argIt++;
  arg = m_builder->CreateAnd(arg, m_builder->getInt32(15));
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::MultiplierForGeometryContributionToShaderIndex]);

  // 6, MissShaderIndex
  arg = argIt++;
  // Only the 16 least-significant bits of miss shader index are used by this instruction - other bits are ignored.
  arg = m_builder->CreateAnd(arg, m_builder->getInt32(UINT16_MAX));
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::MissShaderIndex]);

  // 7, OriginX, 8，OriginY, 9，OriginZ
  arg = argIt++;
  Value *elem0 = m_builder->CreateExtractElement(arg, uint64_t(0));
  Value *elem1 = m_builder->CreateExtractElement(arg, 1);
  Value *elem2 = m_builder->CreateExtractElement(arg, 2);
  m_builder->CreateStore(elem0, traceRaysArgs[TraceRayLibFuncParam::OriginX]);
  m_builder->CreateStore(elem1, traceRaysArgs[TraceRayLibFuncParam::OriginY]);
  m_builder->CreateStore(elem2, traceRaysArgs[TraceRayLibFuncParam::OriginZ]);

  // 10, TMin
  arg = argIt++;
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::TMin]);

  // 11, DirX  12, DirY 13, DirZ
  arg = argIt++;
  elem0 = m_builder->CreateExtractElement(arg, uint64_t(0));
  elem1 = m_builder->CreateExtractElement(arg, 1);
  elem2 = m_builder->CreateExtractElement(arg, 2);
  m_builder->CreateStore(elem0, traceRaysArgs[TraceRayLibFuncParam::DirX]);
  m_builder->CreateStore(elem1, traceRaysArgs[TraceRayLibFuncParam::DirY]);
  m_builder->CreateStore(elem2, traceRaysArgs[TraceRayLibFuncParam::DirZ]);

  // 14, TMax
  const float rayTMax = m_context->getPipelineContext()->getRayTracingState()->maxRayLength;
  if (rayTMax > 0.0) {
    arg = ConstantFP::get(m_builder->getFloatTy(), rayTMax);
  } else {
    arg = argIt;
  }
  m_builder->CreateStore(arg, traceRaysArgs[TraceRayLibFuncParam::TMax]);

  // Parent ray ID and static ID if logging feature is enabled
  if (rayTracingContext->getRayTracingState()->enableRayTracingCounters) {
    arg = ++argIt;
    m_builder->CreateStore(arg, m_traceParams[TraceParam::ParentRayId]);
    arg = ++argIt;
    m_builder->CreateStore(arg, m_traceRayStaticId);
  }

  // Call TraceRay function from traceRays module
  auto result =
      m_builder->CreateNamedCall(m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_TRACE_RAY),
                                 m_builder->getVoidTy(), traceRaysArgs, {Attribute::NoUnwind, Attribute::AlwaysInline});

  (void(result)); // unused
  m_builder->CreateRet(m_builder->CreateLoad(m_globalPayload->getValueType(), m_globalPayload));
}

// =====================================================================================================================
// init TraceParam types
//
// @param traceParam : trace params
void SpirvLowerRayTracing::initTraceParamsTy(unsigned attributeSize) {
  auto floatx3Ty = FixedVectorType::get(Type::getFloatTy(*m_context), 3);
  m_traceParamsTys = {
      m_builder->getInt32Ty(),                                        // 1, rayFlags
      m_builder->getInt32Ty(),                                        // 2, instanceInclusionMask
      floatx3Ty,                                                      // 3, origin
      m_builder->getFloatTy(),                                        // 4, tMin
      floatx3Ty,                                                      // 5, dir
      m_builder->getFloatTy(),                                        // 6, tMax
      m_builder->getFloatTy(),                                        // 7, tCurrent
      m_builder->getInt32Ty(),                                        // 8, kind
      m_builder->getInt32Ty(),                                        // 9, status
      m_builder->getInt32Ty(),                                        // 10, instNodeAddrLo
      m_builder->getInt32Ty(),                                        // 11, instNodeAddrHi
      m_builder->getInt32Ty(),                                        // 12, primitiveIndex
      m_builder->getInt32Ty(),                                        // 13, duplicateAnyHit
      m_builder->getInt32Ty(),                                        // 14, geometryIndex
      ArrayType::get(m_builder->getFloatTy(), attributeSize),         // 15, hit attribute
      m_builder->getInt32Ty(),                                        // 16, parentId
      StructType::get(*m_context, {floatx3Ty, floatx3Ty, floatx3Ty}), // 16, HitTriangleVertexPositions
  };
  TraceParamsTySize[TraceParam::HitAttributes] = attributeSize;
  assert(sizeof(TraceParamsTySize) / sizeof(TraceParamsTySize[0]) == TraceParam::Count);
}

// =====================================================================================================================
// Initialize m_globalPayloads
void SpirvLowerRayTracing::initGlobalPayloads() {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  // Payload max size in bytes
  const auto payloadType = rayTracingContext->getPayloadType(m_builder);
  if (m_globalPayload == nullptr) {
    m_globalPayload =
        new GlobalVariable(*m_module, payloadType, false, GlobalValue::ExternalLinkage, nullptr,
                           Twine(RtName::GlobalPayload), nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
  }
}

// =====================================================================================================================
// Initialize m_globalCallableData
void SpirvLowerRayTracing::initGlobalCallableData() {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  // Callable data max size in bytes
  const auto dataType = rayTracingContext->getCallableDataType(m_builder);
  if (m_globalCallableData == nullptr) {
    m_globalCallableData =
        new GlobalVariable(*m_module, dataType, false, GlobalValue::ExternalLinkage, nullptr,
                           Twine(RtName::GlobalCallableData), nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
  }
}

// =====================================================================================================================
// Initialize builting for shader call
void SpirvLowerRayTracing::initShaderBuiltIns() {
  assert(m_builtInParams.size() == 0);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  auto &contextBuiltIn = rayTracingContext->getBuiltIns();

  for (const unsigned builtIn : contextBuiltIn) {
    switch (builtIn) {
    case BuiltInPrimitiveId:
      m_builtInParams.insert(TraceParam::PrimitiveIndex);
      break;
    case BuiltInHitKindKHR:
      m_builtInParams.insert(TraceParam::Kind);
      break;
    case BuiltInIncomingRayFlagsKHR:
      m_builtInParams.insert(TraceParam::RayFlags);
      break;
    case BuiltInInstanceCustomIndexKHR:
      m_builtInParams.insert(TraceParam::InstNodeAddrLo);
      m_builtInParams.insert(TraceParam::InstNodeAddrHi);
      break;
    case BuiltInInstanceId:
      m_builtInParams.insert(TraceParam::InstNodeAddrLo);
      m_builtInParams.insert(TraceParam::InstNodeAddrHi);
      break;
    case BuiltInRayTminKHR:
      m_builtInParams.insert(TraceParam::TMin);
      break;
    case BuiltInWorldRayOriginKHR:
      m_builtInParams.insert(TraceParam::Origin);
      break;
    case BuiltInWorldRayDirectionKHR:
      m_builtInParams.insert(TraceParam::Dir);
      break;
    case BuiltInRayGeometryIndexKHR:
      m_builtInParams.insert(TraceParam::GeometryIndex);
      break;
    case BuiltInHitTNV:
    case BuiltInRayTmaxKHR:
      m_builtInParams.insert(TraceParam::TMax);
      break;
    case BuiltInObjectToWorldKHR:
    case BuiltInWorldToObjectKHR: {
      m_builtInParams.insert(TraceParam::InstNodeAddrLo);
      m_builtInParams.insert(TraceParam::InstNodeAddrHi);
      break;
    }
    case BuiltInObjectRayOriginKHR: {
      m_builtInParams.insert(TraceParam::InstNodeAddrLo);
      m_builtInParams.insert(TraceParam::InstNodeAddrHi);
      m_builtInParams.insert(TraceParam::Origin);
      break;
    }
    case BuiltInObjectRayDirectionKHR: {
      m_builtInParams.insert(TraceParam::InstNodeAddrLo);
      m_builtInParams.insert(TraceParam::InstNodeAddrHi);
      m_builtInParams.insert(TraceParam::Dir);
      break;
    }
    case BuiltInCullMaskKHR: {
      m_builtInParams.insert(TraceParam::InstanceInclusionMask);
      break;
    }
    case BuiltInHitTriangleVertexPositionsKHR: {
      m_builtInParams.insert(TraceParam::HitTriangleVertexPositions);
      break;
    }
    default:
      llvm_unreachable("Unexpected built-in!");
      break;
    }
  }

  if (rayTracingContext->getHitAttribute())
    m_builtInParams.insert(TraceParam::HitAttributes);
}

// =====================================================================================================================
// Get closeshit/miss/anyhit/intersect entry function type
//
// @param func : The shader stage of entry function
FunctionType *SpirvLowerRayTracing::getShaderEntryFuncTy(ShaderStage stage) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  SmallVector<Type *, 8> argTys;
  auto payloadTy = rayTracingContext->getPayloadType(m_builder);
  argTys.push_back(payloadTy);

  auto retTy = getShaderReturnTy(stage);

  for (auto &builtIn : m_builtInParams) {
    argTys.push_back(m_traceParamsTys[builtIn]);
  }

  for (auto &param : getShaderExtraInputParams(stage)) {
    argTys.push_back(m_traceParamsTys[param]);
  }

  argTys.push_back(m_builder->getInt32Ty());

  return FunctionType::get(retTy, argTys, false);
}

// =====================================================================================================================
// Mutate entry function for the shader stage, ClosestHit, Intersect, AnyHit, Miss
//
// @param func : Function to create
Instruction *SpirvLowerRayTracing::createEntryFunc(Function *func) {
  // Set old entry function name deprecated
  func->setName("deprecated");

  // Create new entry function with new payload and builtIns arguments
  auto newFuncTy = getShaderEntryFuncTy(m_shaderStage);
  Function *newFunc = Function::Create(newFuncTy, GlobalValue::ExternalLinkage, m_module->getName(), m_module);
  newFunc->setCallingConv(CallingConv::SPIR_FUNC);

  // Transfer code from old entry function to the new entry function
  while (!func->empty()) {
    BasicBlock *block = &func->front();
    block->removeFromParent();
    block->insertInto(newFunc);
  }

  // Transfer DiSubprogram to the new function
  cloneDbgInfoSubgrogram(func, newFunc);

  // Now entry function pointer to the new function
  m_entryPoint = newFunc;
  m_entryPoint->addFnAttr(Attribute::NoUnwind);
  m_entryPoint->addFnAttr(Attribute::AlwaysInline);

  Instruction *insertPos = &*(newFunc->begin()->getFirstNonPHIOrDbgOrAlloca());
  m_builder->SetInsertPoint(insertPos);
  auto argIt = newFunc->arg_begin();

  // Save the function input parameter value to the global payloads and builtIns
  // the global payload here are needed for the recursive traceray function of the shader stage
  Value *arg = argIt++;
  m_builder->CreateStore(arg, m_globalPayload);

  for (auto &builtIn : m_builtInParams) {
    Value *arg = argIt++;
    m_builder->CreateStore(arg, m_traceParams[builtIn]);
  }

  for (auto &param : getShaderExtraInputParams(m_shaderStage)) {
    Value *arg = argIt++;
    m_builder->CreateStore(arg, m_traceParams[param]);
  }

  m_shaderRecordIndex = argIt;

  // Initialize hit status for intersection shader (ignore) and any hit shader (accept)
  if (m_shaderStage == ShaderStageRayTracingIntersect || m_shaderStage == ShaderStageRayTracingAnyHit) {
    RayHitStatus hitStatus =
        m_shaderStage == ShaderStageRayTracingIntersect ? RayHitStatus::Ignore : RayHitStatus::Accept;
    m_builder->CreateStore(m_builder->getInt32(hitStatus), m_traceParams[TraceParam::Status]);
  }
  return insertPos;
}

// =====================================================================================================================
// Update global variable from function parameters, assuming the m_builder has been setup
//
// @param func : Function to create
// @param stage : Ray tracing shader stage
void SpirvLowerRayTracing::updateGlobalFromCallShaderFunc(Function *func, ShaderStage stage) {
  auto zero = m_builder->getInt32(0);
  auto one = m_builder->getInt32(1);

  if (stage == ShaderStageRayTracingAnyHit) {
    // Third function parameter attribute
    Value *attrib = func->arg_begin() + 2;

    Value *attribValue0 = m_builder->CreateExtractElement(attrib, uint64_t(0));
    Type *hitAttribEltTy = m_traceParamsTys[TraceParam::HitAttributes];
    Value *attribDestPtr = m_builder->CreateGEP(hitAttribEltTy, m_traceParams[TraceParam::HitAttributes], {zero, zero});
    m_builder->CreateStore(attribValue0, attribDestPtr);

    Value *attribValue1 = m_builder->CreateExtractElement(attrib, 1);
    attribDestPtr = m_builder->CreateGEP(hitAttribEltTy, m_traceParams[TraceParam::HitAttributes], {zero, one});
    m_builder->CreateStore(attribValue1, attribDestPtr);
  }
}

// =====================================================================================================================
// Get callabe shader entry function type
FunctionType *SpirvLowerRayTracing::getCallableShaderEntryFuncTy() {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  SmallVector<Type *, 8> argTys;
  auto callableDataTy = rayTracingContext->getCallableDataType(m_builder);
  argTys.push_back(callableDataTy);
  // Add shaderRecordIndex type
  argTys.push_back(m_builder->getInt32Ty());

  return FunctionType::get(callableDataTy, argTys, false);
}

// =====================================================================================================================
// Get traceray function type
FunctionType *SpirvLowerRayTracing::getTraceRayFuncTy() {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  auto retTy = rayTracingContext->getPayloadType(m_builder);
  SmallVector<Type *, 11> argsTys = {
      rayTracingContext->getPayloadType(m_builder),     // Payload
      FixedVectorType::get(m_builder->getInt32Ty(), 2), // Acceleration structure
      m_builder->getInt32Ty(),                          // Ray flags
      m_builder->getInt32Ty(),                          // Cull mask
      m_builder->getInt32Ty(),                          // Shader binding table offset
      m_builder->getInt32Ty(),                          // Shader binding table stride
      m_builder->getInt32Ty(),                          // Miss shader index
      FixedVectorType::get(m_builder->getFloatTy(), 3), // Ray origin
      m_builder->getFloatTy(),                          // Ray Tmin
      FixedVectorType::get(m_builder->getFloatTy(), 3), // Ray direction
      m_builder->getFloatTy(),                          // Ray Tmax
  };

  // Add parent ray ID and static ID if logging feature is enabled.
  if (m_context->getPipelineContext()->getRayTracingState()->enableRayTracingCounters) {
    argsTys.push_back(m_builder->getInt32Ty());
    argsTys.push_back(m_builder->getInt32Ty());
  }

  auto funcTy = FunctionType::get(retTy, argsTys, false);
  return funcTy;
}

// =====================================================================================================================
// Mutate entry function for the shader stage callable shader
//
// @param func : Function to create
Instruction *SpirvLowerRayTracing::createCallableShaderEntryFunc(Function *func) {
  // Set old entry function name deprecated
  func->setName("deprecatedCallableShader");

  // Create new entry function with new callable data
  auto newFuncTy = getCallableShaderEntryFuncTy();
  Function *newFunc = Function::Create(newFuncTy, GlobalValue::ExternalLinkage, m_module->getName(), m_module);
  newFunc->setCallingConv(CallingConv::C);

  // Transfer code from old entry function to the new entry function
  while (!func->empty()) {
    BasicBlock *block = &func->front();
    block->removeFromParent();
    block->insertInto(newFunc);
  }

  // Transfer DiSubprogram to the new function
  cloneDbgInfoSubgrogram(func, newFunc);

  // Now entry function pointer to the new function
  m_entryPoint = newFunc;
  m_entryPoint->addFnAttr(Attribute::NoUnwind);
  m_entryPoint->addFnAttr(Attribute::AlwaysInline);
  Instruction *insertPos = &*(newFunc->begin()->getFirstNonPHIOrDbgOrAlloca());
  m_builder->SetInsertPoint(insertPos);

  auto argIt = newFunc->arg_begin();

  // Save the function input parameter value to the global callable
  // the global payload here are needed for the recursive traceray function of the shader stage
  Value *arg = argIt++;
  m_builder->CreateStore(arg, m_globalCallableData);

  // Save the shader record index
  m_shaderRecordIndex = argIt;

  return insertPos;
}

// =====================================================================================================================
// Get all the function ReturnInst
//
// @param func : Function to gather ReturnInst
// @param rets : returned vector of  ReturnInst instructions
void SpirvLowerRayTracing::getFuncRets(Function *func, SmallVector<Instruction *, 4> &rets) {
  for (auto &block : *func) {
    auto blockTerm = block.getTerminator();
    if (blockTerm != nullptr && isa<ReturnInst>(blockTerm))
      rets.push_back(blockTerm);
  }
}

// =====================================================================================================================
// Get the extra parameters needed for calling indirect shader
//
// @param stage : The shader stage of shader to call
SmallSet<unsigned, 4> SpirvLowerRayTracing::getShaderExtraInputParams(ShaderStage stage) {
  SmallSet<unsigned, 4> params;

  switch (stage) {
  case ShaderStageRayTracingIntersect:
    params.insert(TraceParam::TMin);
    params.insert(TraceParam::TMax);
    params.insert(TraceParam::TCurrent);
    params.insert(TraceParam::Kind);
    params.insert(TraceParam::DuplicateAnyHit);
    break;
  default:
    break;
  }

  // Add parent ray ID if logging feature is enabled.
  if (m_context->getPipelineContext()->getRayTracingState()->enableRayTracingCounters)
    params.insert(TraceParam::ParentRayId);

  // Remove duplicated ones
  for (auto builtIn : m_builtInParams) {
    if (params.contains(builtIn))
      params.erase(builtIn);
  }

  return params;
}

// =====================================================================================================================
// Get the extra return values needed for indirect shader, in addition to payload
//
// @param stage : The shader stage
SmallSet<unsigned, 4> SpirvLowerRayTracing::getShaderExtraRets(ShaderStage stage) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  SmallSet<unsigned, 4> rets;

  switch (stage) {
  case ShaderStageRayTracingIntersect:
    rets.insert(TraceParam::TMax);
    rets.insert(TraceParam::TCurrent);
    rets.insert(TraceParam::Kind);
    rets.insert(TraceParam::Status);
    rets.insert(TraceParam::DuplicateAnyHit);
    // Intersection shader need to output HitAttribute if necessary
    if (rayTracingContext->getHitAttribute())
      rets.insert(TraceParam::HitAttributes);
    break;
  case ShaderStageRayTracingAnyHit:
    rets.insert(TraceParam::Status);
    break;
  default:
    break;
  }

  return rets;
}

// =====================================================================================================================
// Get return type for specific shader stage
//
// @param stage : The shader stage
Type *SpirvLowerRayTracing::getShaderReturnTy(ShaderStage stage) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());

  // Return payload in default
  auto returnTySizeInDword = rayTracingContext->getPayloadSizeInDword();
  const auto &retParams = getShaderExtraRets(stage);

  for (auto param : retParams)
    returnTySizeInDword += TraceParamsTySize[param];

  return ArrayType::get(m_builder->getInt32Ty(), returnTySizeInDword);
}

// =====================================================================================================================
// Store function call result to payload and other global variables
//
// @param stage : The shader stage
// @param result : The result to store
void SpirvLowerRayTracing::storeFunctionCallResult(ShaderStage stage, Value *result) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());

  unsigned payloadSizeInDword = rayTracingContext->getPayloadSizeInDword();

  const auto &rets = getShaderExtraRets(stage);
  if (!rets.size()) {
    // No extra return value, only return payload
    m_builder->CreateStore(result, m_globalPayload);
  } else {
    // Return extra values
    Value *payloadVal = PoisonValue::get(rayTracingContext->getPayloadType(m_builder));
    unsigned index = 0;

    // Store payload first
    for (; index < payloadSizeInDword; index++)
      payloadVal = m_builder->CreateInsertValue(payloadVal, m_builder->CreateExtractValue(result, index), index);
    m_builder->CreateStore(payloadVal, m_globalPayload);

    // Store extra values, do bitcast if needed
    for (auto ret : rets) {
      Value *retVal = nullptr;
      // If TraceParams type is vector or array
      if (m_traceParamsTys[ret]->isVectorTy() || m_traceParamsTys[ret]->isArrayTy()) {
        retVal = PoisonValue::get(m_traceParamsTys[ret]);
        for (unsigned i = 0; i < TraceParamsTySize[ret]; ++i) {
          Value *retElement = m_builder->CreateExtractValue(result, index++);
          retElement = m_builder->CreateBitCast(retElement, m_traceParamsTys[ret]->getArrayElementType());
          retVal = m_builder->CreateInsertValue(retVal, retElement, i);
        }
      } else {
        assert(TraceParamsTySize[ret] == 1);
        retVal = m_builder->CreateBitCast(m_builder->CreateExtractValue(result, index++), m_traceParamsTys[ret]);
      }

      m_builder->CreateStore(retVal, m_traceParams[ret]);
    }
  }
}

// =====================================================================================================================
// Init inputResult from payload and other global variables
//
// @param stage : The shader stage
// @param payload : The value to initialize first part of inputResult
// @param traceParams : The value to initialize second part of inputResult
// @param result : The result to initialize
void SpirvLowerRayTracing::initInputResult(ShaderStage stage, Value *payload, Value *traceParams[], Value *result) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());

  unsigned payloadSizeInDword = rayTracingContext->getPayloadSizeInDword();

  const auto &rets = getShaderExtraRets(stage);
  if (!rets.size()) {
    // No extra return value, initialize inputResult directly
    m_builder->CreateStore(payload, result);
  } else {
    // Create inputResult values
    Value *resultVal = PoisonValue::get(getShaderReturnTy(stage));
    unsigned index = 0;

    // Initialize inputResultVal from payload first
    for (; index < payloadSizeInDword; index++)
      resultVal = m_builder->CreateInsertValue(resultVal, m_builder->CreateExtractValue(payload, index), index);

    // Initialize inputResultVal from extra values, do bitcast if needed
    for (auto ret : rets) {
      Value *param = traceParams[ret] == nullptr ? m_builder->CreateLoad(m_traceParamsTys[ret], m_traceParams[ret])
                                                 : traceParams[ret];
      // If TraceParams type is vector or array
      if (m_traceParamsTys[ret]->isVectorTy() || m_traceParamsTys[ret]->isArrayTy()) {
        for (unsigned i = 0; i < TraceParamsTySize[ret]; ++i) {
          Value *paramElement = m_builder->CreateExtractValue(param, i);
          resultVal = m_builder->CreateInsertValue(
              resultVal, m_builder->CreateBitCast(paramElement, m_builder->getInt32Ty()), index++);
        }
      } else {
        assert(TraceParamsTySize[ret] == 1);
        param = m_builder->CreateBitCast(param, m_builder->getInt32Ty());
        resultVal = m_builder->CreateInsertValue(resultVal, param, index++);
      }
    }

    // Store the inputResultVal
    m_builder->CreateStore(resultVal, result);
  }
}

// =====================================================================================================================
// Load ObjectToWorld or WorldToObject matrix
//
// @param builtInId : ID of the built-in variable
Value *SpirvLowerRayTracing::createLoadRayTracingMatrix(unsigned builtInId, Instruction *insertPos) {
  assert(builtInId == BuiltInWorldToObjectKHR || builtInId == BuiltInObjectToWorldKHR);
  m_builder->SetInsertPoint(insertPos);

  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  auto instNodeAddrTy = m_traceParamsTys[TraceParam::InstNodeAddrLo];
  assert(instNodeAddrTy == m_traceParamsTys[TraceParam::InstNodeAddrHi]);
  Value *zero = m_builder->getInt32(0);

  // Get matrix address from instance node address
  Value *instNodeAddrLo = m_builder->CreateLoad(instNodeAddrTy, m_traceParams[TraceParam::InstNodeAddrLo]);
  Value *instNodeAddrHi = m_builder->CreateLoad(instNodeAddrTy, m_traceParams[TraceParam::InstNodeAddrHi]);

  Value *instNodeAddr = PoisonValue::get(int32x2Ty);
  instNodeAddr = m_builder->CreateInsertElement(instNodeAddr, instNodeAddrLo, uint64_t(0));
  instNodeAddr = m_builder->CreateInsertElement(instNodeAddr, instNodeAddrHi, 1u);

  Value *matrixAddr = instNodeAddr;

  unsigned transformOffset = offsetof(RayTracingInstanceNode, desc.Transform);
  if (builtInId == BuiltInObjectToWorldKHR) {
    transformOffset = offsetof(RayTracingInstanceNode, extra.Transform);
  }

  Value *matrixOffset = PoisonValue::get(int32x2Ty);
  matrixOffset = m_builder->CreateInsertElement(matrixOffset, m_builder->getInt32(transformOffset), uint64_t(0));
  matrixOffset = m_builder->CreateInsertElement(matrixOffset, zero, 1);

  matrixAddr = m_builder->CreateAdd(matrixAddr, matrixOffset);

  return createLoadMatrixFromAddr(matrixAddr);
}

// =====================================================================================================================
// Process AmdTraceRaySetHitTriangleNodePointer function
//
// @param func : The function to create
void SpirvLowerRayTracing::createSetHitTriangleNodePointer(Function *func) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);
  if (m_builtInParams.find(TraceParam::HitTriangleVertexPositions) != m_builtInParams.end()) {
    Value *bvh = func->arg_begin();
    Value *nodePtr = func->arg_begin() + 1;

    Value *bvhPtr = m_builder->CreateAlloca(bvh->getType());
    Value *nodePtrPtr = m_builder->CreateAlloca(nodePtr->getType());

    m_builder->CreateStore(bvh, bvhPtr);
    m_builder->CreateStore(nodePtr, nodePtrPtr);

    auto triangleDataTy = m_traceParamsTys[TraceParam::HitTriangleVertexPositions];
    auto triangleData =
        m_builder->CreateNamedCall(RtName::FetchTrianglePositionFromNodePointer, triangleDataTy, {bvhPtr, nodePtrPtr},
                                   {Attribute::NoUnwind, Attribute::AlwaysInline});
    m_builder->CreateStore(triangleData, m_traceParams[TraceParam::HitTriangleVertexPositions]);
  }
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Process entry function return instruction, replace new return payload/etc info
//
// @param func : The function to process
void SpirvLowerRayTracing::createEntryTerminator(Function *func) {
  // Return incoming payload, and other values if needed
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  SmallVector<Instruction *, 4> rets;
  getFuncRets(func, rets);
  for (auto ret : rets) {
    m_builder->SetInsertPoint(ret);
    Value *retVal = m_builder->CreateLoad(m_globalPayload->getValueType(), m_globalPayload);

    const auto rets = getShaderExtraRets(m_shaderStage);
    unsigned payloadSizeInDword = rayTracingContext->getPayloadSizeInDword();

    if (rets.size()) {
      // We have extra values to return here
      Value *newRetVal = PoisonValue::get(getShaderReturnTy(m_shaderStage));
      unsigned index = 0;
      // Get payload value first
      for (; index < payloadSizeInDword; index++)
        newRetVal = m_builder->CreateInsertValue(newRetVal, m_builder->CreateExtractValue(retVal, index), index);
      // Get other values
      for (auto ret : rets) {
        Value *traceParam = m_builder->CreateLoad(m_traceParamsTys[ret], m_traceParams[ret]);
        // If TraceParams type is vector or array
        if (m_traceParamsTys[ret]->isVectorTy() || m_traceParamsTys[ret]->isArrayTy()) {
          for (unsigned i = 0; i < TraceParamsTySize[ret]; ++i) {
            Value *traceParamElement = m_builder->CreateExtractValue(traceParam, i);
            newRetVal = m_builder->CreateInsertValue(
                newRetVal, m_builder->CreateBitCast(traceParamElement, m_builder->getInt32Ty()), index++);
          }
        } else {
          assert(TraceParamsTySize[ret] == 1);
          newRetVal = m_builder->CreateInsertValue(
              newRetVal, m_builder->CreateBitCast(traceParam, m_builder->getInt32Ty()), index++);
        }
      }
      retVal = newRetVal;
    }

    Instruction *newfuncEnd = m_builder->CreateRet(retVal);
    ret->replaceAllUsesWith(newfuncEnd);
    ret->eraseFromParent();
  }
}

// =====================================================================================================================
// Add return callable data
//
// @param func : The function to process
void SpirvLowerRayTracing::createCallableShaderEntryTerminator(Function *func) {
  // return global callable data
  SmallVector<Instruction *, 4> rets;
  getFuncRets(func, rets);
  for (auto ret : rets) {
    m_builder->SetInsertPoint(ret);
    Instruction *newfuncEnd =
        m_builder->CreateRet(m_builder->CreateLoad(m_globalCallableData->getValueType(), m_globalCallableData));
    ret->replaceAllUsesWith(newfuncEnd);
    ret->eraseFromParent();
  }
}

// =====================================================================================================================
// Get RemapCapturedVaToReplayVa function for indirect pipeline capture replay, create it if it does not exist.
Function *SpirvLowerRayTracing::getOrCreateRemapCapturedVaToReplayVaFunc() {
  Function *func = dyn_cast_or_null<Function>(m_module->getFunction(RtName::RemapCapturedVaToReplayVa));
  // uint64_t RemapCapturedVaToReplayVa(uint64_t shdaerId) {
  //   // InternalBuffer contains array of Vkgc::RayTracingCaptureReplayVaMappingEntry
  //   numEntries = unsigned(InternalBuffer[0].capturedGpuVa)
  //
  //   for (unsigned i = 1; i <= numEntries; i++)
  //     if (shaderId == InternalBuffer[i].capturedGpuVa)
  //       return InternalBuffer[i].replayGpuVa
  //
  //   return 0
  // }
  if (!func) {
    // Guard original insert point
    IRBuilderBase::InsertPointGuard guard(*m_builder);

    Type *int8Ty = m_builder->getInt8Ty();
    Type *int32Ty = m_builder->getInt32Ty();
    Type *int64Ty = m_builder->getInt64Ty();

    // Takes a shader ID (uint64_t) and returns a remapped one (uint64_t)
    auto funcTy = FunctionType::get(int64Ty, {int64Ty}, false);
    func = Function::Create(funcTy, GlobalValue::InternalLinkage, RtName::RemapCapturedVaToReplayVa, m_module);
    func->addFnAttr(Attribute::NoUnwind);
    func->addFnAttr(Attribute::AlwaysInline);

    Value *shaderId = func->arg_begin();

    BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func);
    BasicBlock *loopConditionBlock = BasicBlock::Create(*m_context, ".loopCondition", func);
    BasicBlock *loopBodyBlock = BasicBlock::Create(*m_context, ".loopBody", func);
    BasicBlock *vaMatchBlock = BasicBlock::Create(*m_context, ".vaMatch", func);
    BasicBlock *vaMismatchBlock = BasicBlock::Create(*m_context, ".vaMismatch", func);
    BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);

    auto zero = m_builder->getInt32(0);
    auto one = m_builder->getInt32(1);
    auto entryStride = m_builder->getInt32(sizeof(Vkgc::RayTracingCaptureReplayVaMappingEntry));

    // Entry block
    m_builder->SetInsertPoint(entryBlock);

    auto loopIteratorPtr = m_builder->CreateAlloca(int32Ty, SPIRAS_Private);

    auto bufferDesc = m_builder->CreateLoadBufferDesc(Vkgc::InternalDescriptorSetId,
                                                      Vkgc::RtCaptureReplayInternalBufferBinding, zero, 0);

    auto numEntriesPtr = m_builder->CreateInBoundsGEP(int8Ty, bufferDesc, zero);
    auto numEntries = m_builder->CreateTrunc(m_builder->CreateLoad(int64Ty, numEntriesPtr), int32Ty);
    m_builder->CreateStore(one, loopIteratorPtr);
    m_builder->CreateBr(loopConditionBlock);

    // Loop condition block
    m_builder->SetInsertPoint(loopConditionBlock);

    auto loopIteratorVal = m_builder->CreateLoad(int32Ty, loopIteratorPtr);
    auto loopCondition = m_builder->CreateICmpULE(loopIteratorVal, numEntries);
    m_builder->CreateCondBr(loopCondition, loopBodyBlock, endBlock);

    // Loop body block
    m_builder->SetInsertPoint(loopBodyBlock);

    auto entryOffset = m_builder->CreateMul(loopIteratorVal, entryStride);
    auto capturedGpuVaPtr = m_builder->CreateInBoundsGEP(int8Ty, bufferDesc, entryOffset);
    auto capturedGpuVa = m_builder->CreateLoad(int64Ty, capturedGpuVaPtr);
    auto match = m_builder->CreateICmpEQ(shaderId, capturedGpuVa);
    m_builder->CreateCondBr(match, vaMatchBlock, vaMismatchBlock);

    // VA match block
    m_builder->SetInsertPoint(vaMatchBlock);

    auto replayGpuVaOffset = m_builder->CreateAdd(
        entryOffset, m_builder->getInt32(offsetof(Vkgc::RayTracingCaptureReplayVaMappingEntry, replayGpuVa)));
    auto replayGpuVaPtr = m_builder->CreateInBoundsGEP(int8Ty, bufferDesc, replayGpuVaOffset);
    auto replayGpuVa = m_builder->CreateLoad(int64Ty, replayGpuVaPtr);
    m_builder->CreateRet(replayGpuVa);

    // VA mismatch block
    m_builder->SetInsertPoint(vaMismatchBlock);

    m_builder->CreateStore(m_builder->CreateAdd(loopIteratorVal, one), loopIteratorPtr);
    m_builder->CreateBr(loopConditionBlock);

    // End block
    m_builder->SetInsertPoint(endBlock);
    m_builder->CreateRet(m_builder->getInt64(0));
  }

  return func;
}

// =====================================================================================================================
// Get DispatchRaysInfo Descriptor
//
// @param insertPos : Where to insert instructions
void SpirvLowerRayTracing::createDispatchRaysInfoDesc() {
  if (!m_dispatchRaysInfoDesc) {
    m_dispatchRaysInfoDesc = m_builder->CreateLoadBufferDesc(
        TraceRayDescriptorSet, RayTracingResourceIndexDispatchRaysInfo, m_builder->getInt32(0), 0);
  }
}

// =====================================================================================================================
// Visits "lgc.rt.accept.hit.and.end.search" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitAcceptHitAndEndSearchOp(AcceptHitAndEndSearchOp &inst) {
  processTerminalFunc(m_entryPoint, &cast<CallInst>(inst), RayHitStatus::AcceptAndEndSearch);
}

// =====================================================================================================================
// Visits "lgc.rt.ignore.hit" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitIgnoreHitOp(IgnoreHitOp &inst) {
  processTerminalFunc(m_entryPoint, &cast<CallInst>(inst), RayHitStatus::Ignore);
}

// =====================================================================================================================
// Visits "lgc.rt.trace.ray" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitTraceRayOp(TraceRayOp &inst) {
  processTraceRayCall(&inst);
}

// =====================================================================================================================
// Visits "lgc.gpurt.get.hit.attributes" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitGetHitAttributes(lgc::GpurtGetHitAttributesOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *tCurrent = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TCurrent], m_traceParams[TraceParam::TCurrent]);
  Value *kind = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Kind], m_traceParams[TraceParam::Kind]);
  Value *status = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Status], m_traceParams[TraceParam::Status]);

  m_builder->CreateStore(tCurrent, inst.getTCurrentPtr());
  m_builder->CreateStore(kind, inst.getKindPtr());
  m_builder->CreateStore(status, inst.getStatusPtr());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.hit.attributes" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetHitAttributes(lgc::GpurtSetHitAttributesOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createSetHitAttributes(func);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.trace.params" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetTraceParams(lgc::GpurtSetTraceParamsOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createSetTraceParams(func);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.closest.hit.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallClosestHitShader(lgc::GpurtCallClosestHitShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingClosestHit, RayTracingContext::InvalidShaderId,
                         m_builder->getTrue());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.miss.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallMissShader(lgc::GpurtCallMissShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingMiss, RayTracingContext::InvalidShaderId, m_builder->getTrue());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.triangle.any.hit.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallTriangleAnyHitShader(lgc::GpurtCallTriangleAnyHitShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingAnyHit, RayTracingContext::TriangleHitGroup, nullptr);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.intersection.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallIntersectionShader(lgc::GpurtCallIntersectionShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingIntersect, RayTracingContext::InvalidShaderId, nullptr);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.triangle.intersection.attributes" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetTriangleIntersectionAttributes(lgc::GpurtSetTriangleIntersectionAttributesOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createSetTriangleInsection(func);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.hit.triangle.node.pointer" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetHitTriangleNodePointer(lgc::GpurtSetHitTriangleNodePointerOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst);

  if (func->isDeclaration())
    createSetHitTriangleNodePointer(func);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.get.parent.id" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitGetParentId(lgc::GpurtGetParentIdOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto parentId = m_builder->CreateLoad(m_builder->getInt32Ty(), m_traceParams[TraceParam::ParentRayId]);
  inst.replaceAllUsesWith(parentId);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.parent.id" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetParentId(lgc::GpurtSetParentIdOp &inst) {
  m_builder->SetInsertPoint(&inst);

  m_builder->CreateStore(m_builder->CreateLoad(m_builder->getInt32Ty(), inst.getRayId()),
                         m_traceParams[TraceParam::ParentRayId]);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.dispatch.rays.index" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitDispatchRayIndex(lgc::rt::DispatchRaysIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto dispatchRayIndex = m_builder->CreateReadBuiltInInput(lgc::BuiltInGlobalInvocationId, {}, nullptr, nullptr, "");
  inst.replaceAllUsesWith(dispatchRayIndex);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Creates a implementation function of a call instruction, redirect the call to the new function, and return the
// function.
//
// @param inst : The instruction
llvm::Function *SpirvLowerRayTracing::createImplFunc(llvm::CallInst &inst) {
  std::string mangledName = inst.getCalledFunction()->getName().str() + ".impl";
  SmallVector<Value *, 10> implCallArgs(inst.args());
  auto newCall = m_builder->CreateNamedCall(mangledName, inst.getCalledFunction()->getReturnType(), implCallArgs,
                                            {Attribute::NoUnwind, Attribute::AlwaysInline});

  inst.replaceAllUsesWith(newCall);

  return m_module->getFunction(mangledName);
}

} // namespace Llpc
