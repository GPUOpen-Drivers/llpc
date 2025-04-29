/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerRayTracing.h
 * @brief LLPC header file: contains declaration of Llpc::LowerRayTracing
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "SPIRVInternal.h"
#include "compilerutils/CompilerUtils.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/PassManager.h"
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
class GpurtGetRayStaticIdOp;
class GpurtStackReadOp;
class GpurtStackWriteOp;
class GpurtLdsStackInitOp;
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
  RayStaticId,                // Ray static ID
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

constexpr unsigned SqttWellKnownTypeFunctionCallCompact = 0x11;
constexpr unsigned SqttWellKnownTypeFunctionReturn = 0x10;
constexpr unsigned SqttWellKnownTypeIndirectFunctionCall = 0x4;

// Corresponds to gl_RayFlags* in GLSL_EXT_ray_tracing.txt
enum RayFlag : unsigned {
  None = 0x0000,                       // gl_RayFlagsNoneEXT
  ForceOpaque = 0x0001,                // gl_RayFlagsOpaqueEXT
  ForceNonOpaque = 0x0002,             // gl_RayFlagsNoOpaqueEXT
  AcceptFirstHitAndEndSearch = 0x0004, // gl_RayFlagsTerminateOnFirstHitEXT
  SkipClosestHitShader = 0x0008,       // gl_RayFlagsSkipClosestHitShaderEXT
  CullBackFacingTriangles = 0x0010,    // gl_RayFlagsCullBackFacingTrianglesEXT
  CullFrontFacingTriangles = 0x0020,   // gl_RayFlagsCullFrontFacingTrianglesEXT
  CullOpaque = 0x0040,                 // gl_RayFlagsCullOpaqueEXT
  CullNonOpaque = 0x0080,              // gl_RayFlagsCullNoOpaqueEXT
};

// =====================================================================================================================
// Represents the pass of FE lowering ray tracing.
class LowerRayTracing : public llvm::PassInfoMixin<LowerRayTracing> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower ray tracing operations"; }
};

} // namespace Llpc
