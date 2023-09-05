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
 * @file  llpcSpirvLowerRayTracing.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvLowerRayTracing
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLowerRayQuery.h"
#include "llvm/ADT/SmallSet.h"
#include <set>

namespace lgc::rt {
class AcceptHitAndEndSearchOp;
class IgnoreHitOp;
class CallCallableShaderOp;
class ReportHitOp;
class BaseTraceRayOp;
class TraceRayOp;
class DispatchRaysIndexOp;
class DispatchRaysDimensionsOp;
class WorldRayOriginOp;
class WorldRayDirectionOp;
class ObjectRayOriginOp;
class ObjectRayDirectionOp;
class RayTminOp;
class RayTcurrentOp;
class InstanceIndexOp;
class ObjectToWorldOp;
class WorldToObjectOp;
class HitKindOp;
class TriangleVertexPositionsOp;
class RayFlagsOp;
class GeometryIndexOp;
class InstanceIdOp;
class PrimitiveIndexOp;
class InstanceInclusionMaskOp;
class ShaderIndexOp;
class ShaderRecordBufferOp;
enum class RayTracingShaderStage;
} // namespace lgc::rt

namespace lgc {
class GpurtSetHitAttributesOp;
class GpurtGetHitAttributesOp;
class GpurtSetTraceParamsOp;
class GpurtCallClosestHitShaderOp;
class GpurtCallMissShaderOp;
class GpurtCallTriangleAnyHitShaderOp;
class GpurtCallIntersectionShaderOp;
class GpurtSetTriangleIntersectionAttributesOp;
class GpurtSetHitTriangleNodePointerOp;
class GpurtGetParentIdOp;
class GpurtSetParentIdOp;
} // namespace lgc

namespace Llpc {
// Enum for the ray start parameter
namespace TraceParam {
enum : unsigned {
  RayFlags,                   // Ray flags
  InstanceInclusionMask,      // Instance inclusion mask
  Origin,                     // Ray origin
  TMin,                       // T min
  Dir,                        // World ray direction
  TMax,                       // T max
  TCurrent,                   // Ray T current (begins as T max)
  Kind,                       // Intersection hit kind
  Status,                     // Hit status
  InstNodeAddrLo,             // Instance node address low part
  InstNodeAddrHi,             // Instance node address high part
  PrimitiveIndex,             // Primitive index
  DuplicateAnyHit,            // Indication of calling behavior on any hit shader,
  GeometryIndex,              // Geometry Index
  HitAttributes,              // Hit attributes
  ParentRayId,                // Ray ID of the parent TraceRay call
  HitTriangleVertexPositions, // Hit triangle vertex positions
  Payload,                    // Payload
  Count                       // Count of the trace attributes
};
}

// Enum for the OpTraceRayKHR parameter
namespace TraceRayParam {
enum : unsigned {
  AccelStruct = 0, // Acceleration structure
  RayFlags,        // Ray flags
  CullMask,        // Cull mask
  SbtOffset,       // Shader binding table offset
  SbtStride,       // Shader binding table stride
  MissIndex,       // Miss shader index
  RayOrigin,       // Ray origin
  RayTMin,         // Ray Tmin
  RayDir,          // Ray direction
  RayTMax,         // Ray Tmax
  Payload,         // Payload
  Paq,             // Payload access qualifier
};
} // namespace TraceRayParam

// Enum for the TraceRay library function input parameter
namespace TraceRayLibFuncParam {
enum : unsigned {
  AcceleStructLo = 0,                             // Acceleration structure address low bits
  AcceleStructHi,                                 // Acceleration structure address high bits
  RayTracingFlags,                                // Ray flags
  InstanceInclusionMask,                          // 8-bit instance mask
  RayContributionToHitGroupIndex,                 // Ray contribution to hit group
  MultiplierForGeometryContributionToShaderIndex, // Stride into shader table index
  MissShaderIndex,                                // Index of the miss shader
  OriginX,                                        // Ray origin X
  OriginY,                                        // Ray origin Y
  OriginZ,                                        // Ray origin Z
  TMin,                                           // T min
  DirX,                                           // World ray direction X
  DirY,                                           // World ray direction Y
  DirZ,                                           // World ray direction Z
  TMax,                                           // T max
  Count
};
} // namespace TraceRayLibFuncParam

// Enum for the shader table global variables
enum ShaderTable : unsigned {
  RayGenTableAddr = 0, // Ray generation table address
  MissTableAddr,       // Miss table address
  HitGroupTableAddr,   // Hit group table address
  CallableTableAddr,   // Callable table address
  MissTableStride,     // Miss table stride
  HitGroupTableStride, // Hit group table stride
  CallableTableStride, // Callable table stride
  LaunchSize,          // Launch size
  TraceRayGpuVirtAddr, // TraceRay GPU virtual address
  Count                // Count of shader table global variables
};

// Enumerates the ray-tracing Hit status enumeration
enum RayHitStatus : unsigned {
  Ignore = 0,             // Ignore hit
  Accept = 1,             // Accept hit
  AcceptAndEndSearch = 2, // Accept hit and end traversal
};

// =====================================================================================================================
// Represents the pass of SPIR-V lowering ray tracing.
class SpirvLowerRayTracing : public SpirvLowerRayQuery {
public:
  SpirvLowerRayTracing();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower SPIR-V RayTracing operations"; }

private:
  void createTraceParams(llvm::Function *func);
  void createRayGenEntryFunc();
  void processShaderRecordBuffer(llvm::GlobalVariable *global, llvm::Value *bufferDesc, llvm::Value *tableIndex,
                                 llvm::Instruction *insertPos);
  llvm::CallInst *createTraceRay();
  void createSetHitAttributes(llvm::Function *func, unsigned instArgsNum, unsigned traceParamsOffset);
  void createSetTraceParams(llvm::Function *func, unsigned instArgNum);
  void createAnyHitFunc(llvm::Value *shaderIdentifier, llvm::Value *shaderRecordIndex);
  void createCallShaderFunc(llvm::Function *func, ShaderStage stage, unsigned intersectId, llvm::Value *retVal,
                            unsigned traceParamsArgOffset);
  void createCallShader(llvm::Function *func, ShaderStage stage, unsigned intersectId, llvm::Value *shaderId,
                        llvm::Value *shaderRecordIndex, llvm::Value *inputResult, llvm::BasicBlock *entryBlock,
                        llvm::BasicBlock *endBlock, unsigned traceParamsArgOffset);
  void updateGlobalFromCallShaderFunc(llvm::Function *func, ShaderStage stage, unsigned traceParamsArgOffset);
  void createSetTriangleInsection(llvm::Function *func);
  void createShaderSelection(llvm::Function *func, llvm::BasicBlock *entryBlock, llvm::BasicBlock *endBlock,
                             llvm::Value *shaderId, unsigned intersectId, ShaderStage stage,
                             const llvm::SmallVector<llvm::Value *, 8> &args, llvm::Value *result,
                             llvm::Type *inResultTy);
  llvm::Value *loadShaderTableVariable(ShaderTable tableKind, llvm::Value *bufferDesc);
  llvm::Value *getShaderIdentifier(ShaderStage stage, llvm::Value *shaderRecordIndex, llvm::Value *bufferDesc);
  void createDbgInfo(llvm::Module &module, llvm::Function *func);
  void processTerminalFunc(llvm::Function *func, llvm::CallInst *inst, RayHitStatus hitStatus);
  void processPostReportIntersection(llvm::Function *func, llvm::Instruction *inst);
  void initTraceParamsTy(unsigned attributeSize);
  void initShaderBuiltIns();
  void inlineTraceRay(llvm::CallInst *callInst, ModuleAnalysisManager &analysisManager);
  llvm::Instruction *createEntryFunc(llvm::Function *func);
  void createEntryTerminator(llvm::Function *func);
  llvm::FunctionType *getShaderEntryFuncTy(ShaderStage stage);
  llvm::FunctionType *getCallableShaderEntryFuncTy();
  llvm::FunctionType *getTraceRayFuncTy();
  void createDispatchRaysInfoDesc();
  llvm::Instruction *createCallableShaderEntryFunc(llvm::Function *func);
  void createCallableShaderEntryTerminator(llvm::Function *func);
  void getFuncRets(llvm::Function *func, llvm::SmallVector<llvm::Instruction *, 4> &rets);
  llvm::SmallSet<unsigned, 4> getShaderExtraInputParams(ShaderStage stage);
  llvm::SmallSet<unsigned, 4> getShaderExtraRets(ShaderStage stage);
  llvm::Type *getShaderReturnTy(ShaderStage stage);
  void storeFunctionCallResult(ShaderStage stage, llvm::Value *result, llvm::Argument *traceIt);
  void initInputResult(ShaderStage stage, llvm::Value *payload, llvm::Value *traceParams[], llvm::Value *result,
                       llvm::Argument *traceIt);
  void cloneDbgInfoSubgrogram(llvm::Function *func, llvm::Function *newfunc);
  llvm::Value *createLoadRayTracingMatrix(unsigned builtInId);
  void createSetHitTriangleNodePointer(llvm::Function *func);
  llvm::Function *getOrCreateRemapCapturedVaToReplayVaFunc();

  void visitAcceptHitAndEndSearchOp(lgc::rt::AcceptHitAndEndSearchOp &inst);
  void visitIgnoreHitOp(lgc::rt::IgnoreHitOp &inst);
  void visitCallCallableShaderOp(lgc::rt::CallCallableShaderOp &inst);
  void visitReportHitOp(lgc::rt::ReportHitOp &inst);
  void visitTraceRayOp(lgc::rt::TraceRayOp &inst);
  void processTraceRayCall(lgc::rt::BaseTraceRayOp *inst);

  llvm::Function *createImplFunc(llvm::CallInst &inst, llvm::ArrayRef<Value *> args);

  void visitGetHitAttributes(lgc::GpurtGetHitAttributesOp &inst);
  void visitSetHitAttributes(lgc::GpurtSetHitAttributesOp &inst);
  void visitSetTraceParams(lgc::GpurtSetTraceParamsOp &inst);
  void visitCallClosestHitShader(lgc::GpurtCallClosestHitShaderOp &inst);
  void visitCallMissShader(lgc::GpurtCallMissShaderOp &inst);
  void visitCallTriangleAnyHitShader(lgc::GpurtCallTriangleAnyHitShaderOp &inst);
  void visitCallIntersectionShader(lgc::GpurtCallIntersectionShaderOp &inst);
  void visitSetTriangleIntersectionAttributes(lgc::GpurtSetTriangleIntersectionAttributesOp &inst);
  void visitSetHitTriangleNodePointer(lgc::GpurtSetHitTriangleNodePointerOp &inst);
  void visitGetParentId(lgc::GpurtGetParentIdOp &inst);
  void visitSetParentId(lgc::GpurtSetParentIdOp &inst);
  void visitDispatchRayIndex(lgc::rt::DispatchRaysIndexOp &inst);
  void visitDispatchRaysDimensionsOp(lgc::rt::DispatchRaysDimensionsOp &inst);
  void visitWorldRayOriginOp(lgc::rt::WorldRayOriginOp &inst);
  void visitWorldRayDirectionOp(lgc::rt::WorldRayDirectionOp &inst);
  void visitObjectRayOriginOp(lgc::rt::ObjectRayOriginOp &inst);
  void visitObjectRayDirectionOp(lgc::rt::ObjectRayDirectionOp &inst);
  void visitRayTminOp(lgc::rt::RayTminOp &inst);
  void visitRayTcurrentOp(lgc::rt::RayTcurrentOp &inst);
  void visitInstanceIndexOp(lgc::rt::InstanceIndexOp &inst);
  void visitObjectToWorldOp(lgc::rt::ObjectToWorldOp &inst);
  void visitWorldToObjectOp(lgc::rt::WorldToObjectOp &inst);
  void visitHitKindOp(lgc::rt::HitKindOp &inst);
  void visitTriangleVertexPositionsOp(lgc::rt::TriangleVertexPositionsOp &inst);
  void visitRayFlagsOp(lgc::rt::RayFlagsOp &inst);
  void visitGeometryIndexOp(lgc::rt::GeometryIndexOp &inst);
  void visitInstanceIdOp(lgc::rt::InstanceIdOp &inst);
  void visitPrimitiveIndexOp(lgc::rt::PrimitiveIndexOp &inst);
  void visitInstanceInclusionMaskOp(lgc::rt::InstanceInclusionMaskOp &inst);
  void visitShaderIndexOp(lgc::rt::ShaderIndexOp &inst);
  void visitShaderRecordBufferOp(lgc::rt::ShaderRecordBufferOp &inst);

  llvm::Value *createLoadInstNodeAddr();

  lgc::rt::RayTracingShaderStage mapStageToLgcRtShaderStage(ShaderStage stage);

  llvm::Value *m_traceParams[TraceParam::Count];           // Trace ray set parameters
  llvm::Value *m_worldToObjMatrix = nullptr;               // World to Object matrix
  llvm::AllocaInst *m_callableData = nullptr;              // Callable data variable for current callable shader
  std::set<unsigned, std::less<unsigned>> m_builtInParams; // Indirect max builtins;
  llvm::SmallVector<llvm::Type *, TraceParam::Count> m_traceParamsTys; // Trace Params types
  llvm::SmallVector<llvm::Instruction *> m_callsToLower;               // Call instruction to lower
  llvm::SmallSet<llvm::Function *, 4> m_funcsToLower;                  // Functions to lower
  llvm::Value *m_dispatchRaysInfoDesc = nullptr;                       // Descriptor of the DispatchRaysInfo
  llvm::Value *m_shaderRecordIndex = nullptr;                          // Variable sourced from entry function argument
  llvm::Instruction *m_insertPosPastInit = nullptr; // Insert position after initialization instructions (storing trace
                                                    // parameters, payload, callable data, etc.)
};

} // namespace Llpc
