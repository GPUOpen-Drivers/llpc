/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

namespace Llpc {
// Enum for the ray start parameter
namespace TraceParam {
enum : unsigned {
  RayFlags,              // Ray flags
  InstanceInclusionMask, // Instance inclusion mask
  Origin,                // Ray origin
  TMin,                  // T min
  Dir,                   // World ray direction
  TMax,                  // T max
  TCurrent,              // Ray T current (begins as T max)
  Kind,                  // Intersection hit kind
  Status,                // Hit status
  InstNodeAddrLo,        // Instance node address low part
  InstNodeAddrHi,        // Instance node address high part
  PrimitiveIndex,        // Primitive index
  DuplicateAnyHit,       // Indication of calling behavior on any hit shader,
  GeometryIndex,         // Geometry Index
  HitAttributes,         // Hit attributes
  Count                  // Count of the trace attributes
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
  PayloadType,     // PayloadType - This parameter is not specify in SPIRV API. This was added only to keep base type
                   //               of the Payload.
  TraceRayCount,   // OpTraceRay params count
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
  ShaderRecordIndex,   // Shader record index
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
  SpirvLowerRayTracing(bool rayQueryLibrary);
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  virtual bool runImpl(llvm::Module &module);

  static llvm::StringRef name() { return "Lower SPIR-V RayTracing operations"; }

private:
  void createGlobalTraceParams();
  llvm::GlobalVariable *createGlobalBuiltIn(unsigned builtInId);
  void createRayGenEntryFunc();
  void replaceGlobal(llvm::GlobalVariable *global, llvm::GlobalVariable *replacedGlobal);
  void processShaderRecordBuffer(llvm::GlobalVariable *global, llvm::Instruction *insertPos);
  void createTraceRay();
  void createSetHitAttributes(llvm::Function *func);
  void createSetTraceParams(llvm::Function *func);
  template <spv::Op> void createRayTracingFunc(llvm::Function *func, unsigned opcode);
  void createAnyHitFunc(llvm::Value *shaderIdentifier);
  void processLibraryFunction(llvm::Function *func);
  void createCallShaderFunc(llvm::Function *func, ShaderStage stage, unsigned intersectId, llvm::Value *retVal);
  void createCallShader(llvm::Function *func, ShaderStage stage, unsigned intersectId, llvm::Value *shaderId,
                        llvm::Value *inputResult, llvm::BasicBlock *entryBlock, llvm::BasicBlock *endBlock);
  void updateGlobalFromCallShaderFunc(llvm::Function *func, ShaderStage stage);
  void createSetTriangleInsection(llvm::Function *func);
  llvm::Value *processBuiltIn(unsigned builtInId, llvm::Instruction *insertPos);
  void createShaderSelection(llvm::Function *func, llvm::BasicBlock *entryBlock, llvm::BasicBlock *endBlock,
                             llvm::Value *shaderId, unsigned intersectId, ShaderStage stage,
                             const llvm::SmallVector<llvm::Value *, 8> &args, llvm::Value *result,
                             llvm::Type *inResultTy);
  llvm::GlobalVariable *createShaderTableVariable(ShaderTable tableKind);
  llvm::Value *getShaderIdentifier(ShaderStage stage, llvm::Value *shaderRecordIndex);
  void createDbgInfo(llvm::Module &module, llvm::Function *func);
  void processTerminalFunc(llvm::Function *func, llvm::CallInst *inst, RayHitStatus hitStatus);
  void processPostReportIntersection(llvm::Function *func, llvm::CallInst *inst);
  void initTraceParamsTy(unsigned attributeSize);
  void initGlobalPayloads();
  void initGlobalCallableData();
  void initShaderBuiltIns();
  void createEntryFunc(llvm::Function *func);
  llvm::FunctionType *getShaderEntryFuncTy(ShaderStage stage);
  llvm::FunctionType *getCallableShaderEntryFuncTy();
  llvm::FunctionType *getTraceRayFuncTy();
  void createCallableShaderEntryFunc(llvm::Function *func);
  void getFuncRets(llvm::Function *func, llvm::SmallVector<llvm::Instruction *, 4> &rets);
  llvm::SmallSet<unsigned, 4> getShaderExtraInputParams(ShaderStage stage);
  llvm::SmallSet<unsigned, 4> getShaderExtraRets(ShaderStage stage);
  llvm::Type *getShaderReturnTy(ShaderStage stage);
  void storeFunctionCallResult(ShaderStage stage, llvm::Value *result);
  void initInputResult(ShaderStage stage, llvm::Value *payload, llvm::Value *traceParams[], llvm::Value *result);
  void cloneDbgInfoSubgrogram(llvm::Function *func, llvm::Function *newfunc);
  llvm::Value *createLoadRayTracingMatrix(unsigned builtInId, llvm::Instruction *insertPos);
  llvm::Function *getOrCreateRemapCapturedVaToReplayVaFunc();

  llvm::GlobalVariable *m_traceParams[TraceParam::Count];              // Trace ray set parameters
  llvm::GlobalVariable *m_shaderTable[ShaderTable::Count];             // Shader table variables
  llvm::GlobalVariable *m_funcRetFlag;                                 // Function return flag
  llvm::Value *m_worldToObjMatrix;                                     // World to Object matrix
  llvm::GlobalVariable *m_globalPayload;                               // Global payload variable
  llvm::GlobalVariable *m_globalCallableData;                          // Global callable data variable
  std::set<unsigned, std::less<unsigned>> m_builtInParams;             // Indirect max builtins;
  llvm::SmallVector<llvm::Type *, TraceParam::Count> m_traceParamsTys; // Trace Params types
};

} // namespace Llpc
