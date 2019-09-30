/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcDebug.h"

namespace Llpc
{

// Invalid value
static const uint32_t InvalidValue  = ~0u;

// Size of vec4
static const uint32_t SizeOfVec4 = sizeof(float) * 4;

// Gets the name string of shader stage.
const char* GetShaderStageName(ShaderStage shaderStage);

// Gets name string of the abbreviation for the specified shader stage.
const char* GetShaderStageAbbreviation(ShaderStage shaderStage, bool upper = false);

// Translates shader stage to corresponding stage mask.
uint32_t ShaderStageToMask(ShaderStage stage);

// =====================================================================================================================
// Gets module ID according to the index
inline uint32_t GetModuleIdByIndex(
    uint32_t index)  // Index in stage array
{
    static const uint32_t BaseModuleId = 1;
    return BaseModuleId + index;
}


// Create directory.
bool CreateDirectory(const char* pDir);

// Translate enum "ResourceMappingNodeType" to string
const char* GetResourceMappingNodeTypeName(ResourceMappingNodeType type);

// =====================================================================================================================
// Increments a pointer by nBytes by first casting it to a uint8_t*.
//
// Returns incremented pointer.
inline void* VoidPtrInc(
    const void* p,         // [in] Pointer to be incremented.
    size_t      numBytes)  // Number of bytes to increment the pointer by
{
    void* ptr = const_cast<void*>(p);
    return (static_cast<uint8_t*>(ptr) + numBytes);
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
// Determines if a value is a power of two.
inline bool IsPowerOfTwo(
    uint64_t value)  // Value to check.
{
    return (value == 0) ? false : ((value & (value - 1)) == 0);
}

// =====================================================================================================================
// Rounds the specified uint "value" up to the nearest value meeting the specified "alignment".  Only power of 2
// alignments are supported by this function.
template<typename T>
inline T Pow2Align(
    T        value,      // Value to align.
    uint64_t alignment)  // Desired alignment (must be a power of 2)
{
    LLPC_ASSERT(IsPowerOfTwo(alignment));
    return ((value + static_cast<T>(alignment) - 1) & ~(static_cast<T>(alignment) - 1));
}

// =====================================================================================================================
// Rounds up the specified integer to the nearest multiple of the specified alignment value.
// Returns rounded value.
template<typename T>
inline T RoundUpToMultiple(
    T operand,   //< Value to be aligned.
    T alignment) //< Alignment desired.
{
    return (((operand + (alignment - 1)) / alignment) * alignment);
}

// =====================================================================================================================
// Rounds down the specified integer to the nearest multiple of the specified alignment value.
// Returns rounded value.
template<typename T>
inline T RoundDownToMultiple(
    T operand,    //< Value to be aligned.
    T alignment)  //< Alignment desired.
{
    return ((operand / alignment) * alignment);
}

// =====================================================================================================================
// Computes the base-2 logarithm of an unsigned 64-bit integer.
//
// If the given integer is not a power of 2, this function will not provide an exact answer.
//
// Returns log2(u)
template< typename T>
inline uint32_t Log2(
    T u)  // Value to compute the logarithm of.
{
    uint32_t logValue = 0;

    while (u > 1)
    {
        ++logValue;
        u >>= 1;
    }

    return logValue;
}

// ===================================================================================
// Returns the bits of a floating point value as an unsigned integer.
inline uint32_t FloatToBits(
    float f)        // Float to be converted to bits
{
    return (*(reinterpret_cast<uint32_t*>(&f)));
}

// ===================================================================================
// Finds the expected structure in Vulkan structure chain with the specified info.
template<class T>
inline const T* FindVkStructInChain(
    VkStructureType type,    // Vulkan structure type
    const void*     pNext)   // Base pointer of Vulkan structure
{
    struct VkStructHeader
    {
        VkStructureType type;
        VkStructHeader* pNext;
    };

    const VkStructHeader* pStructHeader = reinterpret_cast<const VkStructHeader*>(pNext);
    while(pStructHeader != nullptr)
    {
        if (pStructHeader->type == type)
        {
            break;
        }
        else
        {
            pStructHeader = pStructHeader->pNext;
        }
    }

    return reinterpret_cast<const T*>(pStructHeader);
}

} // Llpc

