/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "spirv.hpp"

#include "llpc.h"
#include "vkgcUtil.h"

namespace Llpc
{

using Vkgc::InvalidValue;
using Vkgc::VoidPtrInc;

// Size of vec4
static const unsigned SizeOfVec4 = sizeof(float) * 4;

// Descriptor offset reloc magic number
static const unsigned DescRelocMagic        = 0xA5A5A500;
static const unsigned DescRelocMagicMask    = 0xFFFFFF00;
static const unsigned DescSetMask           = 0x000000FF;

// Gets the name string of shader stage.
const char* GetShaderStageName(ShaderStage shaderStage);

// Translates shader stage to corresponding stage mask.
unsigned ShaderStageToMask(ShaderStage stage);

// Convert shader stage to the SPIR-V execution model
spv::ExecutionModel ConvertToExecModel(ShaderStage shaderStage);

// Convert SPIR-V execution model to the shader stage
ShaderStage ConvertToStageShage(unsigned execModel);

// =====================================================================================================================
// Gets module ID according to the index
inline unsigned GetModuleIdByIndex(
    unsigned index)  // Index in stage array
{
    static const unsigned BaseModuleId = 1;
    return BaseModuleId + index;
}

// =====================================================================================================================
// Decrements a pointer by nBytes by first casting it to a uint8_t*.
//
// Returns decremented pointer.
inline void* VoidPtrDec(
    const void* p,         // [in] Pointer to be decremented.
    size_t      numBytes)  // Number of bytes to decrement the pointer by
{
    void* ptr = const_cast<void*>(p);
    return (static_cast<uint8_t*>(ptr) - numBytes);
}

// =====================================================================================================================
// Finds the number of bytes between two pointers by first casting them to uint8*.
//
// This function expects the first pointer to not be smaller than the second.
//
// Returns Number of bytes between the two pointers.
inline size_t VoidPtrDiff(
    const void* p1,  //< [in] First pointer (higher address).
    const void* p2)  //< [in] Second pointer (lower address).
{
    return (static_cast<const uint8_t*>(p1) - static_cast<const uint8_t*>(p2));
}

// =====================================================================================================================
// Computes the base-2 logarithm of an unsigned 64-bit integer.
//
// If the given integer is not a power of 2, this function will not provide an exact answer.
//
// Returns log2(u)
template< typename T>
inline unsigned Log2(
    T u)  // Value to compute the logarithm of.
{
    unsigned logValue = 0;

    while (u > 1)
    {
        ++logValue;
        u >>= 1;
    }

    return logValue;
}

} // Llpc

