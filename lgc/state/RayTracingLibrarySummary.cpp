/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  RayTracingLibrarySummary.cpp
 * @brief Implementation of helpers for raytracing library summaries
 ***********************************************************************************************************************
 */

#include "lgc/RayTracingLibrarySummary.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

using namespace llvm;
using namespace lgc;

namespace {
namespace RtLibSummary {

constexpr unsigned MajorVersion = 1;

static constexpr char Version[] = "version";
static constexpr char UsesTraceRay[] = "uses_trace_ray";
static constexpr char KnownSetRayFlags[] = "ray_flags_known_set";
static constexpr char KnownUnsetRayFlags[] = "ray_flags_known_unset";
static constexpr char MaxRayPayloadSize[] = "max_ray_payload_size";
static constexpr char MaxHitAttributeSize[] = "max_hit_attribute_size";
static constexpr char MaxUsedPayloadRegisterCount[] = "max_used_payload_register_count";
static constexpr char HasKernelEntry[] = "has_kernel_entry";
static constexpr char HasTraceRayModule[] = "has_trace_ray_module";

} // namespace RtLibSummary
} // anonymous namespace

Expected<RayTracingLibrarySummary> RayTracingLibrarySummary::decodeMsgpack(StringRef data) {
  RayTracingLibrarySummary rls = {};
  msgpack::Document doc;

  if (!doc.readFromBlob(data, false))
    return make_error<StringError>("failed to parse msgpack", inconvertibleErrorCode());

  auto &root = doc.getRoot().getMap();

  auto getBool = [](msgpack::DocNode &node, bool &out) {
    if (!node.isEmpty())
      out = node.getBool();
  };
  auto getUInt = [](msgpack::DocNode &node, auto &out) {
    if (!node.isEmpty())
      out = node.getUInt();
  };

  uint64_t version = 0;
  getUInt(root[RtLibSummary::Version], version);
  if (version != RtLibSummary::MajorVersion)
    return make_error<StringError>("bad/missing RtLibSummary version", inconvertibleErrorCode());

  getBool(root[RtLibSummary::UsesTraceRay], rls.usesTraceRay);
  getUInt(root[RtLibSummary::KnownSetRayFlags], rls.knownSetRayFlags);
  getUInt(root[RtLibSummary::KnownUnsetRayFlags], rls.knownUnsetRayFlags);
  getUInt(root[RtLibSummary::MaxRayPayloadSize], rls.maxRayPayloadSize);
  getUInt(root[RtLibSummary::MaxHitAttributeSize], rls.maxHitAttributeSize);
  getUInt(root[RtLibSummary::MaxUsedPayloadRegisterCount], rls.maxUsedPayloadRegisterCount);
  getBool(root[RtLibSummary::HasKernelEntry], rls.hasKernelEntry);
  getBool(root[RtLibSummary::HasTraceRayModule], rls.hasTraceRayModule);

  return rls;
}

std::string RayTracingLibrarySummary::encodeMsgpack() const {
  msgpack::Document doc;

  auto &root = doc.getRoot().getMap(true);

  root[RtLibSummary::Version] = RtLibSummary::MajorVersion;

  root[RtLibSummary::UsesTraceRay] = usesTraceRay;
  root[RtLibSummary::KnownSetRayFlags] = knownSetRayFlags;
  root[RtLibSummary::KnownUnsetRayFlags] = knownUnsetRayFlags;
  root[RtLibSummary::MaxRayPayloadSize] = maxRayPayloadSize;
  root[RtLibSummary::MaxHitAttributeSize] = maxHitAttributeSize;
  root[RtLibSummary::MaxUsedPayloadRegisterCount] = maxUsedPayloadRegisterCount;
  root[RtLibSummary::HasKernelEntry] = hasKernelEntry;
  root[RtLibSummary::HasTraceRayModule] = hasTraceRayModule;

  std::string out;
  doc.writeToBlob(out);
  return out;
}

void RayTracingLibrarySummary::merge(const RayTracingLibrarySummary &other) {
  usesTraceRay |= other.usesTraceRay;
  if (other.usesTraceRay) {
    knownSetRayFlags &= other.knownSetRayFlags;
    knownUnsetRayFlags &= other.knownUnsetRayFlags;
  }
  maxRayPayloadSize = std::max(maxRayPayloadSize, other.maxRayPayloadSize);
  maxHitAttributeSize = std::max(maxHitAttributeSize, other.maxHitAttributeSize);
  maxUsedPayloadRegisterCount = std::max(maxUsedPayloadRegisterCount, other.maxUsedPayloadRegisterCount);

  // TODO: Inherit kernel entry and trace ray module if possible and avoid recompile?
  hasKernelEntry = false;
  hasTraceRayModule = false;
}
