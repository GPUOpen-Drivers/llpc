/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// LowerRayQuery.cpp : Pass to lower rayQuery ops by inlining GPURT functions.
// Typically used by running a pass class that derives from this one, setting m_staticFlags and setting up
// a GpurtContext as appropriate.

#include "llvmraytracing/LowerRayQuery.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypeLowering.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/LgcRtqDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "lgc-lower-rayquery"
using namespace lgc;
using namespace lgc::rt;
using namespace llvm;
using namespace llvm_dialects;
using namespace CompilerUtils;

// Table of GPURT function names. Must match the order of enum GpurtFunc.
static const char *const GpurtFuncNames[] = {
    "_RayQuery_Abort",
    "_RayQuery_Allocate",
    "_RayQuery_CandidateAabbOpaque",
    "_RayQuery_CommitNonOpaqueTriangleHit",
    "_RayQuery_CommitProceduralPrimitiveHit",
    "_RayQuery_EndInterleavedProceed",
    "FetchTrianglePositionFromRayQuery",
    "_RayQuery_GeometryIndex",
    "_RayQuery_GetObjId",
    "_RayQuery_InstanceContributionToHitGroupIndex",
    "_RayQuery_InstanceID",
    "_RayQuery_InstanceIndex",
    "_RayQuery_IntersectionType",
    "LongRayQueryProceedAMD",
    "_RayQuery_ObjectRayDirection",
    "_RayQuery_ObjectRayOrigin",
    "_RayQuery_ObjectToWorld4x3",
    "_RayQuery_PrimitiveIndex",
    "_RayQuery_RayFlags",
    "RayQueryProceed",
    "_RayQuery_RayT",
    "_RayQuery_RayTMin",
    "_RayQuery_SetObjId",
    "TraceRayInline",
    "_RayQuery_TriangleBarycentrics",
    "_RayQuery_TriangleFrontFace",
    "_RayQuery_WorldRayDirection",
    "_RayQuery_WorldRayOrigin",
    "_RayQuery_WorldToObject4x3",
};
static_assert(sizeof(GpurtFuncNames) / sizeof(GpurtFuncNames[0]) == unsigned(LowerRayQuery::GpurtFunc::Count),
              "Table size mismatch");

namespace RtqAlloc {
enum : unsigned {
  RayQueryId,     // Rayquery Id
  PrevRayQueryId, // Previous rayquery Id
  BoolP,          // Committed condition
  Count
};
}

struct LoweringVisitorRtqType {
  LowerRayQuery *pass;
  TypeLowering typeLower;
  explicit LoweringVisitorRtqType(Type *rtqType, LowerRayQuery *pass) : pass(pass), typeLower(rtqType->getContext()) {
    typeLower.addRule([pass](TypeLowering &, Type * type) -> auto {
      SmallVector<Type *> loweredTy;
      if (pass->hasRtqOpaqueType(type)) {
        loweredTy.push_back(pass->replaceRayQueryType(type));
      }
      return loweredTy;
    });
  }
};

template <> struct llvm_dialects::VisitorPayloadProjection<LoweringVisitorRtqType, LowerRayQuery> {
  static LowerRayQuery &project(LoweringVisitorRtqType &payload) { return *payload.pass; }
};

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(LoweringVisitorRtqType, typeLower)

// =====================================================================================================================
// Lower InitializeOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitInitializeOp(rtq::InitializeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  SmallVector<Value *> args;
  auto rayQuery = getRayQuery(inst.getRayQuery());
  Function *traceRayInlineFunc = getGpurtFunc(GpurtFunc::TraceRayInline);
  Type *rayDescTy = traceRayInlineFunc->getFunctionType()->getParamType(6);
  // 0, rayQuery
  args.push_back(rayQuery);
  // 1, Scene addr low,
  // 2, Scene addr high
  Type *int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Value *scene = m_builder->CreateBitCast(inst.getAccelerationStructure(), int32x2Ty);
  Value *sceneAddLow = m_builder->CreateExtractElement(scene, uint64_t(0));
  Value *sceneAddHigh = m_builder->CreateExtractElement(scene, 1);
  args.push_back(sceneAddLow);
  args.push_back(sceneAddHigh);
  // 3, Const ray flags
  args.push_back(m_builder->getInt32(0));
  // 4, Ray flags
  args.push_back(inst.getRayFlags());
  // 5 instance mask
  args.push_back(inst.getInstanceInclusionMask());
  // 6 RayDesc
  Value *rayDesc = PoisonValue::get(rayDescTy);
  // Origin
  rayDesc = m_builder->CreateInsertValue(rayDesc, inst.getRayOrigin(), 0u);
  // TMin
  rayDesc = m_builder->CreateInsertValue(rayDesc, inst.getTMin(), 1u);
  // Direction
  rayDesc = m_builder->CreateInsertValue(rayDesc, inst.getDirection(), 2u);
  // TMax
  rayDesc = m_builder->CreateInsertValue(rayDesc, inst.getTMax(), 3u);
  args.push_back(rayDesc);
  // 7 dispatchId
  args.push_back(m_builder->create<lgc::GpurtGetRayQueryDispatchIdOp>());
  CrossModuleInliner inliner;
  inliner.inlineCall(*m_builder, traceRayInlineFunc, args);
  setRtqObjId(inst, rayQuery);

  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower TerminateOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitTerminateOp(rtq::TerminateOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rayQuery = getRayQuery(inst.getRayQuery());
  CrossModuleInliner inliner;
  inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::Abort), {rayQuery});
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower ProceedOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitProceedOp(rtq::ProceedOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rayQuery = getRayQuery(inst.getRayQuery());
  CrossModuleInliner inliner;
  // Only use GetObjId if GPURT has it.
  if (Function *getObjIdFunc = getGpurtFunc(GpurtFunc::GetObjId, /*optional=*/true)) {
    Value *rayQueryObj = inliner.inlineCall(*m_builder, getObjIdFunc, {rayQuery}).returnValue;
    // Check interleaved proceed, aka, proceed on the same rayquery object
    Value *notEqual = m_builder->CreateICmpNE(
        rayQueryObj, m_builder->CreateLoad(m_builder->getInt32Ty(), m_rtqAlloc[RtqAlloc::PrevRayQueryId]));
    Instruction *terminator = SplitBlockAndInsertIfThen(notEqual, m_builder->GetInsertPoint(), false);
    m_builder->SetInsertPoint(terminator);
    inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::EndInterleavedProceed), {rayQuery});
    m_builder->SetInsertPoint(&inst);
    m_builder->CreateStore(rayQueryObj, m_rtqAlloc[RtqAlloc::PrevRayQueryId]);
  } else {
    // If GPURT does not have GetObjId, we have to assume always interleaved,
    // which is suboptimal.
    inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::EndInterleavedProceed), {rayQuery});
  }

  // Call proceed function. Use LongRay version if available.
  Value *zero = m_builder->getInt32(0);
  Value *proceedResult = nullptr;
  if (Function *proceedFunc = getGpurtFunc(GpurtFunc::LongRayQueryProceed, /*optional=*/true)) {
    auto earlyRayThreshold = ConstantFP::get(m_builder->getFloatTy(), 0.0);
    Value *args[] = {rayQuery, zero, earlyRayThreshold, m_builder->create<GpurtGetRayQueryDispatchIdOp>()};
    proceedResult = inliner.inlineCall(*m_builder, proceedFunc, args).returnValue;
  } else {
    Value *args[] = {rayQuery, zero, m_builder->create<GpurtGetRayQueryDispatchIdOp>()};
    proceedResult = inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::RayQueryProceed), args).returnValue;
  }
  inst.replaceAllUsesWith(proceedResult);
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower IntersectionCommitAabbOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionCommitAabbOp(rtq::IntersectionCommitAabbOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rayQuery = getRayQuery(inst.getRayQuery());
  CrossModuleInliner inliner;
  inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::CommitProceduralPrimitiveHit), {rayQuery, inst.getTHit()});
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower IntersectionCommitTriangleOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionCommitTriangleOp(rtq::IntersectionCommitTriangleOp &inst) {
  m_builder->SetInsertPoint(&inst);
  auto rayQuery = getRayQuery(inst.getRayQuery());
  CrossModuleInliner inliner;
  inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::CommitNonOpaqueTriangleHit), {rayQuery});
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower IntersectionTypeOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionTypeOp(rtq::IntersectionTypeOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::IntersectionType, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower RayTMinOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitRayTMinOp(rtq::RayTMinOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitAccessor(GpurtFunc::RayTMin, inst.getRayQuery(), &inst);
}

// =====================================================================================================================
// Lower RayFlagsOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitRayFlagsOp(rtq::RayFlagsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitAccessor(GpurtFunc::RayFlags, inst.getRayQuery(), &inst);
}

// =====================================================================================================================
// Lower IntersectionTOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionTOp(rtq::IntersectionTOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::RayT, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionInstanceIdOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionInstanceIdOp(rtq::IntersectionInstanceIdOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::InstanceID, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionInstanceIndexOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionInstanceIndexOp(rtq::IntersectionInstanceIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::InstanceIndex, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionContributionToHitGroupIndexOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionContributionToHitGroupIndexOp(
    rtq::IntersectionContributionToHitGroupIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::InstanceContributionToHitGroupIndex, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionGeometryIndexOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionGeometryIndexOp(rtq::IntersectionGeometryIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::GeometryIndex, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionPrimitiveIndexOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionPrimitiveIndexOp(rtq::IntersectionPrimitiveIndexOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::PrimitiveIndex, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionPrimitiveIndexOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionBarycentricsOp(rtq::IntersectionBarycentricsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::TriangleBarycentrics, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionFrontFaceOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionFrontFaceOp(rtq::IntersectionFrontFaceOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::TriangleFrontFace, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionCandidateAabbOpaqueOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionCandidateAabbOpaqueOp(rtq::IntersectionCandidateAabbOpaqueOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitAccessor(GpurtFunc::CandidateAabbOpaque, inst.getRayQuery(), &inst);
}

// =====================================================================================================================
// Lower IntersectionObjectRayDirectionOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionObjectRayDirectionOp(rtq::IntersectionObjectRayDirectionOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::ObjectRayDirection, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionObjectRayOriginOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionObjectRayOriginOp(rtq::IntersectionObjectRayOriginOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::ObjectRayOrigin, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionWorldRayDirectionOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionWorldRayDirectionOp(rtq::IntersectionWorldRayDirectionOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitAccessor(GpurtFunc::WorldRayDirection, inst.getRayQuery(), &inst);
}

// =====================================================================================================================
// Lower IntersectionWorldRayOriginOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionWorldRayOriginOp(rtq::IntersectionWorldRayOriginOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitAccessor(GpurtFunc::WorldRayOrigin, inst.getRayQuery(), &inst);
}

// =====================================================================================================================
// Lower IntersectionObjectToWorldOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionObjectToWorldOp(rtq::IntersectionObjectToWorldOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::ObjectToWorld4x3, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionWorldToObjectOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionWorldToObjectOp(rtq::IntersectionWorldToObjectOp &inst) {
  m_builder->SetInsertPoint(&inst);
  visitHitAccessor(GpurtFunc::WorldToObject4x3, inst.getRayQuery(), inst.getCommitted(), &inst);
}

// =====================================================================================================================
// Lower IntersectionTriangleVertexPositionsOp dialect
//
// @param inst : the instruction to lower
void LowerRayQuery::visitIntersectionTriangleVertexPositionsOp(rtq::IntersectionTriangleVertexPositionsOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Value *rayQuery = getRayQuery(inst.getRayQuery());
  CrossModuleInliner inliner;
  auto call = inliner.inlineCall(*m_builder, getGpurtFunc(GpurtFunc::FetchTrianglePositionFromRayQuery),
                                 {rayQuery, m_builder->getInt1(inst.getCommitted())});
  auto triangleData = call.returnValue;
  auto floatx3Ty = FixedVectorType::get(m_builder->getFloatTy(), 3);
  auto retType = ArrayType::get(floatx3Ty, 3);
  // Convert from struct TriangleData to the array of vec3
  Value *vertexPos = PoisonValue::get(retType);
  for (unsigned i = 0; i < 3; i++)
    vertexPos = m_builder->CreateInsertValue(vertexPos, m_builder->CreateExtractValue(triangleData, {i}), {i});
  inst.replaceAllUsesWith(vertexPos);
  m_typeLowering->eraseInstruction(&inst);
  m_funcsToLower.insert(inst.getCalledFunction());
}

// =====================================================================================================================
// Lower AllocInst instruction
//
// @param inst : the instruction to lower
VisitorResult LowerRayQuery::visitAlloca(AllocaInst &inst) {
  auto types = m_typeLowering->convertType(inst.getAllocatedType());
  if (!types.empty() && types[0] != inst.getAllocatedType()) {
    m_builder->SetInsertPoint(&inst);
    auto newAllocRtq = m_builder->CreateAlloca(types[0]);
    m_typeLowering->replaceInstruction(&inst, newAllocRtq);
  }
  return VisitorResult::Stop;
}

// =====================================================================================================================
// Set RayQuery ObjectID
//
// @param inst : the instruction to lower
// @param rtq : the rayquery object
void LowerRayQuery::setRtqObjId(rtq::InitializeOp &inst, Value *rtq) {
  // Only use SetObjId if GPURT has it.
  if (Function *setObjIdFunc = getGpurtFunc(GpurtFunc::SetObjId, /*optional=*/true)) {
    CrossModuleInliner inliner;
    inliner.inlineCall(*m_builder, setObjIdFunc, {rtq, m_rtqAlloc[RtqAlloc::RayQueryId]});
  }
  Value *rayQueryObjId = m_builder->CreateLoad(m_builder->getInt32Ty(), m_rtqAlloc[RtqAlloc::RayQueryId]);
  m_builder->CreateStore(m_builder->CreateAdd(rayQueryObjId, m_builder->getInt32(1)), m_rtqAlloc[RtqAlloc::RayQueryId]);
}

// =====================================================================================================================
// Visit ptrtoint instruction, in case its input is a pointer that we lowered.
void LowerRayQuery::visitPtrToInt(PtrToIntInst &inst) {
  auto loweredVals = m_typeLowering->getValueOptional(inst.getOperand(0));
  if (!loweredVals.empty())
    inst.setOperand(0, loweredVals[0]);
}

// =====================================================================================================================
// Visit lgc.GepOpaqueOp instruction
//
// @param inst : the instruction to lower

void LowerRayQuery::visitGepOpaqueOp(rtq::GepOpaqueOp &inst) {
  m_builder->SetInsertPoint(&inst);
  Type *gepTy = replaceRayQueryType(inst.getBaseType());
  Value *srcElement = m_typeLowering->getValue(inst.getBasePointer())[0];
  Value *newGep = nullptr;
  SmallVector<Value *> indices;
  indices.insert(indices.end(), inst.getOffsets().begin(), inst.getOffsets().end());
  if (inst.getInbound())
    newGep = m_builder->CreateInBoundsGEP(gepTy, srcElement, indices);
  else
    newGep = m_builder->CreateGEP(gepTy, srcElement, indices);
  // If the result of the GEP is not a type that we lower (is not and does not
  // contain i127), then manually replace uses here.
  SmallVector<Value *> offsets;
  for (Value *offset : inst.getOffsets())
    offsets.push_back(offset);
  Type *elementTy = GetElementPtrInst::getIndexedType(inst.getBaseType(), offsets);
  if (m_typeLowering->convertType(elementTy)[0] == elementTy)
    inst.replaceAllUsesWith(newGep);
  // Replace with the new GEP.
  m_typeLowering->replaceInstruction(&inst, newGep);
}

// =====================================================================================================================
// Lower LifetimeIntrinsic instruction
//
// @param inst : the instruction to lower
VisitorResult LowerRayQuery::visitLifetimeIntrinsic(LifetimeIntrinsic &inst) {
  Value *arg = inst.getArgOperand(1);
  if (m_typeLowering->getValueOptional(arg).size())
    m_typeLowering->eraseInstruction(&inst);

  return VisitorResult::Stop;
}

// =====================================================================================================================
// Initialize alloc used later for gpurt functions calling
//
// @param func : the function to create alloc
void LowerRayQuery::initializeAlloc(Function *func) {
  assert(m_rtqAlloc.empty());
  Type *funcArgTys[RtqAlloc::Count] = {
      m_builder->getInt32Ty(), // RayQueryId
      m_builder->getInt32Ty(), // PreviousRayQueryId
      m_builder->getInt1Ty(),  // bool committed
  };
  m_builder->SetInsertPointPastAllocas(func);
  for (auto ty : funcArgTys)
    m_rtqAlloc.push_back(m_builder->CreateAlloca(ty, func->getParent()->getDataLayout().getAllocaAddrSpace()));

  m_builder->CreateStore(m_builder->getInt32(0), m_rtqAlloc[RtqAlloc::RayQueryId]);
  m_builder->CreateStore(m_builder->getInt32(UINT32_MAX), m_rtqAlloc[RtqAlloc::PrevRayQueryId]);
}

// =====================================================================================================================
// Visit RayQueryInternal commit/candidate RaySystemData member
//
// @param funcType : the gpurt function to access member
// @param rayQuery : the internal rayquery structure
// @param committed : commit or candidate member
// @param inst : instruction to lower
void LowerRayQuery::visitHitAccessor(GpurtFunc funcType, Value *rayQuery, bool committed, CallBase *inst) {
  rayQuery = getRayQuery(rayQuery);
  Function *gpurtFunc = getGpurtFunc(funcType);
  Value *committedArg = m_builder->getInt1(committed);

  // We need to cope with arg 1 (committed) being either an i1 or a pointer to
  // i1. Pointer to i1 happens when GPURT is compiled to SPIR-V by DXC. A more
  // correct fix would be to get llpcSpirvProcessGpurt to promote the arg,
  // but there are 13 separate GPURT rayQuery functions involved, and building
  // knowledge of that into llpcSpirvProcessGpurt would be too fiddly.
  if (isa<PointerType>(gpurtFunc->getFunctionType()->getParamType(1))) {
    m_builder->CreateStore(committedArg, m_rtqAlloc[RtqAlloc::BoolP]);
    committedArg = m_rtqAlloc[RtqAlloc::BoolP];
  }

  CrossModuleInliner inliner;
  auto call = inliner.inlineCall(*m_builder, gpurtFunc, {rayQuery, committedArg});
  inst->replaceAllUsesWith(call.returnValue);
  m_typeLowering->eraseInstruction(inst);
  m_funcsToLower.insert(inst->getCalledFunction());
}

// =====================================================================================================================
// Visit RayQueryInternal member
//
// @param funcType : the gpurt function to access member
// @param rayQuery : the internal rayquery structure
// @param inst : instruction to lower
void LowerRayQuery::visitAccessor(GpurtFunc funcType, Value *rayQuery, CallBase *inst) {
  rayQuery = getRayQuery(rayQuery);
  CrossModuleInliner inliner;
  auto call = inliner.inlineCall(*m_builder, getGpurtFunc(funcType), {rayQuery});
  inst->replaceAllUsesWith(call.returnValue);
  m_typeLowering->eraseInstruction(inst);
  m_funcsToLower.insert(inst->getCalledFunction());
}

// =====================================================================================================================
// Visit lgc.gpurt.get.static.flags op
void LowerRayQuery::visitGetStaticFlagsOp(GpurtGetStaticFlagsOp &inst) {
  inst.replaceAllUsesWith(m_builder->getInt32(m_staticFlags));
}

// =====================================================================================================================
// Visits "lgc.gpurt.stack.read" instructions
//
// @param inst : The instruction
void LowerRayQuery::visitStackReadOp(GpurtStackReadOp &inst) {
  auto stage = getLgcRtShaderStage(inst.getFunction());
  if (stage == RayTracingShaderStage::AnyHit || stage == RayTracingShaderStage::Intersection)
    inst.setUseExtraStack(true);
}

// =====================================================================================================================
// Visits "lgc.gpurt.stack.write" instructions
//
// @param inst : The instruction
void LowerRayQuery::visitStackWriteOp(GpurtStackWriteOp &inst) {
  auto stage = getLgcRtShaderStage(inst.getFunction());
  if (stage == RayTracingShaderStage::AnyHit || stage == RayTracingShaderStage::Intersection)
    inst.setUseExtraStack(true);
}

// =====================================================================================================================
// Visits "lgc.gpurt.stack.init" instructions
//
// @param inst : The instruction
void LowerRayQuery::visitLdsStackInitOp(GpurtLdsStackInitOp &inst) {
  auto stage = getLgcRtShaderStage(inst.getFunction());
  if (stage == RayTracingShaderStage::AnyHit || stage == RayTracingShaderStage::Intersection)
    inst.setUseExtraStack(true);
}

// =====================================================================================================================
// Executes this LowerRayquery pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerRayQuery::run(Module &module, ModuleAnalysisManager &analysisManager) {
  llvm_dialects::Builder builderImpl(module.getContext());
  m_builder = &builderImpl;

  Function *gpurtFuncs[unsigned(GpurtFunc::Count)] = {};
  m_gpurtFuncs = gpurtFuncs;
  m_gpurtModule = GpurtContext::get(module.getContext()).theModule;

  typedef SmallSetVector<Function *, 4> FuncSet;
  FuncSet rayQueryFuncs;

  static auto findRayqueryDialect =
      llvm_dialects::VisitorBuilder<FuncSet>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<rtq::InitializeOp>([](FuncSet &funcSet, auto &inst) { funcSet.insert(inst.getFunction()); })
          .build();
  findRayqueryDialect.visit(rayQueryFuncs, module);

  if (rayQueryFuncs.empty())
    return PreservedAnalyses::all();

  // Get the ray-query object type from the return type of the GPURT _rayquery_allocate function; we do not
  // otherwise use that function.
  Function *allocateFunc = getGpurtFunc(GpurtFunc::Allocate);
  m_rtqType = allocateFunc->getFunctionType()->getReturnType();
  LoweringVisitorRtqType payload(m_rtqType, this);
  m_typeLowering = &payload.typeLower;

  static const auto visitor = llvm_dialects::VisitorBuilder<LoweringVisitorRtqType>()
                                  .nest<LowerRayQuery>([](auto &b) {
                                    b.add(&LowerRayQuery::visitAlloca);
                                    b.add(&LowerRayQuery::visitPtrToInt);
                                    b.add(&LowerRayQuery::visitLifetimeIntrinsic);
                                    b.add(&LowerRayQuery::visitInitializeOp);
                                    b.add(&LowerRayQuery::visitTerminateOp);
                                    b.add(&LowerRayQuery::visitProceedOp);
                                    b.add(&LowerRayQuery::visitIntersectionCommitAabbOp);
                                    b.add(&LowerRayQuery::visitIntersectionCommitTriangleOp);
                                    b.add(&LowerRayQuery::visitIntersectionTypeOp);
                                    b.add(&LowerRayQuery::visitRayTMinOp);
                                    b.add(&LowerRayQuery::visitRayFlagsOp);
                                    b.add(&LowerRayQuery::visitIntersectionTOp);
                                    b.add(&LowerRayQuery::visitIntersectionInstanceIdOp);
                                    b.add(&LowerRayQuery::visitIntersectionInstanceIndexOp);
                                    b.add(&LowerRayQuery::visitIntersectionContributionToHitGroupIndexOp);
                                    b.add(&LowerRayQuery::visitIntersectionGeometryIndexOp);
                                    b.add(&LowerRayQuery::visitIntersectionPrimitiveIndexOp);
                                    b.add(&LowerRayQuery::visitIntersectionBarycentricsOp);
                                    b.add(&LowerRayQuery::visitIntersectionFrontFaceOp);
                                    b.add(&LowerRayQuery::visitIntersectionCandidateAabbOpaqueOp);
                                    b.add(&LowerRayQuery::visitIntersectionObjectRayDirectionOp);
                                    b.add(&LowerRayQuery::visitIntersectionObjectRayOriginOp);
                                    b.add(&LowerRayQuery::visitIntersectionTriangleVertexPositionsOp);
                                    b.add(&LowerRayQuery::visitIntersectionWorldRayDirectionOp);
                                    b.add(&LowerRayQuery::visitIntersectionWorldRayOriginOp);
                                    b.add(&LowerRayQuery::visitIntersectionObjectToWorldOp);
                                    b.add(&LowerRayQuery::visitIntersectionWorldToObjectOp);
                                    b.add(&LowerRayQuery::visitGepOpaqueOp);
                                  })
                                  .nest(&TypeLowering::registerVisitors)
                                  .build();

  for (auto func : rayQueryFuncs) {
    initializeAlloc(func);
    visitor.visit(payload, *func);
    m_rtqAlloc.clear();
  }

  payload.typeLower.finishPhis();
  payload.typeLower.finishCleanup();

  static auto postVisit = llvm_dialects::VisitorBuilder<LowerRayQuery>()
                              .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                              .add(&LowerRayQuery::visitGetStaticFlagsOp)
                              .add(&LowerRayQuery::visitStackReadOp)
                              .add(&LowerRayQuery::visitStackWriteOp)
                              .add(&LowerRayQuery::visitLdsStackInitOp)
                              .build();
  postVisit.visit(*this, module);

  m_typeLowering = nullptr;
  for (Function *func : m_funcsToLower) {
    func->dropAllReferences();
    func->eraseFromParent();
  }
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Recursive replace i127 opaque to the rayQueryInternal in the aggregation type
//
// @param ty : The type to replace
Type *LowerRayQuery::replaceRayQueryType(Type *ty) {
  if (rtq::isRayQueryType(ty))
    return m_rtqType;
  if (ty->isStructTy()) {
    SmallVector<Type *> elemTys;
    for (unsigned i = 0; i < ty->getStructNumElements(); ++i)
      elemTys.push_back(replaceRayQueryType(ty->getStructElementType(i)));
    return StructType::get(m_rtqType->getContext(), elemTys);
  }
  if (ty->isArrayTy())
    return ArrayType::get(replaceRayQueryType(ty->getArrayElementType()), ty->getArrayNumElements());

  return ty;
}

// =====================================================================================================================
// Recursive find i127 opaque in the aggregation type
//
// @param ty : The type to find
bool LowerRayQuery::hasRtqOpaqueType(Type *ty) {
  if (rtq::isRayQueryType(ty))
    return true;
  if (ty->isStructTy()) {
    bool isMemberRtq = false;
    for (unsigned i = 0; i < ty->getStructNumElements(); ++i) {
      if ((isMemberRtq = hasRtqOpaqueType(ty->getStructElementType(i))))
        break;
    }
    return isMemberRtq;
  }
  if (ty->isArrayTy())
    return hasRtqOpaqueType(ty->getArrayElementType());

  return false;
}

// =====================================================================================================================
// Given a pointer to an i127 rayQuery object, get the pointer to its actual lowered rayQuery object.
Value *LowerRayQuery::getRayQuery(Value *rayQuery) {
  llvm::SmallVector<llvm::Value *> loweredVals = m_typeLowering->getValueOptional(rayQuery);
  if (!loweredVals.empty()) {
    // This is the case that the value is the alloca or a GEP from it. That was
    // lowered earlier.
    return loweredVals[0];
  }
  // This is the case that the value is something that generates an opaque
  // pointer (e.g. inttoptr), so we just use the original value.
  assert(!isa<AllocaInst>(rayQuery));
  return rayQuery;
}

// =====================================================================================================================
// Get GPURT function given its GpurtFunc::* enum value. The first time a particular function is requested, it
// is lazily found in the GPURT module.
//
// @param gpurtFunc : Enum value for which GPURT function we want
// @param optional : Return nullptr instead of throwing an error if the GPURT function is not found
Function *LowerRayQuery::getGpurtFunc(GpurtFunc gpurtFunc, bool optional) {
  if (m_gpurtFuncs[unsigned(gpurtFunc)])
    return m_gpurtFuncs[unsigned(gpurtFunc)];
  StringRef name = GpurtFuncNames[unsigned(gpurtFunc)];
  m_gpurtFuncs[unsigned(gpurtFunc)] = m_gpurtModule->getFunction(name);
  if (!m_gpurtFuncs[unsigned(gpurtFunc)]) {
    if (!optional)
      report_fatal_error(Twine("GPURT function '") + name + "' not found");
    return nullptr;
  }
  return m_gpurtFuncs[unsigned(gpurtFunc)];
}
