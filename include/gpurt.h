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
* @file  gpurt.h
* @brief Main header for the GPU raytracing shared code component
***********************************************************************************************************************
*/

#pragma once

#include "gpurt/gpurtLib.h"
#define MAKE_GPURT_VERSION(MAJOR, MINOR) ((MAJOR << 16) | MINOR)

namespace GpuRt {

#pragma pack(push, 4)
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
#pragma pack(pop)

}; // namespace GpuRt
