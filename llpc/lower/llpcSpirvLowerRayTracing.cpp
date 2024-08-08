/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayTracing.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerRayTracing.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerRayTracing.h"
#include "SPIRVInternal.h"
#include "compilerutils/CompilerUtils.h"
#include "gpurt-compiler.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerUtil.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/Builder.h"
#include "lgc/CommonDefs.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/Pipeline.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "llpc-spirv-lower-ray-tracing"

namespace llvm {
namespace cl {
extern opt<bool> TrimDebugInfo;
} // namespace cl
} // namespace llvm

using namespace llvm;
using namespace Llpc;
using namespace lgc::rt;
using namespace CompilerUtils;

namespace SPIRV {
extern const char *MetaNameSpirvOp;
} // namespace SPIRV

namespace RtName {
const char *TraceRayKHR = "_cs_";
const char *TraceRaySetTraceParams = "TraceRaySetTraceParams";
const char *ShaderTable = "ShaderTable";
static const char *CallAnyHitShader = "AmdTraceRayCallAnyHitShader";
static const char *RemapCapturedVaToReplayVa = "AmdTraceRayRemapCapturedVaToReplayVa";
static const char *ContinufyStageMeta = "continufy.stage";
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
    9, // 17, HitTriangleVertexPositions
    1, // 18, Payload,
    1, // 19, RayStaticId
};

// =====================================================================================================================
SpirvLowerRayTracing::SpirvLowerRayTracing() : m_nextTraceRayId(0) {
}

// =====================================================================================================================
// Process a trace ray call by creating (or get if created) an implementation function and replace the call to it.
//
// @param inst : The original call instruction
void SpirvLowerRayTracing::processTraceRayCall(BaseTraceRayOp *inst) {
  m_builder->SetInsertPoint(inst);

  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  auto payloadTy = rayTracingContext->getPayloadType(m_builder);
  AllocaInst *localPayload = nullptr;
  {
    IRBuilderBase::InsertPointGuard ipg(*m_builder);
    m_builder->SetInsertPointPastAllocas(inst->getFunction());
    localPayload = m_builder->CreateAlloca(payloadTy, SPIRAS_Private);
  }

  // Setup arguments
  SmallVector<Value *> args;
  auto payloadArgSize = m_builder->CreateExtractValue(inst->getPaq(), 0);
  m_builder->CreateMemCpy(localPayload, localPayload->getAlign(), inst->getPayload(), Align(4), payloadArgSize);
  args.push_back(m_builder->CreateLoad(payloadTy, localPayload));
  args.push_back(m_builder->CreateBitCast(inst->getAccelStruct(), FixedVectorType::get(m_builder->getInt32Ty(), 2)));
  args.push_back(inst->getRayFlags());
  args.push_back(inst->getInstanceInclusionMask());
  args.push_back(inst->getRayContributionToHitGroupIndex());
  args.push_back(inst->getMultiplierForGeometryContribution());
  args.push_back(inst->getMissShaderIndex());
  args.push_back(inst->getOrigin());
  args.push_back(inst->getTMin());
  args.push_back(inst->getDirection());
  args.push_back(inst->getTMax());

  if (rayTracingContext->getRayTracingState()->enableRayTracingCounters) {
    args.push_back(m_builder->CreateLoad(m_builder->getInt32Ty(), m_traceParams[TraceParam::ParentRayId]));
    args.push_back(m_builder->getInt32(generateTraceRayStaticId()));
  }

  // Call the trace ray implementation
  if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
    createSqttCallCompactToken(ShaderStageCompute);

  Value *result = nullptr;
  bool indirect = rayTracingContext->getIndirectStageMask() & ShaderStageComputeBit;
  auto funcTy = getTraceRayFuncTy();
  if (indirect) {
    Value *traceRayGpuVa = loadShaderTableVariable(ShaderTable::TraceRayGpuVirtAddr, m_dispatchRaysInfoDesc);
    auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);
    auto funcPtr = m_builder->CreateIntToPtr(traceRayGpuVa, funcPtrTy);
    // Create the indirect function call
    CallInst *call = m_builder->CreateCall(funcTy, funcPtr, args);
    call->setCallingConv(CallingConv::SPIR_FUNC);

    unsigned lgcRtStage = ~0u;
    call->setMetadata(RtName::ContinufyStageMeta,
                      MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(lgcRtStage))));

    result = call;
  } else {
    result = m_builder->CreateNamedCall(RtName::TraceRayKHR, funcTy->getReturnType(), args, {Attribute::AlwaysInline});
  }

  if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
    createSqttFunctionReturnToken();

  // Handle the result
  unsigned payloadSizeInDword = rayTracingContext->getPayloadSizeInDword();
  unsigned index = 0;
  Value *payloadVal = PoisonValue::get(rayTracingContext->getPayloadType(m_builder));
  for (; index < payloadSizeInDword; index++)
    payloadVal = m_builder->CreateInsertValue(payloadVal, m_builder->CreateExtractValue(result, index), index);
  m_builder->CreateStore(payloadVal, localPayload);
  m_builder->CreateMemCpy(inst->getPayload(), Align(4), localPayload, localPayload->getAlign(), payloadArgSize);

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
      SmallVector<StringRef> argNames;
      auto funcTy = getCallableShaderEntryFuncTy(argNames);
      auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);
      if (rayTracingContext->isReplay()) {
        auto remapFunc = getOrCreateRemapCapturedVaToReplayVaFunc();
        shaderIdentifier = m_builder->CreateCall(remapFunc->getFunctionType(), remapFunc, shaderIdentifier);
      }
      auto funcPtr = m_builder->CreateIntToPtr(shaderIdentifier, funcPtrTy);

      if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
        createSqttCallCompactToken(ShaderStageRayTracingCallable);

      CallInst *result = m_builder->CreateCall(funcTy, funcPtr, args);

      if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
        createSqttFunctionReturnToken();

      result->setCallingConv(CallingConv::SPIR_FUNC);

      unsigned lgcRtStage = static_cast<unsigned>(mapStageToLgcRtShaderStage(ShaderStageRayTracingCallable));
      result->setMetadata(RtName::ContinufyStageMeta,
                          MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(lgcRtStage))));

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
  m_builder->SetInsertPoint(&inst);

  assert(m_shaderStage == ShaderStageRayTracingIntersect);

  Value *acceptedPtr = nullptr;
  {
    IRBuilderBase::InsertPointGuard ipg(*m_builder);
    m_builder->SetInsertPointPastAllocas(inst.getFunction());
    acceptedPtr = m_builder->CreateAlloca(m_builder->getInt1Ty(), SPIRAS_Private);
    m_builder->CreateStore(m_builder->getFalse(), acceptedPtr);
  }

  // Check whether candidate Thit is between Tmin and the current committed hit.
  Value *tMin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMin], m_traceParams[TraceParam::TMin]);
  Value *committedTCurrent =
      m_builder->CreateLoad(m_traceParamsTys[TraceParam::TCurrent], m_traceParams[TraceParam::TCurrent]);

  Value *shift = m_builder->CreateFSub(inst.getThit(), tMin);
  Value *shiftGeZero = m_builder->CreateFCmpOGE(shift, ConstantFP::get(m_builder->getFloatTy(), 0.0));
  Value *tCurrentGeShift = m_builder->CreateFCmpOGE(committedTCurrent, shift);
  Value *tmp = m_builder->CreateAnd(shiftGeZero, tCurrentGeShift);

  {
    Instruction *endThitAccept = SplitBlockAndInsertIfThen(tmp, m_builder->GetInsertPoint(), false);
    m_builder->SetInsertPoint(endThitAccept);

    // Backup the committed hit
    Value *committedTMax = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMax], m_traceParams[TraceParam::TMax]);
    Value *committedKind = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Kind], m_traceParams[TraceParam::Kind]);
    Value *committedStatus =
        m_builder->CreateLoad(m_traceParamsTys[TraceParam::Status], m_traceParams[TraceParam::Status]);

    // Tentatively commit the candidate hit
    m_builder->CreateStore(shift, m_traceParams[TraceParam::TCurrent]);
    m_builder->CreateStore(inst.getThit(), m_traceParams[TraceParam::TMax]);
    m_builder->CreateStore(inst.getHitKind(), m_traceParams[TraceParam::Kind]);
    m_builder->CreateStore(m_builder->getInt32(RayHitStatus::Accept), m_traceParams[TraceParam::Status]);

    // Call the anyhit shader if there is one; this updates trace params
    const static std::string ModuleNamePrefix =
        std::string("_") + getShaderStageAbbreviation(ShaderStageRayTracingIntersect) + "_";
    unsigned intersectId = 0;
    m_module->getName().substr(ModuleNamePrefix.size()).consumeInteger(0, intersectId);

    std::vector<unsigned> anyHitIds;
    auto context = static_cast<RayTracingContext *>(m_context->getPipelineContext());
    context->getStageModuleIds(ShaderStageRayTracingAnyHit, intersectId, anyHitIds);

    if (!anyHitIds.empty() || context->hasLibraryStage(shaderStageToMask(ShaderStageRayTracingAnyHit))) {
      auto shaderIdentifier =
          getShaderIdentifier(ShaderStageRayTracingAnyHit, m_shaderRecordIndex, m_dispatchRaysInfoDesc);

      SmallVector<Value *> args;
      args.push_back(shaderIdentifier);
      args.push_back(m_shaderRecordIndex);
      for (unsigned i = 0; i < TraceParam::Count; ++i)
        args.push_back(m_traceParams[i]);

      createAnyHitFunc(shaderIdentifier, m_shaderRecordIndex);
      m_builder->CreateNamedCall(RtName::CallAnyHitShader, m_builder->getVoidTy(), args,
                                 {Attribute::NoUnwind, Attribute::AlwaysInline});
    }

    // Check if the AHS accepted
    Value *status = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Status], m_traceParams[TraceParam::Status]);
    Value *accepted = m_builder->CreateICmpNE(status, m_builder->getInt32(RayHitStatus::Ignore));
    Value *endFromAhs = m_builder->CreateICmpEQ(status, m_builder->getInt32(RayHitStatus::AcceptAndEndSearch));
    tmp = m_builder->CreateLoad(m_traceParamsTys[TraceParam::RayFlags], m_traceParams[TraceParam::RayFlags]);
    tmp = m_builder->CreateAnd(tmp, m_builder->getInt32(RayFlag::AcceptFirstHitAndEndSearch));
    tmp = m_builder->CreateICmpNE(tmp, m_builder->getInt32(0));
    Value *endFromRayFlags = m_builder->CreateAnd(accepted, tmp);
    Value *endRay = m_builder->CreateOr(endFromAhs, endFromRayFlags);

    {
      // Accept the hit and end the ray for one reason or another. Immediately return from the IS.
      Instruction *endEndRay = SplitBlockAndInsertIfThen(endRay, m_builder->GetInsertPoint(), true);
      m_builder->SetInsertPoint(endEndRay);

      // Override the status because it may only be "Accept" if we return due to ray flags.
      m_builder->CreateStore(m_builder->getInt32(RayHitStatus::AcceptAndEndSearch), m_traceParams[TraceParam::Status]);
      m_builder->CreateRetVoid();
      endEndRay->eraseFromParent(); // erase `unreachable`
    }
    m_builder->SetInsertPoint(endThitAccept); // also reset the insert block

    // Restore the old committed hit if the candidate wasn't accepted
    Value *newTCurrent =
        m_builder->CreateLoad(m_traceParamsTys[TraceParam::TCurrent], m_traceParams[TraceParam::TCurrent]);
    Value *newTMax = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMax], m_traceParams[TraceParam::TMax]);
    Value *newKind = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Kind], m_traceParams[TraceParam::Kind]);
    Value *newStatus = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Status], m_traceParams[TraceParam::Status]);

    newTCurrent = m_builder->CreateSelect(accepted, newTCurrent, committedTCurrent);
    newTMax = m_builder->CreateSelect(accepted, newTMax, committedTMax);
    newKind = m_builder->CreateSelect(accepted, newKind, committedKind);
    newStatus = m_builder->CreateSelect(accepted, newStatus, committedStatus);

    m_builder->CreateStore(newTCurrent, m_traceParams[TraceParam::TCurrent]);
    m_builder->CreateStore(newTMax, m_traceParams[TraceParam::TMax]);
    m_builder->CreateStore(newKind, m_traceParams[TraceParam::Kind]);
    m_builder->CreateStore(newStatus, m_traceParams[TraceParam::Status]);

    m_builder->CreateStore(accepted, acceptedPtr);
  }
  m_builder->SetInsertPoint(&inst); // also reset the insert block

  inst.replaceAllUsesWith(m_builder->CreateLoad(m_builder->getInt1Ty(), acceptedPtr));
  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerRayTracing::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Ray-Tracing\n");

  SpirvLower::init(&module);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  memset(m_traceParams, 0, sizeof(m_traceParams));
  initTraceParamsTy(rayTracingContext->getAttributeDataSize());
  initShaderBuiltIns();
  Instruction *insertPos = nullptr;

  const auto *rtState = m_context->getPipelineContext()->getRayTracingState();
  lgc::ComputeShaderMode mode = {};
  mode.workgroupSizeX = rtState->threadGroupSizeX;
  mode.workgroupSizeY = rtState->threadGroupSizeY;
  mode.workgroupSizeZ = rtState->threadGroupSizeZ;
  lgc::Pipeline::setComputeShaderMode(module, mode);

  m_crossModuleInliner = std::make_optional<CrossModuleInliner>();
  auto onExit = make_scope_exit([&] { m_crossModuleInliner.reset(); });

  // Create empty raygen main module
  if (module.empty()) {
    m_shaderStage = ShaderStageRayTracingRayGen;
    createRayGenEntryFunc();
    rayTracingContext->setEntryName("main");
    return PreservedAnalyses::none();
  }

  if (m_shaderStage == ShaderStageRayTracingClosestHit || m_shaderStage == ShaderStageRayTracingAnyHit ||
      m_shaderStage == ShaderStageRayTracingIntersect || m_shaderStage == ShaderStageRayTracingMiss) {
    insertPos = createEntryFunc(m_entryPoint);
  } else if (m_shaderStage == ShaderStageRayTracingCallable) {
    insertPos = createCallableShaderEntryFunc(m_entryPoint);
  } else if (m_shaderStage == ShaderStageRayTracingRayGen) {
    createTraceParams(m_entryPoint);
    insertPos = &*(m_entryPoint->begin()->getFirstNonPHIOrDbgOrAlloca());
    m_shaderRecordIndex = m_builder->getInt32(0);
  }
  // Process traceRays module
  if (m_shaderStage == ShaderStageCompute) {
    CallInst *call = createTraceRay();
    inlineTraceRay(call, analysisManager);

    unsigned lgcRtStage = ~0u;
    m_entryPoint->setMetadata(RtName::ContinufyStageMeta,
                              MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(lgcRtStage))));

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
                              .add(&SpirvLowerRayTracing::visitGetRayStaticId)
                              .add(&SpirvLowerRayTracing::visitDispatchRayIndex)
                              .build();

    visitor.visit(*this, *m_entryPoint);
  } else { // Process ray tracing modules
    m_entryPoint->setName(module.getName());
    m_entryPoint->addFnAttr(Attribute::AlwaysInline);
    m_builder->SetInsertPoint(insertPos);
    createDispatchRaysInfoDesc();
    m_spirvOpMetaKindId = m_context->getMDKindID(MetaNameSpirvOp);

    unsigned lgcRtStage = static_cast<unsigned>(mapStageToLgcRtShaderStage(m_shaderStage));
    m_entryPoint->setMetadata(RtName::ContinufyStageMeta,
                              MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(lgcRtStage))));

    if (m_shaderStage == ShaderStageRayTracingAnyHit || m_shaderStage == ShaderStageRayTracingClosestHit ||
        m_shaderStage == ShaderStageRayTracingIntersect) {
      m_worldToObjMatrix = nullptr;
    }

    m_insertPosPastInit = insertPos;

    static auto visitor = llvm_dialects::VisitorBuilder<SpirvLowerRayTracing>()
                              .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                              .add(&SpirvLowerRayTracing::visitAcceptHitAndEndSearchOp)
                              .add(&SpirvLowerRayTracing::visitIgnoreHitOp)
                              .add(&SpirvLowerRayTracing::visitCallCallableShaderOp)
                              .add(&SpirvLowerRayTracing::visitReportHitOp)
                              .add(&SpirvLowerRayTracing::visitTraceRayOp)
                              .add(&SpirvLowerRayTracing::visitDispatchRayIndex)
                              .add(&SpirvLowerRayTracing::visitDispatchRaysDimensionsOp)
                              .add(&SpirvLowerRayTracing::visitWorldRayOriginOp)
                              .add(&SpirvLowerRayTracing::visitWorldRayDirectionOp)
                              .add(&SpirvLowerRayTracing::visitObjectRayOriginOp)
                              .add(&SpirvLowerRayTracing::visitObjectRayDirectionOp)
                              .add(&SpirvLowerRayTracing::visitRayTminOp)
                              .add(&SpirvLowerRayTracing::visitRayTcurrentOp)
                              .add(&SpirvLowerRayTracing::visitInstanceIndexOp)
                              .add(&SpirvLowerRayTracing::visitObjectToWorldOp)
                              .add(&SpirvLowerRayTracing::visitWorldToObjectOp)
                              .add(&SpirvLowerRayTracing::visitHitKindOp)
                              .add(&SpirvLowerRayTracing::visitTriangleVertexPositionsOp)
                              .add(&SpirvLowerRayTracing::visitRayFlagsOp)
                              .add(&SpirvLowerRayTracing::visitGeometryIndexOp)
                              .add(&SpirvLowerRayTracing::visitInstanceIdOp)
                              .add(&SpirvLowerRayTracing::visitPrimitiveIndexOp)
                              .add(&SpirvLowerRayTracing::visitInstanceInclusionMaskOp)
                              .add(&SpirvLowerRayTracing::visitShaderIndexOp)
                              .add(&SpirvLowerRayTracing::visitShaderRecordBufferOp)
                              .add(&SpirvLowerRayTracing::visitStackReadOp)
                              .add(&SpirvLowerRayTracing::visitStackWriteOp)
                              .add(&SpirvLowerRayTracing::visitLdsStackInitOp)
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

  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    if (!func->empty() && !func->getName().starts_with(module.getName()) &&
        ((func->getLinkage() == GlobalValue::ExternalLinkage) || (func->getLinkage() == GlobalValue::WeakAnyLinkage))) {
      // Newly generated implementation functions have external linkage, but should have internal linkage.
      // Weak-linkage functions are GpuRt functions that we just added calls to, and which are no longer required apart
      // from these calls, so assign internal linkage to them as well.
      // In both cases, these functions are removed after inlining.
      func->setLinkage(GlobalValue::InternalLinkage);
    }
  }

  LLVM_DEBUG(dbgs() << "After the pass Spirv-Lower-Ray-Tracing " << module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Create alloc variable for the TraceParam
void SpirvLowerRayTracing::createTraceParams(Function *entryFunc) {
  m_builder->SetInsertPointPastAllocas(entryFunc);
  for (unsigned i = 0; i < TraceParam::Count; ++i) {
    m_traceParams[i] =
        m_builder->CreateAlloca(m_traceParamsTys[i], SPIRAS_Private, nullptr, Twine("local.") + m_traceParamNames[i]);
  }
}

// =====================================================================================================================
// Create function to set HitAttributes
//
// @param func : Function to create
// @param instArgsNum : Dialect instruction num
// @param traceParamsOffset : TraceParams Offset
void SpirvLowerRayTracing::createSetHitAttributes(Function *func, unsigned instArgsNum, unsigned traceParamsOffset) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  auto argIt = func->arg_begin();
  auto traceParams = argIt + instArgsNum - traceParamsOffset;
  assert(instArgsNum == (TraceParam::GeometryIndex - TraceParam::TCurrent + 1));

  for (unsigned i = 0; i < instArgsNum; ++i) {
    Value *storeValue = argIt + i;
    Value *StorePos = traceParams + i + TraceParam::TCurrent;
    m_builder->CreateStore(storeValue, StorePos);
  }

  Value *tCurrent = argIt;
  Value *tMin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMin], traceParams + TraceParam::TMin);
  Value *tMaxValue = m_builder->CreateFAdd(tCurrent, tMin);
  Value *tMax = traceParams + TraceParam::TMax;
  m_builder->CreateStore(tMaxValue, tMax);

  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Create function to set TraceParams
//
// @param func : Function to create
// @param instArgsNum : Dialect inst arguments count
void SpirvLowerRayTracing::createSetTraceParams(Function *func, unsigned instArgsNum) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 12
  assert(instArgsNum == 9);
#else
  assert(instArgsNum == 8);
#endif

  auto argIt = func->arg_begin();
  auto traceParams = argIt + instArgsNum;

  Value *rayFlags = argIt++;
  m_builder->CreateStore(rayFlags, traceParams + TraceParam::RayFlags);

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 12
  Value *instanceInclusionMask = argIt++;
  m_builder->CreateStore(instanceInclusionMask, traceParams + TraceParam::InstanceInclusionMask);
#endif

  Value *originX = argIt++;
  Value *originY = argIt++;
  Value *originZ = argIt++;
  Value *origin = PoisonValue::get(FixedVectorType::get(Type::getFloatTy(*m_context), 3));
  origin = m_builder->CreateInsertElement(origin, originX, uint64_t(0));
  origin = m_builder->CreateInsertElement(origin, originY, 1);
  origin = m_builder->CreateInsertElement(origin, originZ, 2);
  m_builder->CreateStore(origin, traceParams + TraceParam::Origin);

  Value *tMin = argIt++;
  m_builder->CreateStore(tMin, traceParams + TraceParam::TMin);

  Value *dirX = argIt++;
  Value *dirY = argIt++;
  Value *dirZ = argIt++;
  Value *dir = PoisonValue::get(FixedVectorType::get(Type::getFloatTy(*m_context), 3));
  dir = m_builder->CreateInsertElement(dir, dirX, uint64_t(0));
  dir = m_builder->CreateInsertElement(dir, dirY, 1);
  dir = m_builder->CreateInsertElement(dir, dirZ, 2);
  m_builder->CreateStore(dir, traceParams + TraceParam::Dir);

  Value *zero = ConstantFP::get(m_builder->getFloatTy(), 0.0);
  m_builder->CreateStore(zero, traceParams + TraceParam::TMax);

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
// @param traceParamsArgOffset : Non TraceParam arguments number
void SpirvLowerRayTracing::createCallShaderFunc(Function *func, ShaderStage stage, unsigned intersectId, Value *retVal,
                                                unsigned traceParamsArgOffset) {
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
    updateGlobalFromCallShaderFunc(func, stage, traceParamsArgOffset);
    // Table Index is second parameter for non-intersect shader and third for intersect shader
    Value *tableIndexValue =
        stage != ShaderStageRayTracingIntersect ? (func->arg_begin() + 1) : (func->arg_begin() + 2);

    Value *shaderId = func->arg_begin();
    shaderId = m_builder->CreateBitCast(shaderId, m_builder->getInt64Ty());
    createCallShader(func, stage, intersectId, shaderId, tableIndexValue, inputResult, entryBlock, endBlock,
                     traceParamsArgOffset);
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
// @param traceParamsArgOffset : The count of beginning function non traceParam function arguments
void SpirvLowerRayTracing::createCallShader(Function *func, ShaderStage stage, unsigned intersectId, Value *shaderId,
                                            Value *shaderRecordIndex, Value *inputResult, BasicBlock *entryBlock,
                                            BasicBlock *endBlock, unsigned traceParamsArgOffset) {
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

  Function::arg_iterator traceParamsIt = func->arg_begin() + traceParamsArgOffset;

  SmallVector<Value *, 8> args;

  Value *traceParams[TraceParam::Count] = {};

  // Assemble the arguments from builtIns
  for (auto builtIn : m_builtInParams) {
    traceParams[builtIn] = m_builder->CreateLoad(m_traceParamsTys[builtIn], traceParamsIt + builtIn);
    args.push_back(traceParams[builtIn]);
  }

  // Assemble the extra arguments for specific shader stage
  for (auto param : getShaderExtraInputParams(stage)) {
    traceParams[param] = m_builder->CreateLoad(m_traceParamsTys[param], traceParamsIt + param);
    args.push_back(traceParams[param]);
  }

  args.push_back(shaderRecordIndex);

  auto payload = traceParams[TraceParam::Payload];

  if (indirectShader) {
    SmallVector<StringRef> argNames;
    auto funcTy = getShaderEntryFuncTy(stage, argNames);
    auto funcPtrTy = PointerType::get(funcTy, SPIRAS_Generic);

    if (rayTracingContext->isReplay()) {
      auto remapFunc = getOrCreateRemapCapturedVaToReplayVaFunc();
      shaderId = m_builder->CreateCall(remapFunc->getFunctionType(), remapFunc, shaderId);
    }

    auto funcPtr = m_builder->CreateIntToPtr(shaderId, funcPtrTy);

    if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
      createSqttCallCompactToken(stage);

    CallInst *result = m_builder->CreateCall(funcTy, funcPtr, args);

    if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
      createSqttFunctionReturnToken();

    unsigned lgcRtStage = static_cast<unsigned>(mapStageToLgcRtShaderStage(stage));
    result->setMetadata(RtName::ContinufyStageMeta,
                        MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(lgcRtStage))));

    result->setCallingConv(CallingConv::SPIR_FUNC);
    storeFunctionCallResult(stage, result, traceParamsIt);
    m_builder->CreateBr(endBlock);
    m_builder->SetInsertPoint(endBlock);
  } else {
    initInputResult(stage, payload, traceParams, inputResult, traceParamsIt);
    shaderId = m_builder->CreateTrunc(shaderId, m_builder->getInt32Ty());
    Type *inputResultTy = getShaderReturnTy(stage);
    createShaderSelection(func, entryBlock, endBlock, shaderId, intersectId, stage, args, inputResult, inputResultTy);
    m_builder->SetInsertPoint(endBlock);
    inputResult = m_builder->CreateLoad(inputResultTy, inputResult);
    storeFunctionCallResult(stage, inputResult, traceParamsIt);
  }
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
  Value *traceHitAttributes = func->arg_begin() + 1;
  auto zero = m_builder->getInt32(0);
  auto one = m_builder->getInt32(1);

  Value *attribValue0 = m_builder->CreateExtractElement(barycentrics, uint64_t(0));
  Type *attribHitEltTy = m_traceParamsTys[TraceParam::HitAttributes];
  Value *attribDestPtr = m_builder->CreateGEP(attribHitEltTy, traceHitAttributes, {zero, zero});
  m_builder->CreateStore(attribValue0, attribDestPtr);

  Value *attribValue1 = m_builder->CreateExtractElement(barycentrics, 1);
  attribDestPtr = m_builder->CreateGEP(attribHitEltTy, traceHitAttributes, {zero, one});
  m_builder->CreateStore(attribValue1, attribDestPtr);
  m_builder->CreateRetVoid();
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
                                                 ArrayRef<Value *> args, Value *inResult, Type *inResultTy) {
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

    auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
    if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
      createSqttCallCompactToken(stage);

    Value *result =
        m_builder->CreateNamedCall(funcName, inResultTy, args, {Attribute::NoUnwind, Attribute::AlwaysInline});

    if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
      createSqttFunctionReturnToken();

    if (inResult)
      m_builder->CreateStore(result, inResult);

    BranchInst::Create(endBlock, shaderBlock);
  }
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
  Type *gpuAddrAsPtrTy = PointerType::get(*m_context, SPIRAS_Global);
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
  IRBuilderBase::InsertPointGuard ipg(*m_builder);
  Function *func = dyn_cast_or_null<Function>(m_module->getFunction(RtName::CallAnyHitShader));
  if (!func) {
    SmallVector<Type *> tys = {shaderIdentifier->getType(), shaderRecordIndex->getType()};
    for (unsigned i = 0; i < TraceParam::Count; ++i)
      tys.push_back(m_builder->getPtrTy(SPIRAS_Private));

    auto funcTy = FunctionType::get(m_builder->getVoidTy(), tys, false);
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
    Function::arg_iterator traceParams = func->arg_begin() + 2;
    Value *inputResult = m_builder->CreateAlloca(getShaderReturnTy(ShaderStageRayTracingAnyHit), SPIRAS_Private);
    Value *anyHitCallATypeAddr = traceParams + TraceParam::DuplicateAnyHit;
    Value *anyHitCallType = m_builder->CreateLoad(m_traceParamsTys[TraceParam::DuplicateAnyHit], anyHitCallATypeAddr);
    Value *checkCallType = m_builder->CreateICmpEQ(anyHitCallType, zero);
    m_builder->CreateCondBr(checkCallType, endBlock, shaderBlock);

    m_builder->SetInsertPoint(shaderBlock);
    createCallShader(func, ShaderStageRayTracingAnyHit, RayTracingContext::InvalidShaderId, shaderId, tableIndex,
                     inputResult, shaderBlock, duplicateBlock, 2);

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

  lgc::Pipeline::markShaderEntryPoint(func, lgc::ShaderStage::Compute);

  // Construct entry block guard the launchId from launchSize
  m_builder->SetInsertPoint(entryBlock);
  createDispatchRaysInfoDesc();
  Value *launchSize = loadShaderTableVariable(ShaderTable::LaunchSize, m_dispatchRaysInfoDesc);
  auto builtIn = lgc::BuiltInGlobalInvocationId;
  auto launchlId = m_builder->CreateReadBuiltInInput(builtIn);
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

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION > 68
  if (rayTracingContext->getRaytracingMode() == Vkgc::LlpcRaytracingMode::Continufy &&
      rayTracingContext->getIndirectStageMask() != 0) {
    // Setup continuation stack pointer
    auto offset = offsetof(GpuRt::DispatchRaysConstantData, cpsBackendStackSize);
    auto gep = m_builder->CreateConstGEP1_32(m_builder->getInt8Ty(), m_dispatchRaysInfoDesc, offset);
    Value *stackPtr = m_builder->CreateLoad(m_builder->getInt32Ty(), gep);
    stackPtr = m_builder->CreateIntToPtr(stackPtr, PointerType::get(*m_context, lgc::cps::stackAddrSpace));
    m_builder->create<lgc::cps::SetVspOp>(stackPtr);
  }
#endif

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

    if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
      createSqttCallCompactToken(ShaderStageRayTracingRayGen);

    CallInst *call = m_builder->CreateCall(funcTy, funcPtr, {});

    if (rayTracingContext->getRayTracingState()->exportConfig.emitRaytracingShaderDataToken)
      createSqttFunctionReturnToken();
    call->setCallingConv(CallingConv::SPIR_FUNC);

    unsigned lgcRtStage = static_cast<unsigned>(mapStageToLgcRtShaderStage(ShaderStageRayTracingRayGen));
    call->setMetadata(RtName::ContinufyStageMeta,
                      MDNode::get(*m_context, ConstantAsMetadata::get(m_builder->getInt32(lgcRtStage))));

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
// Create traceray module entry function
CallInst *SpirvLowerRayTracing::createTraceRay() {
  assert(m_shaderStage == ShaderStageCompute);

  // Create traceRay module entry function
  StringRef traceEntryFuncName = m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_TRACE_RAY);
  Function *traceRayFunc = m_module->getFunction(traceEntryFuncName);
  assert(traceRayFunc != nullptr);

  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  bool indirect = rayTracingContext->getIndirectStageMask() & ShaderStageComputeBit;

  auto funcTy = getTraceRayFuncTy();
  StringRef funcName = RtName::TraceRayKHR;
  Function *func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, m_module);
  func->setCallingConv(CallingConv::SPIR_FUNC);
  if (!indirect)
    func->addFnAttr(Attribute::AlwaysInline);

  func->addFnAttr(Attribute::NoUnwind);
  m_entryPoint = func;

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

  createTraceParams(func);

  auto argIt = func->arg_begin();
  // Payload
  Value *arg = argIt++;
  m_builder->CreateStore(arg, m_traceParams[TraceParam::Payload]);

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

  // Parent ray ID and static ID for logging feature
  if (m_context->getPipelineContext()->getRayTracingState()->enableRayTracingCounters) {
    arg = ++argIt;
    m_builder->CreateStore(arg, m_traceParams[TraceParam::ParentRayId]);
    arg = ++argIt;
    m_builder->CreateStore(arg, m_traceParams[TraceParam::RayStaticId]);
  }

  // Call TraceRay function from traceRays module
  auto call = m_builder->CreateCall(traceRayFunc, traceRaysArgs);

  const auto payloadType = rayTracingContext->getPayloadType(m_builder);
  (void(call)); // unused
  m_builder->CreateRet(m_builder->CreateLoad(payloadType, m_traceParams[TraceParam::Payload]));

  return call;
}

// =====================================================================================================================
// Inline traceray entry function in the _cs_ function
//
// @param callInst : Where to inline function
// @param analysisManager : : Analysis manager to use for this transformation
void SpirvLowerRayTracing::inlineTraceRay(llvm::CallInst *callInst, ModuleAnalysisManager &analysisManager) {
  FunctionAnalysisManager &fam = analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(*m_module).getManager();
  auto getAssumptionCache = [&](Function &F) -> AssumptionCache & { return fam.getResult<AssumptionAnalysis>(F); };
  auto getBFI = [&](Function &F) -> BlockFrequencyInfo & { return fam.getResult<BlockFrequencyAnalysis>(F); };
  auto getAAR = [&](Function &F) -> AAResults & { return fam.getResult<AAManager>(F); };
  auto &psi = analysisManager.getResult<ProfileSummaryAnalysis>(*m_module);
  auto calleeFunc = callInst->getCalledFunction();
  auto callingFunc = callInst->getCaller();
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 489715
  // Check if conversion to NewDbgFormat is needed for calleeFunc.
  // If we are inside PassManger then m_module and all its Functions and BB may be converted (depending if feature is
  // turned on) to new Debug Info format. Since calleeFunc is a new function which will be added/inlined into m_module,
  // we have to convert these function to new Debug Info format.
  //
  // Since calleeFunc will be removed after inline then there is no need to convert it back to old DbgInfoFormat after
  // InlineFunction.
  bool shouldConvert = m_module->IsNewDbgInfoFormat && !calleeFunc->IsNewDbgInfoFormat;
  if (shouldConvert)
    calleeFunc->convertToNewDbgValues();
#endif
  InlineFunctionInfo IFI(getAssumptionCache, &psi, &getBFI(*callingFunc), &getBFI(*calleeFunc));
  InlineResult res = InlineFunction(*callInst, IFI, /*MergeAttributes=*/true, &getAAR(*calleeFunc), true);
  (void(res)); // unused
  assert(res.isSuccess());
  calleeFunc->dropAllReferences();
  calleeFunc->eraseFromParent();
}

// =====================================================================================================================
// init TraceParam types
//
// @param traceParam : trace params
void SpirvLowerRayTracing::initTraceParamsTy(unsigned attributeSize) {
  auto floatx3Ty = FixedVectorType::get(Type::getFloatTy(*m_context), 3);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  const auto payloadType = rayTracingContext->getPayloadType(m_builder);
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
      StructType::get(*m_context, {floatx3Ty, floatx3Ty, floatx3Ty}), // 17, HitTriangleVertexPositions
      payloadType,                                                    // 18, Payload
      m_builder->getInt32Ty(),                                        // 19, rayStaticId
  };
  TraceParamsTySize[TraceParam::HitAttributes] = attributeSize;
  TraceParamsTySize[TraceParam::Payload] = payloadType->getArrayNumElements();
  assert(sizeof(TraceParamsTySize) / sizeof(TraceParamsTySize[0]) == TraceParam::Count);

  m_traceParamNames[TraceParam::RayFlags] = "RayFlags";
  m_traceParamNames[TraceParam::InstanceInclusionMask] = "InstanceInclusionMask";
  m_traceParamNames[TraceParam::Origin] = "Origin";
  m_traceParamNames[TraceParam::TMin] = "TMin";
  m_traceParamNames[TraceParam::Dir] = "Dir";
  m_traceParamNames[TraceParam::TMax] = "TMax";
  m_traceParamNames[TraceParam::TCurrent] = "TCurrent";
  m_traceParamNames[TraceParam::Kind] = "Kind";
  m_traceParamNames[TraceParam::Status] = "Status";
  m_traceParamNames[TraceParam::InstNodeAddrLo] = "InstNodeAddrLo";
  m_traceParamNames[TraceParam::InstNodeAddrHi] = "InstNodeAddrHi";
  m_traceParamNames[TraceParam::PrimitiveIndex] = "PrimitiveIndex";
  m_traceParamNames[TraceParam::DuplicateAnyHit] = "DuplicateAnyHit";
  m_traceParamNames[TraceParam::GeometryIndex] = "GeometryIndex";
  m_traceParamNames[TraceParam::HitAttributes] = "HitAttributes";
  m_traceParamNames[TraceParam::ParentRayId] = "ParentRayId";
  m_traceParamNames[TraceParam::HitTriangleVertexPositions] = "HitTriangleVertexPositions";
  m_traceParamNames[TraceParam::Payload] = "Payload";
  m_traceParamNames[TraceParam::RayStaticId] = "RayStaticId";
}

// =====================================================================================================================
// Initialize builting for shader call
void SpirvLowerRayTracing::initShaderBuiltIns() {
  assert(m_builtInParams.size() == 0);
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  const auto *buildInfo = rayTracingContext->getRayTracingPipelineBuildInfo();

  if (buildInfo->libraryMode != Vkgc::LibraryMode::Pipeline || buildInfo->libraryCount != 0) {
    // We're using a library or compiling to be used as a library. When shaders are compiled for library use, we
    // cannot know the full set of required builtins for the shaders that are compiled first (that may already have
    // been compiled at this time!), so we need to define a stable function signature by assuming that *all* builtins
    // are used.
    //
    // Note: The build of traversal could still be optimized in some regards when libraryMode is Pipeline.
    m_builtInParams.insert(TraceParam::PrimitiveIndex);
    m_builtInParams.insert(TraceParam::Kind);
    m_builtInParams.insert(TraceParam::RayFlags);
    m_builtInParams.insert(TraceParam::InstNodeAddrLo);
    m_builtInParams.insert(TraceParam::InstNodeAddrHi);
    m_builtInParams.insert(TraceParam::TMin);
    m_builtInParams.insert(TraceParam::Origin);
    m_builtInParams.insert(TraceParam::Dir);
    m_builtInParams.insert(TraceParam::GeometryIndex);
    m_builtInParams.insert(TraceParam::TMax);
    m_builtInParams.insert(TraceParam::InstanceInclusionMask);
    m_builtInParams.insert(TraceParam::HitTriangleVertexPositions);
    m_builtInParams.insert(TraceParam::HitAttributes);
    return;
  }

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
// @param argNames : Filled with the names of arguments
FunctionType *SpirvLowerRayTracing::getShaderEntryFuncTy(ShaderStage stage, SmallVectorImpl<StringRef> &argNames) {
  SmallVector<Type *, 8> argTys;

  auto retTy = getShaderReturnTy(stage);

  for (auto &builtIn : m_builtInParams) {
    argTys.push_back(m_traceParamsTys[builtIn]);
    argNames.push_back(m_traceParamNames[builtIn]);
  }

  for (auto &param : getShaderExtraInputParams(stage)) {
    argTys.push_back(m_traceParamsTys[param]);
    argNames.push_back(m_traceParamNames[param]);
  }

  argTys.push_back(m_builder->getInt32Ty());
  argNames.push_back("shaderRecordIndex");

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
  SmallVector<StringRef> argNames;
  auto newFuncTy = getShaderEntryFuncTy(m_shaderStage, argNames);
  Function *newFunc = Function::Create(newFuncTy, GlobalValue::ExternalLinkage, m_module->getName(), m_module);
  newFunc->setCallingConv(CallingConv::SPIR_FUNC);

  for (auto [i, argName] : enumerate(argNames))
    newFunc->getArg(i)->setName(argName);

  createTraceParams(func);
  func->getArg(0)->replaceAllUsesWith(m_traceParams[TraceParam::Payload]);
  setShaderPaq(newFunc, getShaderPaq(func));
  if (m_shaderStage != ShaderStageRayTracingMiss) {
    assert((m_shaderStage == ShaderStageRayTracingIntersect) || (m_shaderStage == ShaderStageRayTracingAnyHit) ||
           (m_shaderStage == ShaderStageRayTracingClosestHit));
    func->getArg(1)->replaceAllUsesWith(m_traceParams[TraceParam::HitAttributes]);
    setShaderHitAttributeSize(newFunc, getShaderHitAttributeSize(func).value_or(0));
  }

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
  setLgcRtShaderStage(m_entryPoint, getLgcRtShaderStage(m_shaderStage));

  Instruction *insertPos = &*(newFunc->begin()->getFirstNonPHIOrDbgOrAlloca());
  m_builder->SetInsertPoint(insertPos);
  auto argIt = newFunc->arg_begin();

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
// @param traceParamsArgOffset : Non traceParam arguments number
void SpirvLowerRayTracing::updateGlobalFromCallShaderFunc(Function *func, ShaderStage stage,
                                                          unsigned traceParamsArgOffset) {
  auto zero = m_builder->getInt32(0);
  auto one = m_builder->getInt32(1);

  if (stage == ShaderStageRayTracingAnyHit) {
    // Third function parameter attribute
    Value *attrib = func->arg_begin() + 2;
    Value *hitAttributes = func->arg_begin() + traceParamsArgOffset + TraceParam::HitAttributes;

    Value *attribValue0 = m_builder->CreateExtractElement(attrib, uint64_t(0));
    Type *hitAttribEltTy = m_traceParamsTys[TraceParam::HitAttributes];
    Value *attribDestPtr = m_builder->CreateGEP(hitAttribEltTy, hitAttributes, {zero, zero});
    m_builder->CreateStore(attribValue0, attribDestPtr);

    Value *attribValue1 = m_builder->CreateExtractElement(attrib, 1);
    attribDestPtr = m_builder->CreateGEP(hitAttribEltTy, hitAttributes, {zero, one});
    m_builder->CreateStore(attribValue1, attribDestPtr);
  }
}

// =====================================================================================================================
// Get callabe shader entry function type
FunctionType *SpirvLowerRayTracing::getCallableShaderEntryFuncTy(SmallVectorImpl<StringRef> &argNames) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  SmallVector<Type *, 8> argTys;
  auto callableDataTy = rayTracingContext->getCallableDataType(m_builder);
  argTys.push_back(callableDataTy);
  argNames.push_back("CallableData");

  argTys.push_back(m_builder->getInt32Ty());
  argNames.push_back("ShaderRecordIndex");

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

  // Add parent ray ID and static ID for logging feature.
  if (m_context->getPipelineContext()->getRayTracingState()->enableRayTracingCounters) {
    argsTys.push_back(m_builder->getInt32Ty()); // Parent Id
    argsTys.push_back(m_builder->getInt32Ty()); // Ray Static Id
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
  SmallVector<StringRef> argNames;
  auto newFuncTy = getCallableShaderEntryFuncTy(argNames);
  Function *newFunc = Function::Create(newFuncTy, GlobalValue::ExternalLinkage, m_module->getName(), m_module);
  newFunc->setCallingConv(CallingConv::C);

  for (auto [idx, argName] : enumerate(argNames))
    newFunc->getArg(idx)->setName(argName);

  m_builder->SetInsertPointPastAllocas(func);
  m_callableData = m_builder->CreateAlloca(newFunc->getReturnType());
  func->getArg(0)->replaceAllUsesWith(m_callableData);
  setShaderArgSize(newFunc, getShaderArgSize(func));

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
  Value *callableData = argIt++;
  m_builder->CreateStore(callableData, m_callableData);

  // Save the shader record index
  m_shaderRecordIndex = argIt;

  return insertPos;
}

// =====================================================================================================================
// Get all the function ReturnInst
//
// @param func : Function to gather ReturnInst
SmallVector<Instruction *> SpirvLowerRayTracing::getFuncRets(Function *func) const {
  SmallVector<Instruction *> rets;
  for (auto &block : *func) {
    auto blockTerm = block.getTerminator();
    if (blockTerm != nullptr && isa<ReturnInst>(blockTerm))
      rets.push_back(blockTerm);
  }
  return rets;
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
    params.insert(TraceParam::RayFlags);
    break;
  default:
    break;
  }

  // Always need payload
  params.insert(TraceParam::Payload);

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
void SpirvLowerRayTracing::storeFunctionCallResult(ShaderStage stage, Value *result, Argument *traceParamsIt) {
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());

  unsigned payloadSizeInDword = rayTracingContext->getPayloadSizeInDword();

  const auto &rets = getShaderExtraRets(stage);
  if (!rets.size()) {
    // No extra return value, only return payload
    m_builder->CreateStore(result, traceParamsIt + TraceParam::Payload);
  } else {
    // Return extra values
    Value *payloadVal = PoisonValue::get(rayTracingContext->getPayloadType(m_builder));
    unsigned index = 0;

    // Store payload first
    for (; index < payloadSizeInDword; index++)
      payloadVal = m_builder->CreateInsertValue(payloadVal, m_builder->CreateExtractValue(result, index), index);
    m_builder->CreateStore(payloadVal, traceParamsIt + TraceParam::Payload);

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

      m_builder->CreateStore(retVal, traceParamsIt + ret);
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
// @param traceParams : TraceParam argument
void SpirvLowerRayTracing::initInputResult(ShaderStage stage, Value *payload, Value *traceParams[], Value *result,
                                           Argument *traceParamsIt) {
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
      Value *param = traceParams[ret] == nullptr ? m_builder->CreateLoad(m_traceParamsTys[ret], traceParamsIt + ret)
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
Value *SpirvLowerRayTracing::createLoadRayTracingMatrix(unsigned builtInId) {
  assert(builtInId == BuiltInWorldToObjectKHR || builtInId == BuiltInObjectToWorldKHR);

  IRBuilderBase::InsertPointGuard guard(*m_builder);

  m_builder->SetInsertPoint(m_insertPosPastInit);

  // Get matrix address from instance node address
  Value *instNodeAddr = createLoadInstNodeAddr();

  return createLoadMatrixFromFunc(instNodeAddr, builtInId);
}

// =====================================================================================================================
// Process AmdTraceRaySetHitTriangleNodePointer function
//
// @param func : The function to create
void SpirvLowerRayTracing::createSetHitTriangleNodePointer(Function *func) {
  eraseFunctionBlocks(func);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);
  // Cross module inliner cannot be used to inline a function with multiple blocks into in a degenerate block, create
  // the terminator first.
  m_builder->SetInsertPoint(m_builder->CreateRetVoid());
  if (m_builtInParams.find(TraceParam::HitTriangleVertexPositions) != m_builtInParams.end()) {
    Value *bvh = func->arg_begin();
    Value *nodePtr = func->arg_begin() + 1;
    Value *vertexPos = func->arg_begin() + 2;

    Value *bvhPtr = m_builder->CreateAlloca(bvh->getType());
    Value *nodePtrPtr = m_builder->CreateAlloca(nodePtr->getType());

    m_builder->CreateStore(bvh, bvhPtr);
    m_builder->CreateStore(nodePtr, nodePtrPtr);

    auto triangleData = m_crossModuleInliner.value()
                            .inlineCall(*m_builder,
                                        getGpurtFunction(m_context->getPipelineContext()->getRayTracingFunctionName(
                                            Vkgc::RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_NODE_POINTER)),
                                        {bvhPtr, nodePtrPtr})
                            .returnValue;
    m_builder->CreateStore(triangleData, vertexPos);
  }
}

// =====================================================================================================================
// Process entry function return instruction, replace new return payload/etc info
//
// @param func : The function to process
void SpirvLowerRayTracing::createEntryTerminator(Function *func) {
  // Return incoming payload, and other values if needed
  auto rayTracingContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
  for (auto ret : getFuncRets(func)) {
    m_builder->SetInsertPoint(ret);
    const auto payloadType = rayTracingContext->getPayloadType(m_builder);
    Value *retVal = m_builder->CreateLoad(payloadType, m_traceParams[TraceParam::Payload]);

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
  for (auto ret : getFuncRets(func)) {
    m_builder->SetInsertPoint(ret);
    Instruction *newfuncEnd =
        m_builder->CreateRet(m_builder->CreateLoad(m_callableData->getAllocatedType(), m_callableData));
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

    auto bufferDesc = m_builder->create<lgc::LoadBufferDescOp>(Vkgc::InternalDescriptorSetId,
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
void SpirvLowerRayTracing::createDispatchRaysInfoDesc() {
  if (!m_dispatchRaysInfoDesc) {
    m_dispatchRaysInfoDesc = m_builder->create<lgc::LoadBufferDescOp>(
        TraceRayDescriptorSet, RayTracingResourceIndexDispatchRaysInfo, m_builder->getInt32(0), 0);
    m_builder->CreateInvariantStart(m_dispatchRaysInfoDesc);
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

  ArrayRef<Value *> args(&m_traceParams[TraceParam::TMin], TraceParam::GeometryIndex - TraceParam::TMin + 1);
  auto func = createImplFunc(inst, args);

  if (func->isDeclaration())
    createSetHitAttributes(func, inst.arg_size(), TraceParam::TMin);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.trace.params" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetTraceParams(lgc::GpurtSetTraceParamsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  ArrayRef<Value *> args(m_traceParams, TraceParam::TMax + 1);
  auto func = createImplFunc(inst, args);

  if (func->isDeclaration())
    createSetTraceParams(func, inst.arg_size());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.closest.hit.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallClosestHitShader(lgc::GpurtCallClosestHitShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);
  ArrayRef<Value *> args(m_traceParams, TraceParam::Count);

  auto func = createImplFunc(inst, args);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingClosestHit, RayTracingContext::InvalidShaderId,
                         m_builder->getTrue(), inst.arg_size());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.miss.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallMissShader(lgc::GpurtCallMissShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);
  ArrayRef<Value *> args(m_traceParams, TraceParam::Count);
  auto func = createImplFunc(inst, args);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingMiss, RayTracingContext::InvalidShaderId, m_builder->getTrue(),
                         inst.arg_size());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.triangle.any.hit.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallTriangleAnyHitShader(lgc::GpurtCallTriangleAnyHitShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);
  ArrayRef<Value *> args(m_traceParams, TraceParam::Count);
  auto func = createImplFunc(inst, args);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingAnyHit, RayTracingContext::TriangleHitGroup, nullptr,
                         inst.arg_size());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.call.intersection.shader" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitCallIntersectionShader(lgc::GpurtCallIntersectionShaderOp &inst) {
  m_builder->SetInsertPoint(&inst);
  ArrayRef<Value *> args(m_traceParams, TraceParam::Count);
  auto func = createImplFunc(inst, args);

  if (func->isDeclaration())
    createCallShaderFunc(func, ShaderStageRayTracingIntersect, RayTracingContext::InvalidShaderId, nullptr,
                         inst.arg_size());

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.set.triangle.intersection.attributes" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitSetTriangleIntersectionAttributes(lgc::GpurtSetTriangleIntersectionAttributesOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto func = createImplFunc(inst, {m_traceParams[TraceParam::HitAttributes]});

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

  auto func = createImplFunc(inst, {m_traceParams[TraceParam::HitTriangleVertexPositions]});

  if (func->isDeclaration())
    createSetHitTriangleNodePointer(func);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.get.ray.static.id" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitGetRayStaticId(lgc::GpurtGetRayStaticIdOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto rayStaticId = m_builder->CreateLoad(m_builder->getInt32Ty(), m_traceParams[TraceParam::RayStaticId]);
  inst.replaceAllUsesWith(rayStaticId);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.gpurt.stack.read" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitStackReadOp(lgc::GpurtStackReadOp &inst) {
  // NOTE: If RayQuery is used inside intersection or any-hit shaders, where we already holding a traversal stack for
  // TraceRay, perform the stack operations for this RayQuery in an extra stack space.
  if ((m_shaderStage == ShaderStageRayTracingIntersect) || (m_shaderStage == ShaderStageRayTracingAnyHit))
    inst.setUseExtraStack(true);
}

// =====================================================================================================================
// Visits "lgc.gpurt.stack.write" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitStackWriteOp(lgc::GpurtStackWriteOp &inst) {
  // NOTE: If RayQuery is used inside intersection or any-hit shaders, where we already holding a traversal stack for
  // TraceRay, perform the stack operations for this RayQuery in an extra stack space.
  if ((m_shaderStage == ShaderStageRayTracingIntersect) || (m_shaderStage == ShaderStageRayTracingAnyHit))
    inst.setUseExtraStack(true);
}

// =====================================================================================================================
// Visits "lgc.gpurt.stack.init" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitLdsStackInitOp(lgc::GpurtLdsStackInitOp &inst) {
  // NOTE: If RayQuery is used inside any-hit shaders, where we already holding a traversal stack for
  // TraceRay, perform the stack operations for this RayQuery in an extra stack space.
  if (m_shaderStage == ShaderStageRayTracingAnyHit)
    inst.setUseExtraStack(true);
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

  m_builder->CreateStore(inst.getRayId(), m_traceParams[TraceParam::ParentRayId]);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.dispatch.rays.index" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitDispatchRayIndex(lgc::rt::DispatchRaysIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto dispatchRayIndex = m_builder->CreateReadBuiltInInput(lgc::BuiltInGlobalInvocationId);
  inst.replaceAllUsesWith(dispatchRayIndex);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.dispatch.rays.dimensions" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitDispatchRaysDimensionsOp(lgc::rt::DispatchRaysDimensionsOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto dispatchRaysDimensions = loadShaderTableVariable(ShaderTable::LaunchSize, m_dispatchRaysInfoDesc);
  inst.replaceAllUsesWith(dispatchRaysDimensions);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.world.ray.origin" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitWorldRayOriginOp(lgc::rt::WorldRayOriginOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto worldRayOrigin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Origin], m_traceParams[TraceParam::Origin]);
  inst.replaceAllUsesWith(worldRayOrigin);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.world.ray.direction" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitWorldRayDirectionOp(lgc::rt::WorldRayDirectionOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto worldRayDir = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Dir], m_traceParams[TraceParam::Dir]);
  inst.replaceAllUsesWith(worldRayDir);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.object.ray.origin" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitObjectRayOriginOp(lgc::rt::ObjectRayOriginOp &inst) {
  m_builder->SetInsertPoint(&inst);

  Value *origin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Origin], m_traceParams[TraceParam::Origin]);

  m_worldToObjMatrix = !m_worldToObjMatrix ? createLoadRayTracingMatrix(BuiltInWorldToObjectKHR) : m_worldToObjMatrix;

  // one = vec3(1.0f)
  Value *one = ConstantFP::get(m_worldToObjMatrix->getType()->getArrayElementType(), 1.0);
  // vec3 -> vec4, origin = vec4(origin.xyz, 1.0>
  origin = m_builder->CreateShuffleVector(origin, one, ArrayRef<int>{0, 1, 2, 3});
  // Transform origin
  origin = m_builder->CreateMatrixTimesVector(m_worldToObjMatrix, origin);
  // vec4 -> vec3
  auto objectRayOrigin = m_builder->CreateShuffleVector(origin, origin, ArrayRef<int>{0, 1, 2});

  inst.replaceAllUsesWith(objectRayOrigin);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.object.ray.direction" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitObjectRayDirectionOp(lgc::rt::ObjectRayDirectionOp &inst) {
  m_builder->SetInsertPoint(&inst);

  Value *dir = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Dir], m_traceParams[TraceParam::Dir]);
  m_worldToObjMatrix = !m_worldToObjMatrix ? createLoadRayTracingMatrix(BuiltInWorldToObjectKHR) : m_worldToObjMatrix;

  // zero = vec3(0.0f)
  Value *zero = ConstantFP::get(m_worldToObjMatrix->getType()->getArrayElementType(), 0.0);
  // vec3 -> vec4, vec4 dir = vec4(dir.xyz, 0.0)
  dir = m_builder->CreateShuffleVector(dir, zero, ArrayRef<int>{0, 1, 2, 3});
  // Transform dir
  dir = m_builder->CreateMatrixTimesVector(m_worldToObjMatrix, dir);
  // vec4 -> vec3
  auto objectRayDir = m_builder->CreateShuffleVector(dir, dir, ArrayRef<int>{0, 1, 2});

  inst.replaceAllUsesWith(objectRayDir);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.tmin" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitRayTminOp(lgc::rt::RayTminOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto tMin = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMin], m_traceParams[TraceParam::TMin]);
  inst.replaceAllUsesWith(tMin);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.tcurrent" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitRayTcurrentOp(lgc::rt::RayTcurrentOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto tMax = m_builder->CreateLoad(m_traceParamsTys[TraceParam::TMax], m_traceParams[TraceParam::TMax]);
  inst.replaceAllUsesWith(tMax);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.instance.index" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitInstanceIndexOp(lgc::rt::InstanceIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto instNodeAddr = createLoadInstNodeAddr();
  auto instanceIndex = createLoadInstanceIndexOrId(instNodeAddr, true);
  inst.replaceAllUsesWith(instanceIndex);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.object.to.world" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitObjectToWorldOp(lgc::rt::ObjectToWorldOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto objectToWorld = createLoadRayTracingMatrix(BuiltInObjectToWorldKHR);
  inst.replaceAllUsesWith(objectToWorld);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.world.to.object" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitWorldToObjectOp(lgc::rt::WorldToObjectOp &inst) {
  m_builder->SetInsertPoint(&inst);

  m_worldToObjMatrix = !m_worldToObjMatrix ? createLoadRayTracingMatrix(BuiltInWorldToObjectKHR) : m_worldToObjMatrix;
  inst.replaceAllUsesWith(m_worldToObjMatrix);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.hit.kind" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitHitKindOp(lgc::rt::HitKindOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto hitKind = m_builder->CreateLoad(m_traceParamsTys[TraceParam::Kind], m_traceParams[TraceParam::Kind]);
  inst.replaceAllUsesWith(hitKind);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.triangle.vertex.position" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitTriangleVertexPositionsOp(lgc::rt::TriangleVertexPositionsOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto triangleVertexPositions = m_builder->CreateLoad(m_traceParamsTys[TraceParam::HitTriangleVertexPositions],
                                                       m_traceParams[TraceParam::HitTriangleVertexPositions]);

  // GPURT returns { <3 x float>, <3 x float>, <3 x float> }, but shader requires [3 x <3 x float>].
  Value *newVal = PoisonValue::get(inst.getType());
  for (unsigned i = 0; i < 3; i++) {
    newVal = m_builder->CreateInsertValue(newVal, m_builder->CreateExtractValue(triangleVertexPositions, i), i);
  }

  inst.replaceAllUsesWith(newVal);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.ray.flags" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitRayFlagsOp(lgc::rt::RayFlagsOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto rayFlags = m_builder->CreateLoad(m_traceParamsTys[TraceParam::RayFlags], m_traceParams[TraceParam::RayFlags]);
  inst.replaceAllUsesWith(rayFlags);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.geometry.index" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitGeometryIndexOp(lgc::rt::GeometryIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto geometryIndex =
      m_builder->CreateLoad(m_traceParamsTys[TraceParam::GeometryIndex], m_traceParams[TraceParam::GeometryIndex]);
  inst.replaceAllUsesWith(geometryIndex);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.instance.id" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitInstanceIdOp(lgc::rt::InstanceIdOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto instNodeAddr = createLoadInstNodeAddr();
  auto instanceId = createLoadInstanceIndexOrId(instNodeAddr, false);
  inst.replaceAllUsesWith(instanceId);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.primitive.index" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitPrimitiveIndexOp(lgc::rt::PrimitiveIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto primitiveIndex =
      m_builder->CreateLoad(m_traceParamsTys[TraceParam::PrimitiveIndex], m_traceParams[TraceParam::PrimitiveIndex]);
  inst.replaceAllUsesWith(primitiveIndex);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.instance.inclusion.mask" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitInstanceInclusionMaskOp(lgc::rt::InstanceInclusionMaskOp &inst) {
  m_builder->SetInsertPoint(&inst);

  auto cullMask = m_builder->CreateLoad(m_traceParamsTys[TraceParam::InstanceInclusionMask],
                                        m_traceParams[TraceParam::InstanceInclusionMask]);
  inst.replaceAllUsesWith(cullMask);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// Visits "lgc.rt.shader.index" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitShaderIndexOp(lgc::rt::ShaderIndexOp &inst) {
  // FIXME: This could be wrong if lgc.rt.shader.index is not in the same function as m_shaderRecordIndex, but is
  // this really the case?
  inst.replaceAllUsesWith(m_shaderRecordIndex);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Visits "lgc.rt.shader.record.buffer" instructions
//
// @param inst : The instruction
void SpirvLowerRayTracing::visitShaderRecordBufferOp(lgc::rt::ShaderRecordBufferOp &inst) {
  m_builder->SetInsertPoint(m_insertPosPastInit);

  auto tableIndex = inst.getShaderIndex();

  Value *tableAddr = nullptr;
  Value *tableStride = nullptr;

  switch (m_shaderStage) {
  case ShaderStageRayTracingRayGen: {
    tableAddr = loadShaderTableVariable(ShaderTable::RayGenTableAddr, m_dispatchRaysInfoDesc);
    tableStride = m_builder->getInt32(0);
    break;
  }
  case ShaderStageRayTracingClosestHit:
  case ShaderStageRayTracingAnyHit:
  case ShaderStageRayTracingIntersect: {
    tableAddr = loadShaderTableVariable(ShaderTable::HitGroupTableAddr, m_dispatchRaysInfoDesc);
    tableStride = loadShaderTableVariable(ShaderTable::HitGroupTableStride, m_dispatchRaysInfoDesc);
    break;
  }
  case ShaderStageRayTracingCallable: {
    tableAddr = loadShaderTableVariable(ShaderTable::CallableTableAddr, m_dispatchRaysInfoDesc);
    tableStride = loadShaderTableVariable(ShaderTable::CallableTableStride, m_dispatchRaysInfoDesc);
    break;
  }
  case ShaderStageRayTracingMiss: {
    tableAddr = loadShaderTableVariable(ShaderTable::MissTableAddr, m_dispatchRaysInfoDesc);
    tableStride = loadShaderTableVariable(ShaderTable::MissTableStride, m_dispatchRaysInfoDesc);
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

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 484034
  // Old version without the strided buffer pointers

  // Byte offset = (tableStride * tableIndex) + shaderIdsSize
  Value *offset = m_builder->CreateMul(tableIndex, tableStride);
  offset = m_builder->CreateAdd(offset, shaderIdsSizeVal);

  // Zero-extend offset value to 64 bit
  offset = m_builder->CreateZExt(offset, m_builder->getInt64Ty());

  // Final addr
  tableAddr = m_builder->CreateAdd(tableAddr, offset);

  Type *gpuAddrAsPtrTy = PointerType::get(m_builder->getContext(), SPIRAS_Global);
  tableAddr = m_builder->CreateIntToPtr(tableAddr, gpuAddrAsPtrTy);

  inst.replaceAllUsesWith(tableAddr);

  m_callsToLower.push_back(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
#else
  // New version of the code with strided buffer pointers (also handles unknown version, which we treat as latest)
  tableAddr = m_builder->CreateAdd(tableAddr, m_builder->CreateZExt(shaderIdsSizeVal, m_builder->getInt64Ty()));
  tableAddr = m_builder->create<lgc::StridedBufferAddrAndStrideToPtrOp>(tableAddr, tableStride);
  tableAddr = m_builder->create<lgc::StridedIndexAddOp>(tableAddr, tableIndex);

  SmallVector<Instruction *> toRemove;
  toRemove.push_back(&inst);
  replaceAllPointerUses(m_builder, &inst, tableAddr, toRemove);

  for (auto *I : reverse(toRemove))
    I->eraseFromParent();
#endif
}

// =====================================================================================================================
// Creates instructions to emit SQTT shader data call compact token
//
// @param stage : Ray tracing shader stage
void SpirvLowerRayTracing::createSqttCallCompactToken(ShaderStage stage) {
  // The token is a 32-bit uint compacted with following bit representation:
  // 31-13: extended data, 12-8: data_tokens, 7: extended, 6: special, 5-0: well_known
  // If extended is 0, this is a well known packet type, and data_tokens and extended_data may be interpreted as
  // specified by the well_known packet specification.
  union SqttShaderDataToken {
    struct {
      uint32_t well_known : 6;
      uint32_t special : 1;
      uint32_t extended : 1;
      uint32_t data_token : 5;
      uint32_t extended_data : 19;
    } bits;

    uint32_t u32All;
  };

  SqttShaderDataToken token = {};
  token.bits.well_known = SqttWellKnownTypeFunctionCallCompact;
  switch (stage) {
  case ShaderStage::ShaderStageRayTracingAnyHit:
    token.bits.data_token = 1;
    break;
  case ShaderStage::ShaderStageRayTracingClosestHit:
    token.bits.data_token = 2;
    break;
  case ShaderStage::ShaderStageRayTracingIntersect:
    token.bits.data_token = 3;
    break;
  case ShaderStage::ShaderStageRayTracingMiss:
    token.bits.data_token = 4;
    break;
  case ShaderStage::ShaderStageRayTracingRayGen:
    token.bits.data_token = 5;
    break;
  case ShaderStage::ShaderStageCompute:
    token.bits.data_token = 6;
    break;
  case ShaderStage::ShaderStageRayTracingCallable:
    token.bits.data_token = 7;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  // Get number of active lanes
  auto waveSize = m_context->getPipelineContext()->getRayTracingWaveSize();
  Value *activeLaneCount =
      m_builder->CreateIntrinsic(m_builder->getIntNTy(waveSize), Intrinsic::amdgcn_ballot, m_builder->getInt1(true));
  activeLaneCount = m_builder->CreateUnaryIntrinsic(Intrinsic::ctpop, activeLaneCount);
  if (waveSize > 32)
    activeLaneCount = m_builder->CreateTrunc(activeLaneCount, m_builder->getInt32Ty());

  // Left shift 13 to extended_data position
  activeLaneCount = m_builder->CreateShl(activeLaneCount, 13);

  m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_ttracedata, {}, m_builder->CreateOr(activeLaneCount, token.u32All));
}

// =====================================================================================================================
// Creates instructions to emit SQTT shader data function return token
void SpirvLowerRayTracing::createSqttFunctionReturnToken() {
  m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_ttracedata_imm, {},
                             m_builder->getInt16(SqttWellKnownTypeFunctionReturn));
}

// =====================================================================================================================
// Creates instructions to load instance node address
Value *SpirvLowerRayTracing::createLoadInstNodeAddr() {
  auto instNodeAddrTy = m_traceParamsTys[TraceParam::InstNodeAddrLo];
  assert(instNodeAddrTy == m_traceParamsTys[TraceParam::InstNodeAddrHi]);
  Value *instNodeAddrLo = m_builder->CreateLoad(instNodeAddrTy, m_traceParams[TraceParam::InstNodeAddrLo]);
  Value *instNodeAddrHi = m_builder->CreateLoad(instNodeAddrTy, m_traceParams[TraceParam::InstNodeAddrHi]);

  Value *instNodeAddr = PoisonValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 2));
  instNodeAddr = m_builder->CreateInsertElement(instNodeAddr, instNodeAddrLo, uint64_t(0));
  instNodeAddr = m_builder->CreateInsertElement(instNodeAddr, instNodeAddrHi, 1u);

  return instNodeAddr;
}

// =====================================================================================================================
// Creates a implementation function of a call instruction, redirect the call to the new function, and return the
// function.
//
// @param inst : The instruction
// @param args : Additional TraceSet arguments
llvm::Function *SpirvLowerRayTracing::createImplFunc(CallInst &inst, ArrayRef<Value *> args) {
  std::string mangledName = inst.getCalledFunction()->getName().str() + ".impl";
  SmallVector<Value *, 10> implCallArgs(inst.args());
  for (auto &arg : args) {
    implCallArgs.push_back(arg);
  }
  auto newCall = m_builder->CreateNamedCall(mangledName, inst.getCalledFunction()->getReturnType(), implCallArgs,
                                            {Attribute::NoUnwind, Attribute::AlwaysInline});

  inst.replaceAllUsesWith(newCall);

  return m_module->getFunction(mangledName);
}

lgc::rt::RayTracingShaderStage SpirvLowerRayTracing::mapStageToLgcRtShaderStage(ShaderStage stage) {
  assert((stage >= ShaderStageRayTracingRayGen) && (stage <= ShaderStageRayTracingCallable));
  return static_cast<lgc::rt::RayTracingShaderStage>(stage - ShaderStageRayTracingRayGen);
}

// =====================================================================================================================
// Generate a static ID for current Trace Ray call
//
unsigned SpirvLowerRayTracing::generateTraceRayStaticId() {
  Util::MetroHash64 hasher;
  hasher.Update(m_nextTraceRayId++);
  hasher.Update(m_module->getName().bytes_begin(), m_module->getName().size());

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return MetroHash::compact32(&hash);
}

// =====================================================================================================================
// Erase BasicBlocks from the Function
//
// @param func : Function
void SpirvLowerRayTracing::eraseFunctionBlocks(Function *func) {
  for (auto blockIt = func->begin(), blockEnd = func->end(); blockIt != blockEnd;) {
    BasicBlock *basicBlock = &*blockIt++;
    basicBlock->dropAllReferences();
    basicBlock->eraseFromParent();
  }
}

// =====================================================================================================================
// Call GpuRt Library Func to load a 3x4 matrix from given address at the current insert point
//
// @param instanceNodeAddr : instanceNode address, which type is i64
Value *SpirvLowerRayTracing::createLoadMatrixFromFunc(Value *instanceNodeAddr, unsigned builtInId) {
  auto floatx3Ty = FixedVectorType::get(m_builder->getFloatTy(), 3);
  auto matrixTy = ArrayType::get(floatx3Ty, 4);

  Value *instandeNodeAddrPtr = m_builder->CreateAllocaAtFuncEntry(m_builder->getInt64Ty());
  m_builder->CreateStore(instanceNodeAddr, instandeNodeAddrPtr);

  StringRef getMatrixFunc;
  if (builtInId == BuiltInObjectToWorldKHR) {
    getMatrixFunc =
        m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_OBJECT_TO_WORLD_TRANSFORM);
  } else {
    getMatrixFunc =
        m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_WORLD_TO_OBJECT_TRANSFORM);
  }

  Value *matrixRow[4] = {
      PoisonValue::get(floatx3Ty),
      PoisonValue::get(floatx3Ty),
      PoisonValue::get(floatx3Ty),
      PoisonValue::get(floatx3Ty),
  };

  for (unsigned i = 0; i < 3; ++i) {
    Value *row = m_builder->getInt32(i);
    for (unsigned j = 0; j < 4; ++j) {
      Value *col = m_builder->getInt32(j);

      Value *colPtr = m_builder->CreateAllocaAtFuncEntry(m_builder->getInt32Ty());
      Value *rowPtr = m_builder->CreateAllocaAtFuncEntry(m_builder->getInt32Ty());
      m_builder->CreateStore(col, colPtr);
      m_builder->CreateStore(row, rowPtr);

      auto cmiMatrixResult = m_crossModuleInliner.value().inlineCall(*m_builder, getGpurtFunction(getMatrixFunc),
                                                                     {instandeNodeAddrPtr, rowPtr, colPtr});
      matrixRow[j] = m_builder->CreateInsertElement(matrixRow[j], cmiMatrixResult.returnValue, uint64_t(i));
    }
  }

  Value *matrix = PoisonValue::get(matrixTy);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[0], 0);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[1], 1);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[2], 2);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[3], 3);
  return matrix;
}

// =====================================================================================================================
// Looks up an exported function in the GPURT module
Function *SpirvLowerRayTracing::getGpurtFunction(StringRef name) {
  auto &gpurtContext = lgc::GpurtContext::get(*m_context);
  Function *fn = gpurtContext.theModule->getFunction(name);
  assert(fn);
  return fn;
}

// =====================================================================================================================
// Create instructions to load instance index/id given the 64-bit instance node address at the current insert point
// Note: HLSL has just the opposite naming of index/ID compares to SPIR-V.
// So "isIndex = true" means we use InstanceId(InstanceIndex for GPURT) for vulkan,
// and "isIndex = false" means we use InstanceIndex(InstanceId for GPURT) for vulkan,
// @param instNodeAddr : 64-bit instance node address, in <2 x i32>
Value *SpirvLowerRayTracing::createLoadInstanceIndexOrId(Value *instNodeAddr, bool isIndex) {
  Value *instanceIdPtr = m_builder->CreateAllocaAtFuncEntry(m_builder->getInt64Ty());
  m_builder->CreateStore(instNodeAddr, instanceIdPtr);

  StringRef getterName = isIndex
                             ? m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_INSTANCE_INDEX)
                             : m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_INSTANCE_ID);

  auto cmiResult = m_crossModuleInliner.value().inlineCall(*m_builder, getGpurtFunction(getterName), {instanceIdPtr});

  return cmiResult.returnValue;
}

} // namespace Llpc
