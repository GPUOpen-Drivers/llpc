/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  GpurtShim.cpp
 * @brief Fetch the shader library code directly from the GPURT component.
 *
 * This is not built in standalone LLPC builds (meaning builds that don't have the driver repository available).
 ***********************************************************************************************************************
 */

#include "gpurt/gpurt.h"
#include "vkgcGpurtShim.h"
#include <cassert>

using namespace Vkgc;

void gpurt::getShaderLibrarySpirv(unsigned featureFlags, const void *&code, size_t &size) {
  auto libCode = GpuRt::GetShaderLibraryCode(featureFlags);
  code = libCode.pSpvCode;
  size = libCode.spvSize;
}

RtIpVersion gpurt::getRtIpVersion(GfxIpVersion gfxIpVersion) {
  if (gfxIpVersion.major >= 11)
    return {2, 0};
  if (gfxIpVersion >= GfxIpVersion{10, 3})
    return {1, 1};
  return {0, 0};
}

static Pal::RayTracingIpLevel getRtIpLevel(RtIpVersion rtIpVersion) {
  // clang-format off
  static const std::pair<RtIpVersion, Pal::RayTracingIpLevel> map[] = {
      {{0, 0}, Pal::RayTracingIpLevel::_None},
      {{1, 0}, Pal::RayTracingIpLevel::RtIp1_0},
      {{1, 1}, Pal::RayTracingIpLevel::RtIp1_0},
#if PAL_BUILD_GFX11
      {{2, 0}, Pal::RayTracingIpLevel::RtIp2_0},
#endif
  };
  // clang-format on

  for (const auto &entry : map) {
    if (entry.first == rtIpVersion)
      return entry.second;
  }

  abort();
}

static void unmangleDxilName(char *dst, const char *src) {
  // input  "\01?RayQueryProceed1_1@@YA_NURayQueryInternal@@IV?$vector@I$02@@@Z"
  // output "RayQueryProceed1_1"
  assert(src[0] == '\01' && src[1] == '?');

  src += 2;

  const char *end = strstr(src, "@@");
  assert(end != nullptr);

  size_t len = end - src;
  assert(len <= GpurtFuncTable::MaxFunctionNameLength);

  memcpy(dst, src, len);
  dst[len] = 0;
}

void gpurt::getFuncTable(RtIpVersion rtIpVersion, GpurtFuncTable &table) {
  memset(&table, 0, sizeof(table));

  Pal::RayTracingIpLevel rtIpLevel = getRtIpLevel(rtIpVersion);
  GpuRt::EntryFunctionTable gpurtTable;
#if GPURT_BUILD_RTIP3
  GpuRt::QueryRayTracingEntryFunctionTable(rtIpLevel, true, &gpurtTable);
#else
  GpuRt::QueryRayTracingEntryFunctionTable(rtIpLevel, &gpurtTable);
#endif

  unmangleDxilName(table.pFunc[RT_ENTRY_TRACE_RAY], gpurtTable.traceRay.pTraceRay);
  unmangleDxilName(table.pFunc[RT_ENTRY_TRACE_RAY_INLINE], gpurtTable.rayQuery.pTraceRayInline);
  unmangleDxilName(table.pFunc[RT_ENTRY_TRACE_RAY_HIT_TOKEN], gpurtTable.traceRay.pTraceRayUsingHitToken);
  unmangleDxilName(table.pFunc[RT_ENTRY_RAY_QUERY_PROCEED], gpurtTable.rayQuery.pProceed);
  unmangleDxilName(table.pFunc[RT_ENTRY_INSTANCE_INDEX], gpurtTable.intrinsic.pGetInstanceIndex);
  unmangleDxilName(table.pFunc[RT_ENTRY_INSTANCE_ID], gpurtTable.intrinsic.pGetInstanceID);
  unmangleDxilName(table.pFunc[RT_ENTRY_OBJECT_TO_WORLD_TRANSFORM], gpurtTable.intrinsic.pGetObjectToWorldTransform);
  unmangleDxilName(table.pFunc[RT_ENTRY_WORLD_TO_OBJECT_TRANSFORM], gpurtTable.intrinsic.pGetWorldToObjectTransform);
  unmangleDxilName(table.pFunc[RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_NODE_POINTER],
                   gpurtTable.intrinsic.pFetchTrianglePositionFromNodePointer);
  unmangleDxilName(table.pFunc[RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_RAY_QUERY],
                   gpurtTable.intrinsic.pFetchTrianglePositionFromRayQuery);
}
