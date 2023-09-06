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
* @file  gpurt-compiler.h
* @brief Main header for the GPU raytracing shared code component, cut down to essentials and renamed to reduce the
* risk of mixing up files.
***********************************************************************************************************************
*/

#pragma once

#define MAKE_GPURT_VERSION(MAJOR, MINOR) ((MAJOR << 16) | MINOR)

namespace GpuRt {

#pragma pack(push, 4)
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 31
struct DispatchRaysInfoData {
  uint64_t rayGenerationTable; ///< Shader record table for raygeneration shaders
  unsigned rayDispatchWidth;   ///< Width of the ray dispatch
  unsigned rayDispatchHeight;  ///< Height of the ray dispatch
  unsigned rayDispatchDepth;   ///< Depth of the ray dispatch

  struct {
    uint64_t baseAddress;
    unsigned strideInBytes;
  } missTable; ///< Miss shader record table

  unsigned maxRecursionDepth; ///< Maximum recursion depth

  struct {
    uint64_t baseAddress;
    unsigned strideInBytes;
  } hitGroupTable; ///< Hit group shader record table

  unsigned maxAttributeSize; ///< Maximum attribute size

  struct {
    uint64_t baseAddress;
    unsigned strideInBytes;
  } callableTable; ///< Callable shader table record

  struct {
    unsigned rayFlags;      ///< Ray flags applied when profiling is enabled
    unsigned maxIterations; ///< Maximum trace ray loop iteration limit
  } profile;

  uint64_t traceRayGpuVa; ///< Internal TraceRays indirect function GPU VA
};
#endif
struct DispatchRaysConstantData {
  unsigned rayGenerationTableAddressLo; ///< Ray generation table base address low 32-bits
  unsigned rayGenerationTableAddressHi; ///< Ray generation table base address high 32-bits
  unsigned rayDispatchWidth;            ///< Width of the ray dispatch
  unsigned rayDispatchHeight;           ///< Height of the ray dispatch
  unsigned rayDispatchDepth;            ///< Depth of the ray dispatch
  unsigned missTableBaseAddressLo;      ///< Miss shader table base address low 32-bits
  unsigned missTableBaseAddressHi;      ///< Miss shader table base address high 32-bits
  unsigned missTableStrideInBytes;      ///< Miss shader table record byte stride
  unsigned reserved0;                   ///< Reserved padding
  unsigned hitGroupTableBaseAddressLo;  ///< Hit group table base address low 32-bits
  unsigned hitGroupTableBaseAddressHi;  ///< Hit group table base address high 32-bits
  unsigned hitGroupTableStrideInBytes;  ///< Hit group table record byte stride
  unsigned reserved1;                   ///< Reserved padding
  unsigned callableTableBaseAddressLo;  ///< Callable shader table base address low 32-bits
  unsigned callableTableBaseAddressHi;  ///< Callable shader table base address high 32-bits
  unsigned callableTableStrideInBytes;  ///< Callable shader table byte stride
  unsigned profileRayFlags;             ///< Ray flags for profiling
  unsigned profileMaxIterations;        ///< Maximum traversal iterations for profiling
  unsigned traceRayGpuVaLo;             ///< Traversal shader (shader table) base address low 32-bits
  unsigned traceRayGpuVaHi;             ///< Traversal shader (shader table) base address high 32-bits
  unsigned counterMode;                 ///< Counter capture mode. see TraceRayCounterMode
  unsigned counterRayIdRangeBegin;      ///< Counter capture ray ID range begin
  unsigned counterRayIdRangeEnd;        ///< Counter capture ray ID range end
  unsigned cpsBackendStackSize;         ///< The scratch memory used as stacks are divided into two parts:
                                        ///<  (a) Used by a compiler backend, start at offset 0.
  unsigned cpsFrontendStackSize; ///<  (b) Used by IR (Intermediate Representation), for a continuation passing shader.
  unsigned cpsGlobalMemoryAddressLo; ///< Separate CPS stack memory base address low 32-bits
  unsigned cpsGlobalMemoryAddressHi; ///< Separate CPS stack memory base address high 32-bits
  unsigned counterMask;              ///< Mask for filtering ray history token
};
#pragma pack(pop)

}; // namespace GpuRt
