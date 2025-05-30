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
/**
 ***********************************************************************************************************************
 * @file  LgcXdlTypes.h
 * @brief Declarations for the LGC XDL Types and Enumerations
 ***********************************************************************************************************************
 */
#pragma once

namespace lgc::xdl {

enum class CooperativeMatrixMemoryAccess : unsigned {
  MemoryAccessMaskNone = 0x00,     // No mask
  MemoryAccessVolatileMask = 0x01, // Access memory in volatile
  MemoryAccessCoherentMask = 0x02, // Access memory in coherent
  MemoryAccessTemporalMask = 0x04, // Access memory in temporal
};

enum class CooperativeMatrixElementType : unsigned {
  Unknown = 0,   // Unknown
  Float16,       // 16-bit floating-point
  Float32,       // 32-bit floating-point
  Int8,          // 8-bit integer
  Int16,         // 16-bit integer
  Int32,         // 32 bit integer
  Float16Packed, // packed 16-bit floating-point
  BFloat16,      // 16-bit brain floating-point
  Float8,        // 8-bit floating-point
  BFloat8,       // 8-bit brain floating-point
  Int4,          // 4-bit integer
};

// Layout is virtual concept, eg: 16bit and 32bit for matrixC will share the same layout initially.
// It will be passed as the argument of getTypeProperties to calculate the more detailed layout information.
enum class CooperativeMatrixLayout : unsigned {
  FactorMatrixLayout = 0,            // A/B layout on gfx10/gfx11
  AccumulatorMatrixLayout,           // C/D layout on gfx11
  Gfx10AccumulatorMatrixLayout,      // 32bit@C/D layout on gfx10
  Gfx10Accumulator16bitMatrixLayout, // 16bit@C/D layout on gfx10
  Gfx12BaseLayout,                   // Base layout on gfx12
  Gfx12SwizzledKX16Layout,           // KX16 layout for denseB on gfx12.
  InvalidLayout
};

// The cooperative matrix arithmetic operations the builder can consume.
// NOTE: We rely on casting this implicitly to an integer, so we cannot use an enum class.
enum class CooperativeMatrixArithOp : unsigned {
  IAdd = 0,
  FAdd,
  ISub,
  FSub,
  IMul,
  FMul,
  UDiv,
  SDiv,
  FDiv,
  UMod,
  SRem,
  SMod,
  FRem,
  FMod
};

enum class SparseCooperativeMatrixSparsityFormat : unsigned {
  Sparsity2to4AMD = 0, // Format of the sparse index
  InvalidSparsityFormat
};

} // namespace lgc::xdl
