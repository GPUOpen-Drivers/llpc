/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- DXILEnums.h ---------------------------------------------------------===//
//
// This files contains enums and related functions for processing DXIL.
//
//===----------------------------------------------------------------------===//

#pragma once
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include <type_traits>

namespace llvmraytracing {

// DXIL defines a large number of DxOpCodes. We only include the ones we need here.
namespace DxOpCode {
enum Enum : unsigned {
  IsNaN = 8, // Returns true if x is NAN or QNAN, false otherwise.

  // This is not a typical return code for DirectXShaderCompiler, but we use it.
  Invalid = 0xFFFFFFFF,
};
} // namespace DxOpCode

namespace IsSpecialFloatF32ArgIndex {
enum Enum : unsigned { Opcode = 0, Value, Count };
}

namespace CreateHandleArgIndex {
enum Enum {
  OpCode,
  ResourceClass,
  RangeId,
  Index,
  NonUniformIndex,
};
}

namespace CreateHandleFromBindingArgIndex {
enum Enum {
  OpCode,
  Bind,
  Index,
  NonUniformIndex,
};
}

namespace CreateHandleForLibArgIndex {
enum Enum { OpCode, Resource };
}

namespace AnnotateHandleArgIndex {
enum OperandIdx { OpCode, Bind, Index, NonUniformIndex };
}

namespace ReportHitArgIndex {
enum Enum : unsigned { Opcode = 0, THit, HitKind, Attributes, Count };
}

namespace CallShaderArgIndex {
enum Enum : unsigned { Opcode = 0, ShaderIndex, Param, Count };
}

namespace AtomicCompareExchangeArgIndex {
enum Enum : unsigned { Opcode = 0, Handle, Offset0, Offset1, Offset2, CompareValue, NewValue, Count };
}

namespace TraceRayArgIndex {
enum Enum : unsigned {
  Opcode = 0,
  AccelStruct,
  RayFlags,
  InstanceInclusionMask,
  RayContributionToHitGroupIndex,
  MultiplierForGeometryContribution,
  MissShaderIndex,
  OriginX,
  OriginY,
  OriginZ,
  TMin,
  DirX,
  DirY,
  DirZ,
  TMax,
  Payload,
  Count
};
}

namespace RayQueryTraceRayInlineArgIndex {
enum Enum : unsigned {
  Opcode = 0,
  RayQueryHandle,
  AccelStruct,
  RayFlags,
  InstanceInclusionMask,
  OriginX,
  OriginY,
  OriginZ,
  TMin,
  DirX,
  DirY,
  DirZ,
  TMax,
  Count
};
}

// Indices into the dx.resources MDTuple.
namespace DxResourceMDIndex {
enum Enum : unsigned { SRVs = 0, UAVs, CBuffers, Samplers, Count };
}

// Indices of a resource MDTuple.
namespace ResourceMDIndex {
enum Enum : unsigned {
  ID = 0,     // Unique (per type) resource ID.
  Variable,   // Resource global variable.
  Name,       // Original (HLSL) name of the resource.
  SpaceID,    // Resource range space ID.
  LowerBound, // Resource range lower bound.
  RangeSize,  // Resource range size.
  Count
};
}

namespace DxResourceClass {
enum Enum { SRV, UAV, CBuffer, Sampler, Invalid };
}

namespace DxResBindIndex {
enum Enum {
  RangeLowerBound,
  RangeUpperBound,
  SpaceID,
  ResourceClass,
};
}

} // namespace llvmraytracing
