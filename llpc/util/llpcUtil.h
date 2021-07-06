/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC header file: contains the definition of LLPC internal types and utility functions
 * (independent of LLVM use).
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "spirv.hpp"
#include "vkgcDefs.h"
#include "vkgcUtil.h"
#include "lgc/EnumIterator.h"
#include "llvm/ADT/ArrayRef.h"

namespace Llpc {

using Vkgc::InvalidValue;
using Vkgc::voidPtrInc;

// Size of vec4
static const unsigned SizeOfVec4 = sizeof(float) * 4;

// Descriptor offset reloc magic number
static const unsigned DescRelocMagic = 0xA5A5A500;
static const unsigned DescRelocMagicMask = 0xFFFFFF00;
static const unsigned DescSetMask = 0x000000FF;

// Gets the name string of shader stage.
const char *getShaderStageName(ShaderStage shaderStage);

// Translates shader stage to corresponding stage mask.
unsigned shaderStageToMask(ShaderStage stage);

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

inline bool doesShaderStageExist(llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo, ShaderStage stage) {
  return stage < shaderInfo.size() && shaderInfo[stage] && shaderInfo[stage]->pModuleData;
}
} // namespace Llpc

namespace lgc {
// Make Vkgc::UnlinkedShaderStage iterable using `lgc::enumRange<Vkgc::ShaderStage>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(Vkgc::ShaderStage, Vkgc::ShaderStageCountInternal);

// Make Vkgc::UnlinkedShaderStage iterable using `lgc::enumRange<Vkgc::UnlinedShaderStage>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(Vkgc::UnlinkedShaderStage, Vkgc::UnlinkedStageCount);
} // namespace lgc

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
} // namespace Llpc
