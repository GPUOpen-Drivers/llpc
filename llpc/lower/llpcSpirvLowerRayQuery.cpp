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
 * @file  llpcSpirvLowerRayQuery.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerRayQuery.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerRayQuery.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "llpc-spirv-lower-ray-query"

using namespace spv;
using namespace llvm;
using namespace Llpc;

namespace SPIRV {
extern const char *MetaNameSpirvOp;
} // namespace SPIRV

namespace RtName {
const char *LdsUsage = "LdsUsage";
const char *PrevRayQueryObj = "PrevRayQueryObj";
const char *RayQueryObjGen = "RayQueryObjGen";
static const char *FetchTrianglePositionFromRayQuery = "FetchTrianglePositionFromRayQuery";
} // namespace RtName

// Enum for the RayDesc
namespace RayDescParams {
enum : unsigned {
  Origin = 0, // 0, Origin
  TMin,       // 1, T Min
  Direction,  // 2, Direction
  TMax        // 3, T Max
};
} // namespace RayDescParams

// Enum for the RaySystem
namespace RaySystemParams {
enum : unsigned {
  CurrNodePtr = 0,      // 0, Current Node pointer
  RayTCurrent,          // 1, T Current
  InstanceNodePtr,      // 2, Instance node pointer
  InstanceContribution, // 3, Instance contribution
  GeometryIndex,        // 4, Geometry index
  PrimitiveIndex,       // 5, Primitive index
  Barycentrics,         // 6, Barycentrics
  FrontFace,            // 7, Front face
  Origin,               // 8, Ray origin
  Direction             // 9, Ray direction
};
} // namespace RaySystemParams

namespace RayQueryParams {
enum : unsigned {
  BvhLo = 0,                       // 0, Acceleration structure address low bits
  BvhHi,                           // 1, Acceleration structure address high bits
  TopLevelBvhLo,                   // 2, Top level AS address low bits
  TopLevelBvhHi,                   // 3, Top level AS address high bits
  StackPtr,                        // 4, Stack pointer
  StackPtrTop,                     // 5, Top Stack pointer
  StackNumEntries,                 // 6, Stack number entries
  InstNodePtr,                     // 7, Instance node pointer
  CurrNodePtr,                     // 8, Current node pointer
  InstanceHitContributionAndFlags, // 9, Instance hit contribution and flags
  PrevNodePtr,                     // 10, Last node pointer
  IsGoingDown,                     // 11, Is going down
  LastInstanceNode,                // 12, Last instance node
  RayDesc,                         // 13, RayDesc structure
  RayTMin,                         // 14, T min
  RayFlags,                        // 15, Ray flags
  InstanceInclusionMask,           // 16, Instance inclusion mask
  CandidateType,                   // 17, Candidate type
  Candidate,                       // 18, Candidate system info
  CommittedStatus,                 // 19, Committed status
  Committed,                       // 20, Committed system info
  CurrNodePtr2,                    // 21, currNodePtr2
  NumRayBoxTest,                   // 22, numRayBoxTest;
  NumRayTriangleTest,              // 23, numRayTriangleTest;
  NumIterations,                   // 24, numIterations;
  MaxStackDepth,                   // 25, maxStackDepth;
  Clocks,                          // 26, clocks;
  NumCandidateHits,                // 27, numCandidateHits;
  UnstanceIntersections,           // 28, instanceIntersections;
  RayQueryObj                      // 29, Internal ray query object handle
};
} // namespace RayQueryParams

// Enums for the committed status
namespace CommittedStatus {
enum : unsigned {
  Nothing = 0,           // Nothing hit
  TriangleHit,           // Triangle hit
  ProceduralPrimitiveHit // Procedural hit
};
} // namespace CommittedStatus

// Ray query candidate intersection type values
namespace RayQueryCandidateIntersection {
enum : unsigned {
  NonOpaqueTriangle = 0, // Candidate Intersection Non Opaque Triangle
  Aabb,                  // Candidate Intersection Aabb
  NonOpaqueAabb,         // Candidate Intersection Non Opaque Aabb
  NoDuplicateAnyHitAabb  // Candidate Intersection No Duplicate Any Hit Aabb
};
} // namespace RayQueryCandidateIntersection

// Ray query committed intersection type values
namespace RayQueryCommittedIntersection {
enum : unsigned {
  None = 0,  // Committed Intersection None
  Triangle,  // Committed Intersection Triangle
  Generated, // Committed Intersection Generated
};
} // namespace RayQueryCommittedIntersection

namespace Llpc {

// =====================================================================================================================
// Get RayDesc Type

// @param builder : The builder to construct LLVM IR IR
Type *getRayDescTy(lgc::Builder *builder) {

  //  struct RayDesc {
  //   vec3 origin;
  //   float tMin;
  //   vec3 direction;
  //   float tMax;
  // };

  LLVMContext &context = builder->getContext();
  auto floatx3Ty = FixedVectorType::get(builder->getFloatTy(), 3);
  Type *rayDescTys[] = {
      floatx3Ty,             // origin
      builder->getFloatTy(), // tMin
      floatx3Ty,             // direction
      builder->getFloatTy(), // tMax
  };
  StructType *rayDescTy = StructType::get(context, rayDescTys, false);
  return rayDescTy;
}

// =============================================================================
// Get RayQueryInternal type
//
// @param build : The builder to construct LLVM IR
Type *getRayQueryInternalTy(lgc::Builder *builder) {
  auto rayDescTy = getRayDescTy(builder);
  LLVMContext &context = builder->getContext();

  // struct RaySystemData {
  //   uint nodeIndex;
  //   float rayTCurrent;
  //   uint instanceNodePtr;
  //   uint instanceContribution;
  //   uint geometryIndex;
  //   uint primitiveIndex;
  //   vec2 barycentrics;
  //   uint frontFace;
  //   vec3 origin;
  //   vec3 direction;
  // };

  auto floatx2Ty = FixedVectorType::get(builder->getFloatTy(), 2);
  auto floatx3Ty = FixedVectorType::get(builder->getFloatTy(), 3);
  Type *raySystemDatas[] = {
      builder->getInt32Ty(), // 0, nodeIndex
      builder->getFloatTy(), // 1, rayTCurrent
      builder->getInt32Ty(), // 2, instanceNodePtr
      builder->getInt32Ty(), // 3, instanceContribution
      builder->getInt32Ty(), // 4, geometryIndex;
      builder->getInt32Ty(), // 5, primitiveIndex;
      floatx2Ty,             // 6, barycentrics;
      builder->getInt32Ty(), // 7, frontFace;
      floatx3Ty,             // 8, origin;
      floatx3Ty,             // 9, direction;
  };
  auto raySystemDataTy = StructType::get(context, raySystemDatas, false);

  // struct RayQueryInternal {
  //   uint bvhLo;
  //   uint bvhHi;
  //   uint topLevelBvhLo;
  //   uint topLevelBvhHi;
  //   uint stackPtr;
  //   uint stackPtrTop;
  //   uint stackNumEntries;
  //   uint instNodePtr;
  //   uint currNodePtr;
  //   uint instanceHitContributionAndFlags;
  //   uint prevNodePtr;
  //   uint isGoingDown;
  //   uint lastInstanceNode;
  //   RayDesc rayDesc;
  //   float rayTMin;
  //   uint rayFlags;
  //   uint instanceInclusionMask;
  //   uint candidateType;
  //   RaySystemData candidate;
  //   uint committedStatus;
  //   RaySystemData committed;
  //   uint numRayBoxTest;
  //   uint numRayTriangleTest;
  //   uint numIterations;
  //   uint maxStackDepth;
  //   uint clocks;
  //   uint numCandidateHits;
  //   uint instanceIntersections;
  //   uint rayqueryObj;
  // };

  Type *rayQueryInternalTys[] = {
      builder->getInt32Ty(), // 0, bvhLo,
      builder->getInt32Ty(), // 1, bvhHi,
      builder->getInt32Ty(), // 2, topLevelBvhLo,
      builder->getInt32Ty(), // 3, topLevelBvhHi,
      builder->getInt32Ty(), // 4, stackPtr,
      builder->getInt32Ty(), // 5, stackPtrTop,
      builder->getInt32Ty(), // 6, stackNumEntries,
      builder->getInt32Ty(), // 7, instNodePtr,
      builder->getInt32Ty(), // 8, currNodePtr,
      builder->getInt32Ty(), // 9, instanceHitContributionAndFlags,
      builder->getInt32Ty(), // 10, prevNodePtr,
      builder->getInt32Ty(), // 11, isGoingDown,
      builder->getInt32Ty(), // 12, lastInstanceNode,
      rayDescTy,             // 13, rayDesc,
      builder->getFloatTy(), // 14, rayTMin,
      builder->getInt32Ty(), // 15, rayFlags,
      builder->getInt32Ty(), // 16, instanceInclusionMask,
      builder->getInt32Ty(), // 17, candidateType;
      raySystemDataTy,       // 18, candidate;
      builder->getInt32Ty(), // 19, committedStatus;
      raySystemDataTy,       // 20, committed;
      builder->getInt32Ty(), // 21, currNodePtr2
      builder->getInt32Ty(), // 22, numRayBoxTest;
      builder->getInt32Ty(), // 23, numRayTriangleTest;
      builder->getInt32Ty(), // 24, numIterations;
      builder->getInt32Ty(), // 25, maxStackDepth;
      builder->getInt32Ty(), // 26, clocks;
      builder->getInt32Ty(), // 27, numCandidateHits;
      builder->getInt32Ty(), // 28, instanceIntersections;
      builder->getInt32Ty(), // 29, rayqueryObj
  };
  return StructType::get(context, rayQueryInternalTys, false);
}

// =====================================================================================================================
SpirvLowerRayQuery::SpirvLowerRayQuery() : SpirvLowerRayQuery(false) {
}

// =====================================================================================================================
SpirvLowerRayQuery::SpirvLowerRayQuery(bool rayQueryLibrary)
    : m_rayQueryLibrary(rayQueryLibrary), m_spirvOpMetaKindId(0), m_prevRayQueryObj(nullptr), m_rayQueryObjGen(nullptr),
      m_nextTraceRayId(0) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerRayQuery::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerRayQuery::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-ray-query\n");
  SpirvLower::init(&module);
  createGlobalRayQueryObj();
  createGlobalLdsUsage();
  if (!m_rayQueryLibrary) {
    Instruction *insertPos = &*(m_entryPoint->begin()->getFirstNonPHIOrDbgOrAlloca());
    m_builder->SetInsertPoint(insertPos);
    initGlobalVariable();
    m_spirvOpMetaKindId = m_context->getMDKindID(MetaNameSpirvOp);
    for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
      Function *func = &*funcIt++;
      processShaderFunction(func, getFuncOpcode(func));
    }
  }
  return true;
}

// =====================================================================================================================
// Process RayQuery OpRayQueryInitializeKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryInitializeKHR>(Function *func) {
  //  void TraceRayInlineAmdInternal(
  //      inout RayQueryInternal rayQuery,
  //      in    uint             accelStructLo,
  //      in    uint             accelStructHi,
  //      in    uint             constRayFlags,
  //      in    uint             rayFlags,
  //      in    uint             instanceMask,
  //      in    RayDesc          rayDesc,
  //      in    uint             rayId)
  //
  //  void rayQueryInitializeEXT(
  //      rayQueryEXT q -> rayQuery,
  //      accelerationStructureEXT topLevel,
  //      uint rFlags,
  //      uint cullMask,
  //      vec3 origin,
  //      float tMin,
  //      vec3 direction,
  //      float tMax)
  //  {
  //      rayQuery = q
  //      accelStructLo = topLevel.x
  //      accelStructHi = topLevel.y
  //      instanceMask = cullMask
  //      rayDesc.Origin = origin
  //      rayDesc.Direction = direction
  //      rayDesc.TMin = tMin
  //      rayDesc.TMax = tMax
  //      constRayFlags = 0
  //      rayFlags = rFlags
  //      rayId = 0
  //      call TraceRayInlineAmdInternal
  //  }

  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *zero = m_builder->getInt32(0);
  Type *rayDescTy = getRayDescTy(m_builder);
  auto int32x3Ty = FixedVectorType::get(m_builder->getInt32Ty(), 3);

  // traceRaysInline argument types
  Type *funcArgTys[] = {
      nullptr,                 // 0, Ray query type
      m_builder->getInt32Ty(), // 1, Scene Addr low
      m_builder->getInt32Ty(), // 2, Scene Addr high
      m_builder->getInt32Ty(), // 3, Const ray flags
      m_builder->getInt32Ty(), // 4, Ray flags
      m_builder->getInt32Ty(), // 5, InstanceMask
      rayDescTy,               // 6, Ray desc
      int32x3Ty,               // 7, DispatchRay ID
  };
  SmallVector<Value *, 8> traceRaysArgs(sizeof(funcArgTys) / sizeof(funcArgTys[0]));
  auto argIt = func->arg_begin();
  traceRaysArgs[0] = argIt++;
  for (size_t i = 1; i < traceRaysArgs.size(); ++i)
    traceRaysArgs[i] = m_builder->CreateAlloca(funcArgTys[i], SPIRAS_Private);

  // NOTE: Initialize rayQuery.committed to zero, as a workaround for CTS that uses it without committed intersection.
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  Value *committedAddr = m_builder->CreateConstGEP2_32(rayQueryTy, traceRaysArgs[0], 0, RayQueryParams::Committed);
  auto committedTy = rayQueryTy->getStructElementType(RayQueryParams::Committed);
  m_builder->CreateStore(ConstantAggregateZero::get(committedTy), committedAddr);

  // Setup the rayQuery Object ID
  Value *rayQueryObjId = m_builder->CreateLoad(m_builder->getInt32Ty(), m_rayQueryObjGen);
  Value *rayQueryObjAddr = m_builder->CreateConstGEP2_32(rayQueryTy, traceRaysArgs[0], 0, RayQueryParams::RayQueryObj);
  m_builder->CreateStore(rayQueryObjId, rayQueryObjAddr);
  m_builder->CreateStore(m_builder->CreateAdd(rayQueryObjId, m_builder->getInt32(1)), m_rayQueryObjGen);

  // 1, Scene Addr low  2, Scene Addr high
  Value *arg = argIt++;
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

  m_builder->CreateStore(sceneAddLow, traceRaysArgs[1]);
  m_builder->CreateStore(sceneAddHigh, traceRaysArgs[2]);
  // 3, Const ray flags
  m_builder->CreateStore(zero, traceRaysArgs[3]);
  // 4, Ray flags
  arg = argIt++;
  m_builder->CreateStore(arg, traceRaysArgs[4]);
  // 5, instance mask
  arg = argIt++;
  m_builder->CreateStore(arg, traceRaysArgs[5]);
  // 6, RayDesc
  Value *rayDesc = PoisonValue::get(rayDescTy);
  // Insert values Origin,TMin,Direction,TMax to the RayDesc
  // Origin
  arg = argIt++;
  rayDesc = m_builder->CreateInsertValue(rayDesc, arg, 0u);
  // TMin
  arg = argIt++;
  rayDesc = m_builder->CreateInsertValue(rayDesc, arg, 1u);
  // Direction
  arg = argIt++;
  rayDesc = m_builder->CreateInsertValue(rayDesc, arg, 2u);
  // TMax
  arg = argIt++;
  rayDesc = m_builder->CreateInsertValue(rayDesc, arg, 3u);
  m_builder->CreateStore(rayDesc, traceRaysArgs[6]);
  // 7, Dispatch Id
  m_builder->CreateStore(getDispatchId(), traceRaysArgs[7]);

  if (m_context->getPipelineContext()->getRayTracingState()->enableRayTracingCounters)
    generateTraceRayStaticId();

  StringRef rayQueryInitialize =
      m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_TRACE_RAY_INLINE);
  m_builder->CreateNamedCall(rayQueryInitialize, m_builder->getVoidTy(), traceRaysArgs,
                             {Attribute::NoUnwind, Attribute::AlwaysInline});
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Get Dispatch Id
//
//
Value *SpirvLowerRayQuery::getDispatchId() {
  Value *zero = m_builder->getInt32(0);
  Value *dispatchId = nullptr;
  lgc::InOutInfo inputInfo = {};
  // Local thread ID for graphics shader Stage, global thread ID for compute/raytracing shader stage
  if (m_shaderStage < ShaderStageCompute) {
    auto subThreadId =
        m_builder->CreateReadBuiltInInput(lgc::BuiltInSubgroupLocalInvocationId, inputInfo, nullptr, nullptr, "");
    dispatchId = PoisonValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 3));
    dispatchId = m_builder->CreateInsertElement(dispatchId, subThreadId, uint64_t(0));
    dispatchId = m_builder->CreateInsertElement(dispatchId, zero, 1);
    dispatchId = m_builder->CreateInsertElement(dispatchId, zero, 2);
  } else
    dispatchId = m_builder->CreateReadBuiltInInput(lgc::BuiltInGlobalInvocationId, inputInfo, nullptr, nullptr, "");

  return dispatchId;
}

void SpirvLowerRayQuery::createRayQueryProceedFunc(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  auto int32x3Ty = FixedVectorType::get(m_builder->getInt32Ty(), 3);
  Value *constRayFlags = m_builder->CreateAlloca(m_builder->getInt32Ty(), SPIRAS_Private);
  Value *threadId = m_builder->CreateAlloca(int32x3Ty, SPIRAS_Private);

  Value *zero = m_builder->getInt32(0);
  Value *rayQuery = func->arg_begin();
  Type *rayQueryEltTy = getRayQueryInternalTy(m_builder);

  // Initialize ldsUsage for the shader stage
  if (stageNotSupportLds(m_shaderStage))
    m_builder->CreateStore(m_builder->getInt32(0), m_ldsUsage);
  else
    m_builder->CreateStore(m_builder->getInt32(1), m_ldsUsage);

  // Get RayQueryObj for rayquery object comparison
  Value *rayQueryObj = m_builder->CreateLoad(
      m_builder->getInt32Ty(), m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::RayQueryObj));
  Value *notEqual =
      m_builder->CreateICmpNE(rayQueryObj, m_builder->CreateLoad(m_builder->getInt32Ty(), m_prevRayQueryObj));

  Value *stackNumEntriesAddr =
      m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::StackNumEntries);

  Value *stackNumEntries = m_builder->CreateLoad(m_builder->getInt32Ty(), stackNumEntriesAddr);
  stackNumEntries = m_builder->CreateSelect(notEqual, zero, stackNumEntries);
  m_builder->CreateStore(stackNumEntries, stackNumEntriesAddr);

  m_builder->CreateStore(rayQueryObj, m_prevRayQueryObj);

  m_builder->CreateStore(zero, constRayFlags);

  m_builder->CreateStore(getDispatchId(), threadId);

  Value *result = m_builder->CreateNamedCall(
      m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_RAY_QUERY_PROCEED),
      func->getReturnType(), {rayQuery, constRayFlags, threadId}, {Attribute::NoUnwind, Attribute::AlwaysInline});

  m_builder->CreateStore(m_builder->getInt32(1), m_ldsUsage);
  m_builder->CreateRet(result);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryProceedKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryProceedKHR>(Function *func) {

  // bool RayQueryProceedAmdInternal(
  //     inout RayQueryInternal rayQuery,
  //     in    uint             constRayFlags,
  //     in    uint3            dispatchThreadId)

  // bool rayQueryProceedEXT(rayQueryEXT q -> rayQuery)
  // {
  //     if (stageNotSupportLds(stage))
  //         ldsUsage = 0;
  //     else
  //         ldsUsage = 1;
  //     if (rayQuery != prevRayQueryObj)
  //         rayQuery.stackNumEntries = 0
  //     prevRayQueryObj = rayQuery
  //     constRayFlags = 0
  //     rayId = 0
  //     bool proceed = call RayQueryProceedAmdInternal
  //     ldsUsage = 1;
  //     return proceed;
  // }

  createRayQueryProceedFunc(func);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionTypeKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionTypeKHR>(Function *func) {
  // uint rayQueryGetIntersectionTypeEXT(rayQueryEXT q -> rayQuery, bool committed)
  // {
  //     if (committed)
  //         return q.committedStatus
  //     else
  //         return q.candidateType (return Aabb if q.candidateType is Aabb/NonOpaqueAabb/NoDuplicateAnyHitAabb)
  // }
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  Value *committed = func->arg_begin() + 1;
  committed = m_builder->CreateTrunc(committed, m_builder->getInt1Ty());
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  rayQuery = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto candidateTy = m_builder->CreateExtractValue(rayQuery, RayQueryParams::CandidateType);
  auto committedStatus = m_builder->CreateExtractValue(rayQuery, RayQueryParams::CommittedStatus);
  Value *result = m_builder->CreateSelect(committed, committedStatus, candidateTy);

  // if (!committed && (q.candidateType))
  //     result = Aabb
  Value *compare = m_builder->CreateICmpUGE(result, m_builder->getInt32(RayQueryCandidateIntersection::Aabb));
  compare = m_builder->CreateAnd(compare, m_builder->CreateNot(committed));
  result = m_builder->CreateSelect(compare, m_builder->getInt32(RayQueryCandidateIntersection::Aabb), result);

  m_builder->CreateRet(result);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionTypeKHR
//
// @param func : The function to create
// @param raySystem : raySystem Parameter
Value *SpirvLowerRayQuery::createIntersectSystemValue(Function *func, unsigned raySystem) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  Value *intersect = func->arg_begin() + 1;
  intersect = m_builder->CreateTrunc(intersect, m_builder->getInt1Ty());
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  rayQuery = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto candidate = m_builder->CreateExtractValue(rayQuery, RayQueryParams::Candidate);
  auto committed = m_builder->CreateExtractValue(rayQuery, RayQueryParams::Committed);
  auto candidateVal = m_builder->CreateExtractValue(candidate, raySystem);
  auto committedVal = m_builder->CreateExtractValue(committed, raySystem);
  return m_builder->CreateSelect(intersect, committedVal, candidateVal);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionBarycentricsKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionBarycentricsKHR>(Function *func) {
  m_builder->CreateRet(createIntersectSystemValue(func, RaySystemParams::Barycentrics));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionTKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionTKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);
  Value *intersect = func->arg_begin() + 1;
  Value *rayTMinAddr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::RayTMin);
  auto minTVal = m_builder->CreateLoad(m_builder->getFloatTy(), rayTMinAddr);

  intersect = m_builder->CreateTrunc(intersect, m_builder->getInt1Ty());
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  rayQuery = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto candidate = m_builder->CreateExtractValue(rayQuery, RayQueryParams::Candidate);
  auto committed = m_builder->CreateExtractValue(rayQuery, RayQueryParams::Committed);
  auto candidateVal = m_builder->CreateExtractValue(candidate, RaySystemParams::RayTCurrent);
  auto committedVal = m_builder->CreateExtractValue(committed, RaySystemParams::RayTCurrent);
  auto lengthVal = m_builder->CreateSelect(intersect, committedVal, candidateVal);

  m_builder->CreateRet(m_builder->CreateFAdd(lengthVal, minTVal));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionInstanceCustomIndexKHR
//
// @param func : The function to create
template <>
void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionInstanceCustomIndexKHR>(Function *func) {
  // Read instance node pointer
  auto instanceNodePtr = createIntersectSystemValue(func, RaySystemParams::InstanceNodePtr);

  // Extract instance node address from instance node pointer
  Value *rayQuery = func->arg_begin();
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  rayQuery = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto instanceNodeAddr = createGetInstanceNodeAddr(instanceNodePtr, rayQuery);

  // Load instance index from instance node address
  auto instanceIndex = createLoadInstanceId(instanceNodeAddr);

  m_builder->CreateRet(instanceIndex);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionInstanceIdKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionInstanceIdKHR>(Function *func) {
  // Read instance node pointer
  auto instanceNodePtr = createIntersectSystemValue(func, RaySystemParams::InstanceNodePtr);

  // Extract instance node address from instance node pointer
  Value *rayQuery = func->arg_begin();
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  rayQuery = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto instanceNodeAddr = createGetInstanceNodeAddr(instanceNodePtr, rayQuery);

  // Load instance index from instance node address
  auto instanceId = createLoadInstanceIndex(instanceNodeAddr);

  m_builder->CreateRet(instanceId);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR
//
// @param func : The function to create
template <>
void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR>(
    Function *func) {
  m_builder->CreateRet(createIntersectSystemValue(func, RaySystemParams::InstanceContribution));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionGeometryIndexKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionGeometryIndexKHR>(Function *func) {
  m_builder->CreateRet(createIntersectSystemValue(func, RaySystemParams::GeometryIndex));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionPrimitiveIndexKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionPrimitiveIndexKHR>(Function *func) {
  m_builder->CreateRet(createIntersectSystemValue(func, RaySystemParams::PrimitiveIndex));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionObjectRayDirectionKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionFrontFaceKHR>(Function *func) {
  Value *frontFace = createIntersectSystemValue(func, RaySystemParams::FrontFace);
  frontFace = m_builder->CreateTrunc(frontFace, m_builder->getInt1Ty());
  m_builder->CreateRet(frontFace);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionObjectRayDirectionKHR
//
// @param func : The function to create
template <>
void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionObjectRayDirectionKHR>(Function *func) {
  m_builder->CreateRet(createIntersectSystemValue(func, RaySystemParams::Direction));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionObjectRayOriginKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionObjectRayOriginKHR>(Function *func) {
  m_builder->CreateRet(createIntersectSystemValue(func, RaySystemParams::Origin));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryTerminateKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryTerminateKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);

  if (m_context->getGfxIpVersion().major >= 11) {
    // Navi3x and beyond, use rayQuery.currentNodePtr == TERMINAL_NODE to determine Terminate()

    // TERMINAL_NODE defined in GPURT is 0xFFFFFFFE
    static const unsigned RayQueryTerminalNode = 0xFFFFFFFE;

    Value *currNodeAddr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::CurrNodePtr);
    m_builder->CreateStore(m_builder->getInt32(RayQueryTerminalNode), currNodeAddr);
  } else {
    // Navi2x, use the following combination to determine Terminate()
    //  rayQuery.nodeIndex = 0xFFFFFFFF // invalid index
    //  rayQuery.numStackEntries = 0;
    //  rayQuery.stackPtr = ThreadIdInGroup()

    Value *currNodeAddr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::CurrNodePtr);
    m_builder->CreateStore(m_builder->getInt32(InvalidValue), currNodeAddr);

    Value *stackNumEntries = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::StackNumEntries);
    m_builder->CreateStore(m_builder->getInt32(0), stackNumEntries);

    Value *stackPtr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::StackPtr);
    m_builder->CreateStore(getThreadIdInGroup(), stackPtr);
  }
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGenerateIntersectionKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGenerateIntersectionKHR>(Function *func) {
  // Ray tracing patch function: rayQueryGenerateIntersectionEXT
  // void rayQueryGenerateIntersectionEXT(rayQuery, tHit)
  // {
  //      if (rayQuery.candidateType == Aabb ||
  //          rayQuery.candidateType == NonOpaqueAabb ||
  //          rayQuery.candidateType == NoDuplicateAnyHitAabb) -> rayQuery.candidateType >= Aabb
  //      {
  //          rayQuery.commit = rayQuery.candidate
  //          rayQuery.committedStatus = gl_RayQueryCommittedIntersectionGeneratedEXT
  //          rayQuery.committed.rayTCurrent = tHit - rayQuery.rayTMin
  //      }
  // }
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func);
  BasicBlock *setBlock = BasicBlock::Create(*m_context, ".set", func);
  BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);

  m_builder->SetInsertPoint(entryBlock);
  Value *rayQuery = func->arg_begin();
  Value *hitT = func->arg_begin() + 1;
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  Value *rayQueryVal = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto candidateTy = m_builder->CreateExtractValue(rayQueryVal, RayQueryParams::CandidateType);
  auto checkCandidate = m_builder->CreateICmpUGE(candidateTy, m_builder->getInt32(RayQueryCandidateIntersection::Aabb));
  m_builder->CreateCondBr(checkCandidate, setBlock, endBlock);

  // Set confirm block, set committed status and value
  m_builder->SetInsertPoint(setBlock);
  Value *candidate = m_builder->CreateExtractValue(rayQueryVal, RayQueryParams::Candidate);
  Value *zero = m_builder->getInt32(0);
  Value *storeAddr = m_builder->CreateConstGEP2_32(rayQueryTy, rayQuery, 0, RayQueryParams::Committed);
  m_builder->CreateStore(candidate, storeAddr);
  storeAddr = m_builder->CreateConstGEP2_32(rayQueryTy, rayQuery, 0, RayQueryParams::CommittedStatus);
  m_builder->CreateStore(m_builder->getInt32(RayQueryCommittedIntersection::Generated), storeAddr);
  storeAddr = m_builder->CreateGEP(
      rayQueryTy, rayQuery,
      {zero, m_builder->getInt32(RayQueryParams::Committed), m_builder->getInt32(RaySystemParams::RayTCurrent)});
  Value *rayTMinAddr = m_builder->CreateConstGEP2_32(rayQueryTy, rayQuery, 0, RayQueryParams::RayTMin);
  auto minTVal = m_builder->CreateLoad(m_builder->getFloatTy(), rayTMinAddr);
  // NOTE: rayTCurrent stored in rayQuery is relative to rayTMin, but tHit given by app is relative to ray origin.
  m_builder->CreateStore(m_builder->CreateFSub(hitT, minTVal), storeAddr);
  m_builder->CreateBr(endBlock);

  m_builder->SetInsertPoint(endBlock);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Process RayQuery OpRayQueryConfirmIntersectionKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryConfirmIntersectionKHR>(Function *func) {
  // Ray tracing patch function: rayQueryConfirmIntersectionEXT
  // void rayQueryConfirmIntersectionEXT(rayQuery)
  // {
  //      if (rayQuery.candidateType == gl_RayQueryCandidateIntersectionTriangleEXT)
  //      {
  //          rayQuery.committed = rayQuery.candidate;
  //          rayQuery.committedStatus = gl_RayQueryCommittedIntersectionTriangleEXT;
  //      }
  // }

  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func);
  BasicBlock *setBlock = BasicBlock::Create(*m_context, ".set", func);
  BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);

  m_builder->SetInsertPoint(entryBlock);
  Value *rayQuery = func->arg_begin();
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  Value *rayQueryVal = m_builder->CreateLoad(rayQueryTy, rayQuery);
  auto candidateTy = m_builder->CreateExtractValue(rayQueryVal, RayQueryParams::CandidateType);
  auto checkCandidate =
      m_builder->CreateICmpEQ(candidateTy, m_builder->getInt32(RayQueryCandidateIntersection::NonOpaqueTriangle));
  m_builder->CreateCondBr(checkCandidate, setBlock, endBlock);

  // Set confirm block, set committed status and value
  m_builder->SetInsertPoint(setBlock);
  Value *candidate = m_builder->CreateExtractValue(rayQueryVal, RayQueryParams::Candidate);
  Value *storeAddr = m_builder->CreateConstGEP2_32(rayQueryTy, rayQuery, 0, RayQueryParams::Committed);
  m_builder->CreateStore(candidate, storeAddr);
  storeAddr = m_builder->CreateConstGEP2_32(rayQueryTy, rayQuery, 0, RayQueryParams::CommittedStatus);
  m_builder->CreateStore(m_builder->getInt32(RayQueryCommittedIntersection::Triangle), storeAddr);
  m_builder->CreateBr(endBlock);

  m_builder->SetInsertPoint(endBlock);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetRayTMinKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetRayTMinKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);
  Value *rayTMinAddr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::RayTMin);

  m_builder->CreateRet(m_builder->CreateLoad(m_builder->getFloatTy(), rayTMinAddr));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetRayFlagsKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetRayFlagsKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);
  Value *rayFlagsAddr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::RayFlags);

  m_builder->CreateRet(m_builder->CreateLoad(m_builder->getInt32Ty(), rayFlagsAddr));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionCandidateAABBOpaqueKHR
//
// @param func : The function to create
template <>
void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionCandidateAABBOpaqueKHR>(Function *func) {
  // bool rayQueryGetIntersectionCandidateAABBOpaqueEXT(rayQueryEXT q)
  // {
  //      return (rayQuery.candidateType != NonOpaqueAabb);
  // }
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);
  Value *candidateTypeAddr = m_builder->CreateConstGEP2_32(rayQueryEltTy, rayQuery, 0, RayQueryParams::CandidateType);
  Value *candidateType = m_builder->CreateLoad(m_builder->getInt32Ty(), candidateTypeAddr);
  Value *ret =
      m_builder->CreateICmpNE(candidateType, m_builder->getInt32(RayQueryCandidateIntersection::NonOpaqueAabb));
  m_builder->CreateRet(ret);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetWorldRayDirectionKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetWorldRayDirectionKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  auto floatx3Ty = FixedVectorType::get(m_builder->getFloatTy(), 3);
  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);
  Value *dirAddr = m_builder->CreateGEP(rayQueryEltTy, rayQuery,
                                        {m_builder->getInt32(0), m_builder->getInt32(RayQueryParams::RayDesc),
                                         m_builder->getInt32(RayDescParams::Direction)});
  m_builder->CreateRet(m_builder->CreateLoad(floatx3Ty, dirAddr));
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetWorldRayOriginKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetWorldRayOriginKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, "", func);
  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryEltTy = getRayQueryInternalTy(m_builder);
  Value *originAddr = m_builder->CreateGEP(rayQueryEltTy, rayQuery,
                                           {m_builder->getInt32(0), m_builder->getInt32(RayQueryParams::RayDesc),
                                            m_builder->getInt32(RayDescParams::Origin)});
  auto floatx3Ty = FixedVectorType::get(m_builder->getFloatTy(), 3);
  m_builder->CreateRet(m_builder->CreateLoad(floatx3Ty, originAddr));
}

// =====================================================================================================================
// Get RayQuery intersection matrix
//
// @param builtInId : ID of the built-in variable
void SpirvLowerRayQuery::createIntersectMatrix(Function *func, unsigned builtInId) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func);
  BasicBlock *endBlock = BasicBlock::Create(*m_context, ".end", func);

  m_builder->SetInsertPoint(entryBlock);

  Value *rayQuery = func->arg_begin();
  auto rayQueryTy = getRayQueryInternalTy(m_builder);
  rayQuery = m_builder->CreateLoad(rayQueryTy, rayQuery);

  Value *intersect = func->arg_begin() + 1;
  Value *accelStructLo = m_builder->CreateExtractValue(rayQuery, RayQueryParams::TopLevelBvhLo);
  Value *accelStructHi = m_builder->CreateExtractValue(rayQuery, RayQueryParams::TopLevelBvhHi);

  Value *accelStruct = PoisonValue::get(FixedVectorType::get(Type::getInt32Ty(*m_context), 2));
  accelStruct = m_builder->CreateInsertElement(accelStruct, accelStructLo, uint64_t(0));
  accelStruct = m_builder->CreateInsertElement(accelStruct, accelStructHi, 1);

  intersect = m_builder->CreateTrunc(intersect, m_builder->getInt1Ty());
  auto candidate = m_builder->CreateExtractValue(rayQuery, RayQueryParams::Candidate);
  auto committed = m_builder->CreateExtractValue(rayQuery, RayQueryParams::Committed);
  auto candidateInstanceNodePtr = m_builder->CreateExtractValue(candidate, RaySystemParams::InstanceNodePtr);
  auto committedInstanceNodePtr = m_builder->CreateExtractValue(committed, RaySystemParams::InstanceNodePtr);
  Value *instanceNodePtr = m_builder->CreateSelect(intersect, committedInstanceNodePtr, candidateInstanceNodePtr);
  Value *instanceNodeAddr = createGetInstanceNodeAddr(instanceNodePtr, rayQuery);
  Value *instanceId = createLoadInstanceIndex(instanceNodeAddr);

  Instruction *brInst = m_builder->CreateBr(endBlock);
  Value *matrix = createTransformMatrix(builtInId, accelStruct, instanceId, brInst);
  m_builder->SetInsertPoint(endBlock);
  m_builder->CreateRet(matrix);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionWorldToObjectKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionWorldToObjectKHR>(Function *func) {
  createIntersectMatrix(func, BuiltInWorldToObjectKHR);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionObjectToWorldKHR
//
// @param func : The function to create
template <> void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionObjectToWorldKHR>(Function *func) {
  createIntersectMatrix(func, BuiltInObjectToWorldKHR);
}

// =====================================================================================================================
// Process RayQuery OpRayQueryGetIntersectionTriangleVertexPositionsKHR
//
// @param func : The function to create
template <>
void SpirvLowerRayQuery::createRayQueryFunc<OpRayQueryGetIntersectionTriangleVertexPositionsKHR>(Function *func) {
  func->addFnAttr(Attribute::AlwaysInline);
  BasicBlock *entryBlock = BasicBlock::Create(*m_context, ".entry", func);
  m_builder->SetInsertPoint(entryBlock);
  Value *rayQuery = func->arg_begin();
  Value *intersectVal = func->arg_begin() + 1;
  Value *intersectPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
  m_builder->CreateStore(intersectVal, intersectPtr);

  // Call {vec3, vec3, vec3} FetchTrianglePositionFromRayQuery(rayquery* rayquery, int* intersect)
  // return 3 triangle vertices
  auto floatx3Ty = FixedVectorType::get(m_builder->getFloatTy(), 3);
  auto triangleDataTy = StructType::get(*m_context, {floatx3Ty, floatx3Ty, floatx3Ty});
  auto triangleData =
      m_builder->CreateNamedCall(RtName::FetchTrianglePositionFromRayQuery, triangleDataTy, {rayQuery, intersectPtr},
                                 {Attribute::NoUnwind, Attribute::AlwaysInline});

  // Return type of OpRayQueryGetIntersectionTriangleVertexPositionsKHR is array of vec3 (vec3[3]).
  auto retType = ArrayType::get(floatx3Ty, 3);
  Value *ret = PoisonValue::get(retType);
  for (unsigned i = 0; i < 3; i++)
    ret = m_builder->CreateInsertValue(ret, m_builder->CreateExtractValue(triangleData, {i}), {i});
  m_builder->CreateRet(ret);
}

// =====================================================================================================================
// Process compute/graphics/raytracing shader RayQueryOp functions
//
// @param func : The function to create
void SpirvLowerRayQuery::processShaderFunction(Function *func, unsigned opcode) {
  switch (opcode) {
  case OpRayQueryInitializeKHR:
    return createRayQueryFunc<OpRayQueryInitializeKHR>(func);
  case OpRayQueryProceedKHR:
    return createRayQueryFunc<OpRayQueryProceedKHR>(func);
  case OpRayQueryGetIntersectionTypeKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionTypeKHR>(func);
  case OpRayQueryGetIntersectionBarycentricsKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionBarycentricsKHR>(func);
  case OpRayQueryGetIntersectionTKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionTKHR>(func);
  case OpRayQueryGetIntersectionInstanceCustomIndexKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionInstanceCustomIndexKHR>(func);
  case OpRayQueryGetIntersectionInstanceIdKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionInstanceIdKHR>(func);
  case OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR>(func);
  case OpRayQueryGetIntersectionGeometryIndexKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionGeometryIndexKHR>(func);
  case OpRayQueryGetIntersectionPrimitiveIndexKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionPrimitiveIndexKHR>(func);
  case OpRayQueryGetIntersectionFrontFaceKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionFrontFaceKHR>(func);
  case OpRayQueryGetIntersectionObjectRayDirectionKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionObjectRayDirectionKHR>(func);
  case OpRayQueryGetIntersectionObjectRayOriginKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionObjectRayOriginKHR>(func);
  case OpRayQueryTerminateKHR:
    return createRayQueryFunc<OpRayQueryTerminateKHR>(func);
  case OpRayQueryGenerateIntersectionKHR:
    return createRayQueryFunc<OpRayQueryGenerateIntersectionKHR>(func);
  case OpRayQueryConfirmIntersectionKHR:
    return createRayQueryFunc<OpRayQueryConfirmIntersectionKHR>(func);
  case OpRayQueryGetRayTMinKHR:
    return createRayQueryFunc<OpRayQueryGetRayTMinKHR>(func);
  case OpRayQueryGetRayFlagsKHR:
    return createRayQueryFunc<OpRayQueryGetRayFlagsKHR>(func);
  case OpRayQueryGetIntersectionCandidateAABBOpaqueKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionCandidateAABBOpaqueKHR>(func);
  case OpRayQueryGetWorldRayDirectionKHR:
    return createRayQueryFunc<OpRayQueryGetWorldRayDirectionKHR>(func);
  case OpRayQueryGetWorldRayOriginKHR:
    return createRayQueryFunc<OpRayQueryGetWorldRayOriginKHR>(func);
  case OpRayQueryGetIntersectionObjectToWorldKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionObjectToWorldKHR>(func);
  case OpRayQueryGetIntersectionWorldToObjectKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionWorldToObjectKHR>(func);
  case OpRayQueryGetIntersectionTriangleVertexPositionsKHR:
    return createRayQueryFunc<OpRayQueryGetIntersectionTriangleVertexPositionsKHR>(func);
  default:
    return;
  }
}

// =====================================================================================================================
// Create global variable for the LDS stack
void SpirvLowerRayQuery::createGlobalLdsUsage() {
  m_ldsUsage =
      new GlobalVariable(*m_module, Type::getInt32Ty(m_module->getContext()), true, GlobalValue::ExternalLinkage,
                         nullptr, RtName::LdsUsage, nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);

  m_ldsUsage->setAlignment(MaybeAlign(4));
}

// =====================================================================================================================
// Create global variable for the prevRayQueryObj
void SpirvLowerRayQuery::createGlobalRayQueryObj() {
  m_prevRayQueryObj =
      new GlobalVariable(*m_module, m_builder->getInt32Ty(), false, GlobalValue::ExternalLinkage, nullptr,
                         RtName::PrevRayQueryObj, nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
  m_prevRayQueryObj->setAlignment(MaybeAlign(4));

  m_rayQueryObjGen =
      new GlobalVariable(*m_module, m_builder->getInt32Ty(), false, GlobalValue::ExternalLinkage, nullptr,
                         RtName::RayQueryObjGen, nullptr, GlobalValue::NotThreadLocal, SPIRAS_Private);
  m_rayQueryObjGen->setAlignment(MaybeAlign(4));
}

// =====================================================================================================================
// Erase BasicBlocks from the Function
//
// @param func : Function
void SpirvLowerRayQuery::eraseFunctionBlocks(Function *func) {
  for (auto blockIt = func->begin(), blockEnd = func->end(); blockIt != blockEnd;) {
    BasicBlock *basicBlock = &*blockIt++;
    basicBlock->dropAllReferences();
    basicBlock->eraseFromParent();
  }
}

// =====================================================================================================================
// Get function opcode
//
// @param func : Function to get opcode
unsigned SpirvLowerRayQuery::getFuncOpcode(Function *func) {
  const MDNode *const funcMeta = func->getMetadata(m_spirvOpMetaKindId);
  if (!funcMeta)
    return 0;

  const ConstantAsMetadata *const metaConst = cast<ConstantAsMetadata>(funcMeta->getOperand(0));
  unsigned opcode = cast<ConstantInt>(metaConst->getValue())->getZExtValue();
  return opcode;
}

// =====================================================================================================================
// Create WorldToObject/ObjectToWorld Matrix by given instance ID
//
// @param builtInId : ID of the built-in variable
// @param accelStruct : Top accelerate structure
// @param instanceId : Instance ID
// @param insertPos : Where to insert instructions
Value *SpirvLowerRayQuery::createTransformMatrix(unsigned builtInId, Value *accelStruct, Value *instanceId,
                                                 Instruction *insertPos) {
  assert(builtInId == BuiltInWorldToObjectKHR || builtInId == BuiltInObjectToWorldKHR);
  m_builder->SetInsertPoint(insertPos);
  Value *zero = m_builder->getInt32(0);

  // offsetof(AccelStructHeader, dataOffsets) + offsetof(AccelStructOffsets, leafNodes)
  unsigned instanceNodeOffset = offsetof(AccelStructHeader, dataOffsets) + offsetof(ResultDataOffsets, leafNodes);
  Value *instanceNodeOffsetVal = m_builder->getInt32(instanceNodeOffset);

  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);

  instanceNodeOffsetVal =
      m_builder->CreateInsertElement(PoisonValue::get(int32x2Ty), instanceNodeOffsetVal, uint64_t(0));

  instanceNodeOffsetVal = m_builder->CreateInsertElement(instanceNodeOffsetVal, zero, 1);
  Value *instanceNodeOffsetAddr = m_builder->CreateAdd(accelStruct, instanceNodeOffsetVal);

  // Bitcast instanceNodeOffsetAddr to i64 integer
  instanceNodeOffsetAddr = m_builder->CreateBitCast(instanceNodeOffsetAddr, m_builder->getInt64Ty());
  Type *gpuAddrAsPtrTy = PointerType::get(*m_context, SPIRAS_Global);
  auto instNodeOffsetAddrAsPtr = m_builder->CreateIntToPtr(instanceNodeOffsetAddr, gpuAddrAsPtrTy);
  Value *baseInstOffset = m_builder->CreateConstGEP1_32(m_builder->getInt8Ty(), instNodeOffsetAddrAsPtr, 0);
  Type *baseInstOffsetTy = m_builder->getInt32Ty()->getPointerTo(SPIRAS_Global);

  // Load base instance offset from InstanceNodeOffsetAddr
  baseInstOffset = m_builder->CreateBitCast(baseInstOffset, baseInstOffsetTy);
  baseInstOffset = m_builder->CreateLoad(m_builder->getInt32Ty(), baseInstOffset);

  // Instance node includes the instance descriptor (64-bytes) followed by the extra instance node
  // data (64-bytes).
  Value *instanceNodeStrideShift = m_builder->getInt32(7);

  // Offset into the instance node
  instanceId = m_builder->CreateShl(instanceId, instanceNodeStrideShift);
  Value *matrixOffset = m_builder->CreateAdd(baseInstOffset, instanceId);

  if (builtInId == BuiltInObjectToWorldKHR) {
    // The ObjectToWorld transform is at a 80 byte offset within the extra data structure
    Value *transformOffset = m_builder->getInt32(80);
    matrixOffset = m_builder->CreateAdd(matrixOffset, transformOffset);
  }

  Value *vecMatrixOffset = PoisonValue::get(int32x2Ty);
  vecMatrixOffset = m_builder->CreateInsertElement(vecMatrixOffset, matrixOffset, uint64_t(0));
  vecMatrixOffset = m_builder->CreateInsertElement(vecMatrixOffset, zero, 1);
  Value *matrixAddr = m_builder->CreateAdd(accelStruct, vecMatrixOffset);

  return createLoadMatrixFromAddr(matrixAddr);
}

// =====================================================================================================================
// Init Ray Query Count
//
void SpirvLowerRayQuery::initGlobalVariable() {
  m_builder->CreateStore(m_builder->getInt32(InvalidValue), m_prevRayQueryObj);
  m_builder->CreateStore(m_builder->getInt32(0), m_rayQueryObjGen);
  m_builder->CreateStore(m_builder->getInt32(1), m_ldsUsage);
}

// =====================================================================================================================
// Generate a static ID for current Trace Ray call
//
unsigned SpirvLowerRayQuery::generateTraceRayStaticId() {
  Util::MetroHash64 hasher;
  hasher.Update(m_nextTraceRayId++);
  hasher.Update(m_module->getName());

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return MetroHash::compact32(&hash);
}

// =====================================================================================================================
// Shader stages not support LDS
//
// @param stage : Shader stage
bool SpirvLowerRayQuery::stageNotSupportLds(ShaderStage stage) {
  return stage == ShaderStageRayTracingAnyHit;
}

// =====================================================================================================================
// Create instructions to load instance index given the 64-bit instance node address at the current insert point
//
// @param instNodeAddr : 64-bit instance node address, in <2 x i32>
Value *SpirvLowerRayQuery::createLoadInstanceIndex(Value *instNodeAddr) {
  Value *zero = m_builder->getInt32(0);
  Type *gpuAddrAsPtrTy = PointerType::get(*m_context, SPIRAS_Global);
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);

  const unsigned instanceIndexOffset = offsetof(RayTracingInstanceNode, extra.instanceIndex);

  Value *instanceIndexOffsetVar = PoisonValue::get(int32x2Ty);
  instanceIndexOffsetVar =
      m_builder->CreateInsertElement(instanceIndexOffsetVar, m_builder->getInt32(instanceIndexOffset), uint64_t(0));
  instanceIndexOffsetVar = m_builder->CreateInsertElement(instanceIndexOffsetVar, zero, 1);
  Value *instanceIndexAddr = m_builder->CreateAdd(instNodeAddr, instanceIndexOffsetVar);

  instanceIndexAddr = m_builder->CreateBitCast(instanceIndexAddr, m_builder->getInt64Ty());
  auto instanceIndexAddrAsPtr = m_builder->CreateIntToPtr(instanceIndexAddr, gpuAddrAsPtrTy);
  auto loadValue = m_builder->CreateConstGEP1_32(m_builder->getInt8Ty(), instanceIndexAddrAsPtr, 0);
  loadValue = m_builder->CreateBitCast(loadValue, PointerType::get(*m_context, SPIRAS_Global));

  return m_builder->CreateLoad(m_builder->getInt32Ty(), loadValue);
}

// =====================================================================================================================
// Create instructions to get instance node address given the instance node pointer at the current insert point
//
// @param instNodePtr : Instance node pointer
// @param rayQuery : Ray query structure
Value *SpirvLowerRayQuery::createGetInstanceNodeAddr(Value *instNodePtr, Value *rayQuery) {
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Value *zero = m_builder->getInt32(0);

  Value *BvhAddrLo = m_builder->CreateExtractValue(rayQuery, RayQueryParams::TopLevelBvhLo);
  Value *BvhAddrHi = m_builder->CreateExtractValue(rayQuery, RayQueryParams::TopLevelBvhHi);

  Value *BvhAddr = PoisonValue::get(FixedVectorType::get(Type::getInt32Ty(*m_context), 2));
  BvhAddr = m_builder->CreateInsertElement(BvhAddr, BvhAddrLo, uint64_t(0));
  BvhAddr = m_builder->CreateInsertElement(BvhAddr, BvhAddrHi, 1);

  // Mask out the node offset
  auto nodeOffsetMask = m_builder->getInt32(0xFFFFFFF8u);
  // Shift left by 3 to make it 64B aligned address
  auto nodeOffsetShift = m_builder->getInt32(3u);

  auto nodeOffset = m_builder->CreateAnd(instNodePtr, nodeOffsetMask);
  nodeOffset = m_builder->CreateShl(nodeOffset, nodeOffsetShift);

  Value *instNodeOffset = PoisonValue::get(int32x2Ty);
  instNodeOffset = m_builder->CreateInsertElement(instNodeOffset, nodeOffset, uint64_t(0));
  instNodeOffset = m_builder->CreateInsertElement(instNodeOffset, zero, 1);

  auto instNodeAddr = m_builder->CreateAdd(BvhAddr, instNodeOffset);
  return instNodeAddr;
}

// =====================================================================================================================
// Create instructions to load instance ID given the 64-bit instance node address at the current insert point
//
// @param instNodeAddr : 64-bit instance node address, in <2 x i32>
Value *SpirvLowerRayQuery::createLoadInstanceId(Value *instNodeAddr) {
  Value *zero = m_builder->getInt32(0);
  Type *gpuAddrAsPtrTy = PointerType::get(*m_context, SPIRAS_Global);
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);

  const unsigned instanceIdOffset = offsetof(RayTracingInstanceNode, desc.InstanceID_and_Mask);

  Value *instanceIdOffsetVar = PoisonValue::get(int32x2Ty);
  instanceIdOffsetVar =
      m_builder->CreateInsertElement(instanceIdOffsetVar, m_builder->getInt32(instanceIdOffset), uint64_t(0));
  instanceIdOffsetVar = m_builder->CreateInsertElement(instanceIdOffsetVar, zero, 1);
  Value *instanceIdAddr = m_builder->CreateAdd(instNodeAddr, instanceIdOffsetVar);

  instanceIdAddr = m_builder->CreateBitCast(instanceIdAddr, m_builder->getInt64Ty());
  auto instanceIdAddrAsPtr = m_builder->CreateIntToPtr(instanceIdAddr, gpuAddrAsPtrTy);
  auto loadValue = m_builder->CreateConstGEP1_32(m_builder->getInt8Ty(), instanceIdAddrAsPtr, 0);
  loadValue = m_builder->CreateBitCast(loadValue, PointerType::get(*m_context, SPIRAS_Global));

  loadValue = m_builder->CreateLoad(m_builder->getInt32Ty(), loadValue);
  // Mask out the instance ID in lower 24 bits
  loadValue = m_builder->CreateAnd(loadValue, 0x00FFFFFFu);

  return loadValue;
}

// =====================================================================================================================
// Create instructions to load a 3x4 matrix from given address at the current insert point
//
// @param matrixAddr : Matrix address, which type is <2 x i32>
Value *SpirvLowerRayQuery::createLoadMatrixFromAddr(Value *matrixAddr) {
  Value *zero = m_builder->getInt32(0);
  Type *gpuAddrAsPtrTy = PointerType::get(*m_context, SPIRAS_Global);

  // Bitcast matrixAddr to i64 integer
  matrixAddr = m_builder->CreateBitCast(matrixAddr, m_builder->getInt64Ty());
  auto matrixAddrAsPtr = m_builder->CreateIntToPtr(matrixAddr, gpuAddrAsPtrTy);

  auto floatx3Ty = FixedVectorType::get(m_builder->getFloatTy(), 3);
  auto floatx4Ty = FixedVectorType::get(m_builder->getFloatTy(), 4);
  auto matrixTy = ArrayType::get(floatx3Ty, 4);

  auto loadPtrTy = floatx4Ty->getPointerTo(SPIRAS_Global);

  // Construct [4 x <3 x float>]
  Value *matrixRow[4] = {
      PoisonValue::get(floatx3Ty),
      PoisonValue::get(floatx3Ty),
      PoisonValue::get(floatx3Ty),
      PoisonValue::get(floatx3Ty),
  };

  // Matrix in the memory is [3 x <4 x float>], need to transform to [4 x <3 x float>]
  Value *loadOffset = zero;
  Value *stride = m_builder->getInt32(sizeof(float) * 4);
  // For Three columns
  for (unsigned i = 0; i < 3; ++i) {
    Value *loadValue = m_builder->CreateGEP(m_builder->getInt8Ty(), matrixAddrAsPtr, loadOffset);
    loadValue = m_builder->CreateBitCast(loadValue, loadPtrTy);
    auto rowValue = m_builder->CreateLoad(floatx4Ty, loadValue);
    for (unsigned j = 0; j < 4; ++j) {
      auto element = m_builder->CreateExtractElement(rowValue, uint64_t(j));
      matrixRow[j] = m_builder->CreateInsertElement(matrixRow[j], element, uint64_t(i));
    }
    loadOffset = m_builder->CreateAdd(loadOffset, stride);
  }
  Value *matrix = PoisonValue::get(matrixTy);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[0], 0);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[1], 1);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[2], 2);
  matrix = m_builder->CreateInsertValue(matrix, matrixRow[3], 3);

  return matrix;
}

// =====================================================================================================================
// Get thread ID in group.
Value *SpirvLowerRayQuery::getThreadIdInGroup() const {
  // Todo: for graphics shader, subgroupId * waveSize + subgroupLocalInvocationId()
  unsigned builtIn = m_context->getPipelineType() == PipelineType::Graphics ? BuiltInSubgroupLocalInvocationId
                                                                            : BuiltInLocalInvocationIndex;
  lgc::InOutInfo inputInfo = {};
  return m_builder->CreateReadBuiltInInput(static_cast<lgc::BuiltInKind>(builtIn), inputInfo, nullptr, nullptr, "");
}

} // namespace Llpc
