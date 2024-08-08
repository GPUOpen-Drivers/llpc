/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// LowerRayQuery.h : Pass to lower rayQuery ops by inlining GPURT functions.
// Typically used by running a pass class that derives from this one, setting m_staticFlags and setting up
// a GpurtContext as appropriate.

#pragma once

#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/PassManager.h"

namespace CompilerUtils {
class TypeLowering;
}

namespace llvm_dialects {
class Builder;
} // namespace llvm_dialects

namespace lgc {
class GpurtGetStaticFlagsOp;
class GpurtStackReadOp;
class GpurtStackWriteOp;
class GpurtLdsStackInitOp;

namespace rtq {
class InitializeOp;
class TerminateOp;
class ProceedOp;
class IntersectionCommitAabbOp;
class IntersectionCommitTriangleOp;
class IntersectionTypeOp;
class RayTMinOp;
class RayFlagsOp;
class IntersectionTOp;
class IntersectionInstanceIdOp;
class IntersectionInstanceIndexOp;
class IntersectionContributionToHitGroupIndexOp;
class IntersectionGeometryIndexOp;
class IntersectionPrimitiveIndexOp;
class IntersectionBarycentricsOp;
class IntersectionFrontFaceOp;
class IntersectionCandidateAabbOpaqueOp;
class IntersectionObjectRayDirectionOp;
class IntersectionObjectRayOriginOp;
class IntersectionWorldRayDirectionOp;
class IntersectionWorldRayOriginOp;
class IntersectionObjectToWorldOp;
class IntersectionWorldToObjectOp;
class IntersectionTriangleVertexPositionsOp;
class GepOpaqueOp;
} // namespace rtq

namespace rt {

class LowerRayQuery : public llvm::PassInfoMixin<LowerRayQuery> {
public:
  // Enum of GPURT functions. Order must match GpurtFuncNames array in LowerRayTracing.cpp.
  enum class GpurtFunc : unsigned {
    Abort,                               // _RayQuery_Abort
    Allocate,                            // _RayQuery_Allocate
    CandidateAabbOpaque,                 // _RayQuery_CandidateAabbOpaque
    CommitNonOpaqueTriangleHit,          // _RayQuery_CommitNonOpaqueTriangleHit
    CommitProceduralPrimitiveHit,        // _RayQuery_CommitProceduralPrimitiveHit
    EndInterleavedProceed,               // _RayQuery_EndInterleavedProceed
    FetchTrianglePositionFromRayQuery,   // FetchTrianglePositionFromRayQuery
    GeometryIndex,                       // _RayQuery_GeometryIndex
    GetObjId,                            // _RayQuery_GetObjId
    InstanceContributionToHitGroupIndex, // _RayQuery_InstanceContributionToHitGroupIndex
    InstanceID,                          // _RayQuery_InstanceID
    InstanceIndex,                       // _RayQuery_InstanceIndex
    IntersectionType,                    // _RayQuery_IntersectionType
    LongRayQueryProceed,                 // LongRayQueryProceedAMD
    ObjectRayDirection,                  // _RayQuery_ObjectRayDirection
    ObjectRayOrigin,                     // _RayQuery_ObjectRayOrigin
    ObjectToWorld4x3,                    // _RayQuery_ObjectToWorld4x3
    PrimitiveIndex,                      // _RayQuery_PrimitiveIndex
    RayFlags,                            // _RayQuery_RayFlags
    RayQueryProceed,                     // RayQueryProceed
    RayT,                                // _RayQuery_RayT
    RayTMin,                             // _RayQuery_RayTMin
    SetObjId,                            // _RayQuery_SetObjId
    TraceRayInline,                      // TraceRayInline
    TriangleBarycentrics,                // _RayQuery_TriangleBarycentrics
    TriangleFrontFace,                   // _RayQuery_TriangleFrontFace
    WorldRayDirection,                   // _RayQuery_WorldRayDirection
    WorldRayOrigin,                      // _RayQuery_WorldRayOrigin
    WorldToObject4x3,                    // _RayQuery_WorldToObject4x3
    Count
  };

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  llvm::Type *replaceRayQueryType(llvm::Type *ty);
  bool hasRtqOpaqueType(llvm::Type *ty);

protected:
  unsigned m_staticFlags = 0;

private:
  void visitInitializeOp(lgc::rtq::InitializeOp &inst);
  void visitTerminateOp(lgc::rtq::TerminateOp &inst);
  void visitProceedOp(lgc::rtq::ProceedOp &inst);
  void visitIntersectionCommitAabbOp(lgc::rtq::IntersectionCommitAabbOp &inst);
  void visitIntersectionCommitTriangleOp(lgc::rtq::IntersectionCommitTriangleOp &inst);
  void visitIntersectionTypeOp(lgc::rtq::IntersectionTypeOp &inst);
  void visitRayTMinOp(lgc::rtq::RayTMinOp &inst);
  void visitRayFlagsOp(lgc::rtq::RayFlagsOp &inst);
  void visitIntersectionTOp(lgc::rtq::IntersectionTOp &inst);
  void visitIntersectionInstanceIdOp(lgc::rtq::IntersectionInstanceIdOp &inst);
  void visitIntersectionInstanceIndexOp(lgc::rtq::IntersectionInstanceIndexOp &inst);
  void visitIntersectionContributionToHitGroupIndexOp(lgc::rtq::IntersectionContributionToHitGroupIndexOp &inst);
  void visitIntersectionGeometryIndexOp(lgc::rtq::IntersectionGeometryIndexOp &inst);
  void visitIntersectionPrimitiveIndexOp(lgc::rtq::IntersectionPrimitiveIndexOp &inst);
  void visitIntersectionBarycentricsOp(lgc::rtq::IntersectionBarycentricsOp &inst);
  void visitIntersectionFrontFaceOp(lgc::rtq::IntersectionFrontFaceOp &inst);
  void visitIntersectionCandidateAabbOpaqueOp(lgc::rtq::IntersectionCandidateAabbOpaqueOp &inst);
  void visitIntersectionObjectRayDirectionOp(lgc::rtq::IntersectionObjectRayDirectionOp &inst);
  void visitIntersectionObjectRayOriginOp(lgc::rtq::IntersectionObjectRayOriginOp &inst);
  void visitIntersectionWorldRayDirectionOp(lgc::rtq::IntersectionWorldRayDirectionOp &inst);
  void visitIntersectionWorldRayOriginOp(lgc::rtq::IntersectionWorldRayOriginOp &inst);
  void visitIntersectionObjectToWorldOp(lgc::rtq::IntersectionObjectToWorldOp &inst);
  void visitIntersectionWorldToObjectOp(lgc::rtq::IntersectionWorldToObjectOp &inst);
  void visitIntersectionTriangleVertexPositionsOp(lgc::rtq::IntersectionTriangleVertexPositionsOp &inst);
  void visitPtrToInt(llvm::PtrToIntInst &inst);
  void visitGepOpaqueOp(lgc::rtq::GepOpaqueOp &inst);
  void visitGetStaticFlagsOp(lgc::GpurtGetStaticFlagsOp &inst);
  void visitStackReadOp(lgc::GpurtStackReadOp &inst);
  void visitStackWriteOp(lgc::GpurtStackWriteOp &inst);
  void visitLdsStackInitOp(lgc::GpurtLdsStackInitOp &inst);

  void visitHitAccessor(GpurtFunc instType, llvm::Value *rayQuery, bool committed, llvm::CallBase *inst);
  void visitAccessor(GpurtFunc instType, llvm::Value *rayQuery, llvm::CallBase *inst);
  llvm_dialects::VisitorResult visitAlloca(llvm::AllocaInst &alloca);
  llvm_dialects::VisitorResult visitLifetimeIntrinsic(llvm::LifetimeIntrinsic &intrinc);
  void initializeAlloc(llvm::Function *func);
  void setRtqObjId(lgc::rtq::InitializeOp &inst, llvm::Value *rtq);
  llvm::Value *getRayQuery(llvm::Value *rayQuery);
  llvm::Function *getGpurtFunc(GpurtFunc gpurtFunc, bool optional = false);

  llvm::Module *m_gpurtModule = nullptr;
  llvm::Function **m_gpurtFuncs = nullptr;
  llvm::SmallVector<llvm::Value *> m_rtqAlloc;
  llvm::SmallSet<llvm::Function *, 4> m_funcsToLower;
  llvm_dialects::Builder *m_builder = nullptr;
  CompilerUtils::TypeLowering *m_typeLowering = nullptr;
  llvm::Type *m_rtqType = nullptr;
};

} // namespace rt
} // namespace lgc
