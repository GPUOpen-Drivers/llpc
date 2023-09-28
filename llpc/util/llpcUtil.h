/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcUtil.h
 * @brief LLPC header file: contains the definition of LLPC internal types and utility functions.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "spirv.hpp"
#include "vkgcDefs.h"
#include "vkgcMetroHash.h"
#include "vkgcUtil.h"
#include "lgc/CommonDefs.h"
#include "lgc/EnumIterator.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Twine.h"
#include <cassert>

namespace Llpc {

using Vkgc::InvalidValue;
using Vkgc::voidPtrInc;

// Size of vec4
static const unsigned SizeOfVec4 = sizeof(float) * 4;

class Context;

// Gets the name string of shader stage.
const char *getShaderStageName(ShaderStage shaderStage);

bool isRayTracingShaderStage(ShaderStage stage);
bool hasRayTracingShaderStage(unsigned shageMask);

// Convert shader stage to the SPIR-V execution model
spv::ExecutionModel convertToExecModel(ShaderStage shaderStage);

// Convert SPIR-V execution model to the shader stage
ShaderStage convertToShaderStage(unsigned execModel);

// =====================================================================================================================
// Gets module ID according to the index
//
// @param index : Index in stage array
inline unsigned getModuleIdByIndex(unsigned index) {
  static const unsigned BaseModuleId = 1;
  return BaseModuleId + index;
}

// =====================================================================================================================
// Decrements a pointer by nBytes by first casting it to a uint8_t*.
//
// Returns decremented pointer.
//
// @param p : Pointer to be decremented.
// @param numBytes : Number of bytes to decrement the pointer by
inline void *voidPtrDec(const void *p, size_t numBytes) {
  void *ptr = const_cast<void *>(p);
  return (static_cast<uint8_t *>(ptr) - numBytes);
}

// =====================================================================================================================
// Finds the number of bytes between two pointers by first casting them to uint8*.
//
// This function expects the first pointer to not be smaller than the second.
//
// Returns Number of bytes between the two pointers.
//
// @param p1 : First pointer (higher address).
// @param p2 : Second pointer (lower address).
inline size_t voidPtrDiff(const void *p1, const void *p2) {
  return (static_cast<const uint8_t *>(p1) - static_cast<const uint8_t *>(p2));
}

// =====================================================================================================================
// Computes the base-2 logarithm of an unsigned 64-bit integer.
//
// If the given integer is not a power of 2, this function will not provide an exact answer.
//
// Returns log2(u)
template <typename T>
//
// @param u : Value to compute the logarithm of.
inline unsigned log2(T u) {
  unsigned logValue = 0;

  while (u > 1) {
    ++logValue;
    u >>= 1;
  }

  return logValue;
}

// Returns true if shaderInfo has the information required to compile an unlinked shader of the given type.
bool hasDataForUnlinkedShaderType(Vkgc::UnlinkedShaderStage type,
                                  llvm::ArrayRef<const Vkgc::PipelineShaderInfo *> shaderInfo);

// Returns the shader stage mask for all shader stages that can be part of the given unlinked shader type.
unsigned getShaderStageMaskForType(Vkgc::UnlinkedShaderStage type);

// Returns the name of the given unlinked shader stage.
const char *getUnlinkedShaderStageName(Vkgc::UnlinkedShaderStage type);

// Returns the name of the given part-pipeline stage.
const char *getPartPipelineStageName(Vkgc::PartPipelineStage type);

// Returns the uniform constant map entry of the given location.
Vkgc::UniformConstantMapEntry *getUniformConstantEntryByLocation(const Llpc::Context *context, Vkgc::ShaderStage stage,
                                                                 unsigned loc);

inline bool doesShaderStageExist(llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo, ShaderStage stage) {
  return stage < shaderInfo.size() && shaderInfo[stage] && shaderInfo[stage]->pModuleData;
}

// =====================================================================================================================
// Returns true iff `stage` is present in the `stageMask`.
//
// @param stage : Shader stage to look for
// @param stageMask : Stage mask to check
// @returns : true iff `stageMask` contains `stage`
inline bool isShaderStageInMask(Vkgc::ShaderStage stage, unsigned stageMask) {
  assert(stage != Vkgc::ShaderStageInvalid);
  return (Vkgc::shaderStageToMask(stage) & stageMask) != 0;
}

// =====================================================================================================================
// Returns true iff `stage` is a native stage (graphics or compute).
//
// @param stage : Shader stage to check
// @returns : true iff `stage` is a native shader stage
inline bool isNativeStage(Vkgc::ShaderStage stage) {
  return lgc::toUnderlying(stage) < lgc::toUnderlying(Vkgc::ShaderStage::ShaderStageNativeStageCount);
}

// =====================================================================================================================
// Return true iff `stageMask` contains only the compute stage.
//
// @param stageMask : Stage mask to check
// @returns : true iff `stageMask` contains only the compute stage
inline bool isComputePipeline(unsigned stageMask) {
  return stageMask == Vkgc::ShaderStageBit::ShaderStageComputeBit;
}

// =====================================================================================================================
// Return true iff `stageMask` contains only graphics stage(s).
//
// @param stageMask : Stage mask to check
// @returns : true iff `stageMask` contains only graphics stages
inline bool isGraphicsPipeline(unsigned stageMask) {
  return (stageMask & Vkgc::ShaderStageBit::ShaderStageAllGraphicsBit) != 0 &&
         (stageMask & Vkgc::ShaderStageBit::ShaderStageComputeBit) == 0;
}

// =====================================================================================================================
// Find userDataNode with specified set and binding. And return Node index.
//
// @param userDataNodes : Point to ResourceMappingNode array
// @param nodeCount : User data node count
// @param set : Find same set in node array
// @param binding : Find same binding in node array
// @param [out] index : Return node position in node array
const ResourceMappingNode *findResourceNode(const ResourceMappingNode *userDataNodes, unsigned nodeCount, unsigned set,
                                            unsigned binding, unsigned *index);

// =====================================================================================================================
// Find userDataNode with specified set and binding. And return Node index.
//
// @param userDataNodes : Point to ResourceMappingNode array
// @param nodeCount : User data node count
// @param set : Find same set in node array
// @param binding : Find same binding in node array
// @param [out] index : Return node position in node array
// @returns : The Node index
const ResourceMappingNode *findResourceNode(const ResourceMappingRootNode *userDataNodes, unsigned nodeCount,
                                            unsigned set, unsigned binding, unsigned *index);

// =====================================================================================================================
// Returns true iff the compiled pipeline is a raytracing pipeline.
//
// @returns : True iff the compiled pipeline is a raytracing pipeline, false if not.
inline bool isRayTracingPipeline(unsigned stageMask) {
  return hasRayTracingShaderStage(stageMask);
}

} // namespace Llpc

namespace llvm {
// Make Vkgc::UnlinkedShaderStage iterable using `lgc::enumRange<Vkgc::ShaderStage>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(Vkgc::ShaderStage, Vkgc::ShaderStageCountInternal);

// Make Vkgc::UnlinkedShaderStage iterable using `lgc::enumRange<Vkgc::UnlinkedShaderStage>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(Vkgc::UnlinkedShaderStage, Vkgc::UnlinkedStageCount);

// Make Vkgc::PartPipelineStage iterable using `lgc::enumRange<Vkgc::PartPipelineStage>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(Vkgc::PartPipelineStage, Vkgc::PartPipelineStageCount);
} // namespace llvm

namespace Llpc {
// Returns the range of all native ShaderStages.
inline auto nativeShaderStages() {
  return lgc::enumRange(Vkgc::ShaderStage::ShaderStageNativeStageCount);
}

// Returns the range of all graphics ShaderStages.
inline auto gfxShaderStages() {
  return lgc::enumRange(Vkgc::ShaderStage::ShaderStageGfxCount);
}

// Returns the range of all internal ShaderStages.
inline auto internalShaderStages() {
  return lgc::enumRange(Vkgc::ShaderStage::ShaderStageCopyShader, Vkgc::ShaderStage::ShaderStageCountInternal);
}

// =====================================================================================================================
// Returns a vector with all shader stages in the `stageMask`. You can use this function to iterate over all stages in
// the mask.
//
// @param stageMask : Stage mask to check
// @returns : Vector of all shader stages contained in `stageMask`, in the same order as defined in `Vkgc::ShaderStage`.
inline llvm::SmallVector<Vkgc::ShaderStage, 4> maskToShaderStages(unsigned stageMask) {
  llvm::SmallVector<Vkgc::ShaderStage, 4> stages;
  for (auto stage : lgc::enumRange<Vkgc::ShaderStage>())
    if (isShaderStageInMask(stage, stageMask))
      stages.push_back(stage);

  return stages;
}
} // namespace Llpc

// Make MetroHash::Hash compatible with LLVM's unordered containers.
namespace llvm {
template <> struct DenseMapInfo<MetroHash::Hash> {
  static MetroHash::Hash getEmptyKey() {
    MetroHash::Hash hash = {};
    hash.qwords[0] = static_cast<uint64_t>(-1);
    hash.qwords[1] = static_cast<uint64_t>(-1);
    return hash;
  }
  static MetroHash::Hash getTombstoneKey() {
    MetroHash::Hash hash = {};
    hash.qwords[0] = static_cast<uint64_t>(-2);
    hash.qwords[1] = static_cast<uint64_t>(-2);
    return hash;
  }
  static unsigned getHashValue(MetroHash::Hash hash) { return MetroHash::compact32(&hash); }
  static bool isEqual(MetroHash::Hash lhs, MetroHash::Hash rhs) { return lhs == rhs; }
};
} // namespace llvm
