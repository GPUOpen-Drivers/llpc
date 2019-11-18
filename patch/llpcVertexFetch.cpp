/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcVertexFetch.cpp
 * @brief LLPC source file: contains implementation of class Llpc::VertexFetch.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-vertex-fetch"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcSystemValues.h"
#include "llpcVertexFetch.h"

using namespace llvm;

namespace Llpc
{

#define VERTEX_FORMAT_UNDEFINED(_format) \
{ \
    _format, \
    BUF_NUM_FORMAT_FLOAT, \
    BUF_DATA_FORMAT_INVALID, \
    0, \
}

// Initializes info table of vertex format map
const VertexFormatInfo VertexFetch::m_vertexFormatInfo[] =
{
    // VK_FORMAT_UNDEFINED = 0
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_UNDEFINED),
    // VK_FORMAT_R4G4_UNORM_PACK8 = 1
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R4G4_UNORM_PACK8),
    // VK_FORMAT_R4G4B4A4_UNORM_PACK16 = 2
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R4G4B4A4_UNORM_PACK16),
    // VK_FORMAT_B4G4R4A4_UNORM_PACK16 = 3
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B4G4R4A4_UNORM_PACK16),
    // VK_FORMAT_R5G6B5_UNORM_PACK16 = 4
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R5G6B5_UNORM_PACK16),
    // VK_FORMAT_B5G6R5_UNORM_PACK16 = 5
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B5G6R5_UNORM_PACK16),
    // VK_FORMAT_R5G5B5A1_UNORM_PACK16 = 6
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R5G5B5A1_UNORM_PACK16),
    // VK_FORMAT_B5G5R5A1_UNORM_PACK16 = 7
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B5G5R5A1_UNORM_PACK16),
    // VK_FORMAT_A1R5G5B5_UNORM_PACK16 = 8
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_A1R5G5B5_UNORM_PACK16),
    // VK_FORMAT_R8_UNORM = 9
    {
        VK_FORMAT_R8_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_8,
        1
    },
    // VK_FORMAT_R8_SNORM = 10
    {
        VK_FORMAT_R8_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_8,
        1
    },
    // VK_FORMAT_R8_USCALED = 11
    {
        VK_FORMAT_R8_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_8,
        1
    },
    // VK_FORMAT_R8_SSCALED = 12
    {
        VK_FORMAT_R8_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_8,
        1
    },
    // VK_FORMAT_R8_UINT = 13
    {
        VK_FORMAT_R8_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_8,
        1
    },
    // VK_FORMAT_R8_SINT = 14
    {
        VK_FORMAT_R8_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_8,
        1
    },
    // VK_FORMAT_R8_SRGB = 15
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8_SRGB),
    // VK_FORMAT_R8G8_UNORM = 16
    {
        VK_FORMAT_R8G8_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_8_8,
        2
    },
    // VK_FORMAT_R8G8_SNORM = 17
    {
        VK_FORMAT_R8G8_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_8_8,
        2
    },
    // VK_FORMAT_R8G8_USCALED = 18
    {
        VK_FORMAT_R8G8_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_8_8,
        2
    },
    // VK_FORMAT_R8G8_SSCALED = 19
    {
        VK_FORMAT_R8G8_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_8_8,
        2
    },
    // VK_FORMAT_R8G8_UINT = 20
    {
        VK_FORMAT_R8G8_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_8_8,
        2
    },
    // VK_FORMAT_R8G8_SINT = 21
    {
        VK_FORMAT_R8G8_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_8_8,
        2
    },
    // VK_FORMAT_R8G8_SRGB = 22
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8_SRGB),
    // VK_FORMAT_R8G8B8_UNORM = 23
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_UNORM),
    // VK_FORMAT_R8G8B8_SNORM = 24
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_SNORM),
    // VK_FORMAT_R8G8B8_USCALED = 25
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_USCALED),
    // VK_FORMAT_R8G8B8_SSCALED = 26
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_SSCALED),
    // VK_FORMAT_R8G8B8_UINT = 27
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_UINT),
    // VK_FORMAT_R8G8B8_SINT = 28
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_SINT),
    // VK_FORMAT_R8G8B8_SRGB = 29
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8_SRGB),
    // VK_FORMAT_B8G8R8_UNORM = 30
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_UNORM),
    // VK_FORMAT_B8G8R8_SNORM = 31
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_SNORM),
    // VK_FORMAT_B8G8R8_USCALED = 32
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_USCALED),
    // VK_FORMAT_B8G8R8_SSCALED = 33
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_SSCALED),
    // VK_FORMAT_B8G8R8_UINT = 34
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_UINT),
    // VK_FORMAT_B8G8R8_SINT = 35
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_SINT),
    // VK_FORMAT_B8G8R8_SRGB = 36
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8_SRGB),
    // VK_FORMAT_R8G8B8A8_UNORM = 37
    {
        VK_FORMAT_R8G8B8A8_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_R8G8B8A8_SNORM = 38
    {
        VK_FORMAT_R8G8B8A8_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_R8G8B8A8_USCALED = 39
    {
        VK_FORMAT_R8G8B8A8_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_R8G8B8A8_SSCALED = 40
    {
        VK_FORMAT_R8G8B8A8_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_R8G8B8A8_UINT = 41
    {
        VK_FORMAT_R8G8B8A8_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_R8G8B8A8_SINT = 42
    {
        VK_FORMAT_R8G8B8A8_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_R8G8B8A8_SRGB = 43
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R8G8B8A8_SRGB),
    // VK_FORMAT_B8G8R8A8_UNORM = 44
    {
        VK_FORMAT_B8G8R8A8_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_B8G8R8A8_SNORM = 45
    {
        VK_FORMAT_B8G8R8A8_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_B8G8R8A8_USCALED = 46
    {
        VK_FORMAT_B8G8R8A8_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_B8G8R8A8_SSCALED = 47
    {
        VK_FORMAT_B8G8R8A8_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_B8G8R8A8_UINT = 48
    {
        VK_FORMAT_B8G8R8A8_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_B8G8R8A8_SINT = 49
    {
        VK_FORMAT_B8G8R8A8_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_B8G8R8A8_SRGB = 50
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_B8G8R8A8_SRGB),
    // VK_FORMAT_A8B8G8R8_UNORM_PACK32 = 51
    {
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_A8B8G8R8_SNORM_PACK32 = 52
    {
        VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_A8B8G8R8_USCALED_PACK32 = 53
    {
        VK_FORMAT_A8B8G8R8_USCALED_PACK32,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_A8B8G8R8_SSCALED_PACK32 = 54
    {
        VK_FORMAT_A8B8G8R8_SSCALED_PACK32,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_A8B8G8R8_UINT_PACK32 = 55
    {
        VK_FORMAT_A8B8G8R8_UINT_PACK32,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_A8B8G8R8_SINT_PACK32 = 56
    {
        VK_FORMAT_A8B8G8R8_SINT_PACK32,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_8_8_8_8,
        4
    },
    // VK_FORMAT_A8B8G8R8_SRGB_PACK32 = 57
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_A8B8G8R8_SRGB_PACK32),
    // VK_FORMAT_A2R10G10B10_UNORM_PACK32 = 58
    {
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2R10G10B10_SNORM_PACK32 = 59
    {
        VK_FORMAT_A2R10G10B10_SNORM_PACK32,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2R10G10B10_USCALED_PACK32 = 60
    {
        VK_FORMAT_A2R10G10B10_USCALED_PACK32,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2R10G10B10_SSCALED_PACK32 = 61
    {
        VK_FORMAT_A2R10G10B10_SSCALED_PACK32,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2R10G10B10_UINT_PACK32 = 62
    {
        VK_FORMAT_A2R10G10B10_UINT_PACK32,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2R10G10B10_SINT_PACK32 = 63
    {
        VK_FORMAT_A2R10G10B10_SINT_PACK32,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2B10G10R10_UNORM_PACK32 = 64
    {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2B10G10R10_SNORM_PACK32 = 65
    {
        VK_FORMAT_A2B10G10R10_SNORM_PACK32,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2B10G10R10_USCALED_PACK32 = 66
    {
        VK_FORMAT_A2B10G10R10_USCALED_PACK32,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2B10G10R10_SSCALED_PACK32 = 67
    {
        VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2B10G10R10_UINT_PACK32 = 68
    {
        VK_FORMAT_A2B10G10R10_UINT_PACK32,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_A2B10G10R10_SINT_PACK32 = 69
    {
        VK_FORMAT_A2B10G10R10_SINT_PACK32,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_2_10_10_10,
        4
    },
    // VK_FORMAT_R16_UNORM = 70
    {
        VK_FORMAT_R16_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16_SNORM = 71
    {
        VK_FORMAT_R16_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16_USCALED = 72
    {
        VK_FORMAT_R16_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16_SSCALED = 73
    {
        VK_FORMAT_R16_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16_UINT = 74
    {
        VK_FORMAT_R16_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16_SINT = 75
    {
        VK_FORMAT_R16_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16_SFLOAT = 76
    {
        VK_FORMAT_R16_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_16,
        1
    },
    // VK_FORMAT_R16G16_UNORM = 77
    {
        VK_FORMAT_R16G16_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16_SNORM = 78
    {
        VK_FORMAT_R16G16_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16_USCALED = 79
    {
        VK_FORMAT_R16G16_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16_SSCALED = 80
    {
        VK_FORMAT_R16G16_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16_UINT = 81
    {
        VK_FORMAT_R16G16_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16_SINT = 82
    {
        VK_FORMAT_R16G16_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16_SFLOAT = 83
    {
        VK_FORMAT_R16G16_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_16_16,
        2
    },
    // VK_FORMAT_R16G16B16_UNORM = 84
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_UNORM),
    // VK_FORMAT_R16G16B16_SNORM = 85
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_SNORM),
    // VK_FORMAT_R16G16B16_USCALED = 86
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_USCALED),
    // VK_FORMAT_R16G16B16_SSCALED = 87
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_SSCALED),
    // VK_FORMAT_R16G16B16_UINT = 88
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_UINT),
    // VK_FORMAT_R16G16B16_SINT = 89
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_SINT),
    // VK_FORMAT_R16G16B16_SFLOAT = 90
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_R16G16B16_SFLOAT),
    // VK_FORMAT_R16G16B16A16_UNORM = 91
    {
        VK_FORMAT_R16G16B16A16_UNORM,
        BUF_NUM_FORMAT_UNORM,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R16G16B16A16_SNORM = 92
    {
        VK_FORMAT_R16G16B16A16_SNORM,
        BUF_NUM_FORMAT_SNORM,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R16G16B16A16_USCALED = 93
    {
        VK_FORMAT_R16G16B16A16_USCALED,
        BUF_NUM_FORMAT_USCALED,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R16G16B16A16_SSCALED = 94
    {
        VK_FORMAT_R16G16B16A16_SSCALED,
        BUF_NUM_FORMAT_SSCALED,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R16G16B16A16_UINT = 95
    {
        VK_FORMAT_R16G16B16A16_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R16G16B16A16_SINT = 96
    {
        VK_FORMAT_R16G16B16A16_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R16G16B16A16_SFLOAT = 97
    {
        VK_FORMAT_R16G16B16A16_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_16_16_16_16,
        4
    },
    // VK_FORMAT_R32_UINT = 98
    {
        VK_FORMAT_R32_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32,
        1
    },
    // VK_FORMAT_R32_SINT = 99
    {
        VK_FORMAT_R32_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32,
        1
    },
    // VK_FORMAT_R32_SFLOAT = 100
    {
        VK_FORMAT_R32_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32,
        1
    },
    // VK_FORMAT_R32G32_UINT = 101
    {
        VK_FORMAT_R32G32_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32,
        2
    },
    // VK_FORMAT_R32G32_SINT = 102
    {
        VK_FORMAT_R32G32_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32,
        2
    },
    // VK_FORMAT_R32G32_SFLOAT = 103
    {
        VK_FORMAT_R32G32_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32,
        2
    },
    // VK_FORMAT_R32G32B32_UINT = 104
    {
        VK_FORMAT_R32G32B32_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32_32,
        3
    },
    // VK_FORMAT_R32G32B32_SINT = 105
    {
        VK_FORMAT_R32G32B32_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32_32,
        3
    },
    // VK_FORMAT_R32G32B32_SFLOAT = 106
    {
        VK_FORMAT_R32G32B32_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32_32,
        3
    },
    // VK_FORMAT_R32G32B32A32_UINT = 107
    {
        VK_FORMAT_R32G32B32A32_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R32G32B32A32_SINT = 108
    {
        VK_FORMAT_R32G32B32A32_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R32G32B32A32_SFLOAT = 109
    {
        VK_FORMAT_R32G32B32A32_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64_UINT = 110
    {
        VK_FORMAT_R64_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32,
        2
    },
    // VK_FORMAT_R64_SINT = 111
    {
        VK_FORMAT_R64_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32,
        2
    },
    // VK_FORMAT_R64_SFLOAT = 112
    {
        VK_FORMAT_R64_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32,
        2
    },
    // VK_FORMAT_R64G64_UINT = 113
    {
        VK_FORMAT_R64G64_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64_SINT = 114
    {
        VK_FORMAT_R64G64_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64_SFLOAT = 115
    {
        VK_FORMAT_R64G64_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64B64_UINT = 116
    {
        VK_FORMAT_R64G64B64_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64B64_SINT = 117
    {
        VK_FORMAT_R64G64B64_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64B64_SFLOAT = 118
    {
        VK_FORMAT_R64G64B64_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64B64A64_UINT = 119
    {
        VK_FORMAT_R64G64B64A64_UINT,
        BUF_NUM_FORMAT_UINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64B64A64_SINT = 120
    {
        VK_FORMAT_R64G64B64A64_SINT,
        BUF_NUM_FORMAT_SINT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_R64G64B64A64_SFLOAT = 121
    {
        VK_FORMAT_R64G64B64A64_SFLOAT,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_32_32_32_32,
        4
    },
    // VK_FORMAT_B10G11R11_UFLOAT_PACK32 = 122
    {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        BUF_NUM_FORMAT_FLOAT,
        BUF_DATA_FORMAT_10_11_11,
        3
    },
    // VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 = 123
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    // VK_FORMAT_D16_UNORM = 124
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_D16_UNORM),
    // VK_FORMAT_X8_D24_UNORM_PACK32 = 125
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_X8_D24_UNORM_PACK32),
    // VK_FORMAT_D32_SFLOAT = 126
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_D32_SFLOAT),
    // VK_FORMAT_S8_UINT = 127
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_S8_UINT),
    // VK_FORMAT_D16_UNORM_S8_UINT = 128
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_D16_UNORM_S8_UINT),
    // VK_FORMAT_D24_UNORM_S8_UINT = 129
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_D24_UNORM_S8_UINT),
    // VK_FORMAT_D32_SFLOAT_S8_UINT = 130
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_D32_SFLOAT_S8_UINT),
    // VK_FORMAT_BC1_RGB_UNORM_BLOCK = 131
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC1_RGB_UNORM_BLOCK),
    // VK_FORMAT_BC1_RGB_SRGB_BLOCK = 132
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC1_RGB_SRGB_BLOCK),
    // VK_FORMAT_BC1_RGBA_UNORM_BLOCK = 133
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
    // VK_FORMAT_BC1_RGBA_SRGB_BLOCK = 134
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
    // VK_FORMAT_BC2_UNORM_BLOCK = 135
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC2_UNORM_BLOCK),
    // VK_FORMAT_BC2_SRGB_BLOCK = 136
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC2_SRGB_BLOCK),
    // VK_FORMAT_BC3_UNORM_BLOCK = 137
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC3_UNORM_BLOCK),
    // VK_FORMAT_BC3_SRGB_BLOCK = 138
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC3_SRGB_BLOCK),
    // VK_FORMAT_BC4_UNORM_BLOCK = 139
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC4_UNORM_BLOCK),
    // VK_FORMAT_BC4_SNORM_BLOCK = 140
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC4_SNORM_BLOCK),
    // VK_FORMAT_BC5_UNORM_BLOCK = 141
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC5_UNORM_BLOCK),
    // VK_FORMAT_BC5_SNORM_BLOCK = 142
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC5_SNORM_BLOCK),
    // VK_FORMAT_BC6H_UFLOAT_BLOCK = 143
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC6H_UFLOAT_BLOCK),
    // VK_FORMAT_BC6H_SFLOAT_BLOCK = 144
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC6H_SFLOAT_BLOCK),
    // VK_FORMAT_BC7_UNORM_BLOCK = 145
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC7_UNORM_BLOCK),
    // VK_FORMAT_BC7_SRGB_BLOCK = 146
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_BC7_SRGB_BLOCK),
    // VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK = 147
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK),
    // VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK = 148
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK),
    // VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK = 149
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK),
    // VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK = 150
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK),
    // VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK = 151
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK),
    // VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK = 152
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK),
    // VK_FORMAT_EAC_R11_UNORM_BLOCK = 153
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_EAC_R11_UNORM_BLOCK),
    // VK_FORMAT_EAC_R11_SNORM_BLOCK = 154
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_EAC_R11_SNORM_BLOCK),
    // VK_FORMAT_EAC_R11G11_UNORM_BLOCK = 155
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_EAC_R11G11_UNORM_BLOCK),
    // VK_FORMAT_EAC_R11G11_SNORM_BLOCK = 156
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_EAC_R11G11_SNORM_BLOCK),
    // VK_FORMAT_ASTC_4x4_UNORM_BLOCK = 157
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_4x4_UNORM_BLOCK),
    // VK_FORMAT_ASTC_4x4_SRGB_BLOCK = 158
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_4x4_SRGB_BLOCK),
    // VK_FORMAT_ASTC_5x4_UNORM_BLOCK = 159
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_5x4_UNORM_BLOCK),
    // VK_FORMAT_ASTC_5x4_SRGB_BLOCK = 160
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_5x4_SRGB_BLOCK),
    // VK_FORMAT_ASTC_5x5_UNORM_BLOCK = 161
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_5x5_UNORM_BLOCK),
    // VK_FORMAT_ASTC_5x5_SRGB_BLOCK = 162
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_5x5_SRGB_BLOCK),
    // VK_FORMAT_ASTC_6x5_UNORM_BLOCK = 163
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_6x5_UNORM_BLOCK),
    // VK_FORMAT_ASTC_6x5_SRGB_BLOCK = 164
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_6x5_SRGB_BLOCK),
    // VK_FORMAT_ASTC_6x6_UNORM_BLOCK = 165
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_6x6_UNORM_BLOCK),
    // VK_FORMAT_ASTC_6x6_SRGB_BLOCK = 166
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_6x6_SRGB_BLOCK),
    // VK_FORMAT_ASTC_8x5_UNORM_BLOCK = 167
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_8x5_UNORM_BLOCK),
    // VK_FORMAT_ASTC_8x5_SRGB_BLOCK = 168
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_8x5_SRGB_BLOCK),
    // VK_FORMAT_ASTC_8x6_UNORM_BLOCK = 169
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_8x6_UNORM_BLOCK),
    // VK_FORMAT_ASTC_8x6_SRGB_BLOCK = 170
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_8x6_SRGB_BLOCK),
    // VK_FORMAT_ASTC_8x8_UNORM_BLOCK = 171
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_8x8_UNORM_BLOCK),
    // VK_FORMAT_ASTC_8x8_SRGB_BLOCK = 172
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_8x8_SRGB_BLOCK),
    // VK_FORMAT_ASTC_10x5_UNORM_BLOCK = 173
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x5_UNORM_BLOCK),
    // VK_FORMAT_ASTC_10x5_SRGB_BLOCK = 174
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x5_SRGB_BLOCK),
    // VK_FORMAT_ASTC_10x6_UNORM_BLOCK = 175
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x6_UNORM_BLOCK),
    // VK_FORMAT_ASTC_10x6_SRGB_BLOCK = 176
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x6_SRGB_BLOCK),
    // VK_FORMAT_ASTC_10x8_UNORM_BLOCK = 177
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x8_UNORM_BLOCK),
    // VK_FORMAT_ASTC_10x8_SRGB_BLOCK = 178
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x8_SRGB_BLOCK),
    // VK_FORMAT_ASTC_10x10_UNORM_BLOCK = 179
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x10_UNORM_BLOCK),
    // VK_FORMAT_ASTC_10x10_SRGB_BLOCK = 180
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_10x10_SRGB_BLOCK),
    // VK_FORMAT_ASTC_12x10_UNORM_BLOCK = 181
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_12x10_UNORM_BLOCK),
    // VK_FORMAT_ASTC_12x10_SRGB_BLOCK = 182
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_12x10_SRGB_BLOCK),
    // VK_FORMAT_ASTC_12x12_UNORM_BLOCK = 183
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_12x12_UNORM_BLOCK),
    // VK_FORMAT_ASTC_12x12_SRGB_BLOCK = 184
    VERTEX_FORMAT_UNDEFINED(VK_FORMAT_ASTC_12x12_SRGB_BLOCK),
};

// Initializes info table of vertex component format map
const VertexCompFormatInfo VertexFetch::m_vertexCompFormatInfo[] =
{
    { 0,  0, 0, BUF_DATA_FORMAT_INVALID     }, // BUF_DATA_FORMAT_INVALID
    { 1,  1, 1, BUF_DATA_FORMAT_8           }, // BUF_DATA_FORMAT_8
    { 2,  2, 1, BUF_DATA_FORMAT_16          }, // BUF_DATA_FORMAT_16
    { 2,  1, 2, BUF_DATA_FORMAT_8           }, // BUF_DATA_FORMAT_8_8
    { 4,  4, 1, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32
    { 4,  2, 2, BUF_DATA_FORMAT_16          }, // BUF_DATA_FORMAT_16_16
    { 4,  0, 0, BUF_DATA_FORMAT_10_11_11    }, // BUF_DATA_FORMAT_10_11_11 (Packed)
    { 4,  0, 0, BUF_DATA_FORMAT_11_11_10    }, // BUF_DATA_FORMAT_11_11_10 (Packed)
    { 4,  0, 0, BUF_DATA_FORMAT_10_10_10_2  }, // BUF_DATA_FORMAT_10_10_10_2 (Packed)
    { 4,  0, 0, BUF_DATA_FORMAT_2_10_10_10  }, // BUF_DATA_FORMAT_2_10_10_10 (Packed)
    { 4,  1, 4, BUF_DATA_FORMAT_8           }, // BUF_DATA_FORMAT_8_8_8_8
    { 8,  4, 2, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32_32
    { 8,  2, 4, BUF_DATA_FORMAT_16          }, // BUF_DATA_FORMAT_16_16_16_16
    { 12, 4, 3, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32_32_32
    { 16, 4, 4, BUF_DATA_FORMAT_32          }, // BUF_DATA_FORMAT_32_32_32_32
};

#if LLPC_BUILD_GFX10
const BufFormat VertexFetch::m_vertexFormatMap[] =
{
    // BUF_DATA_FORMAT
    //   BUF_NUM_FORMAT_UNORM
    //   BUF_NUM_FORMAT_SNORM
    //   BUF_NUM_FORMAT_USCALED
    //   BUF_NUM_FORMAT_SSCALED
    //   BUF_NUM_FORMAT_UINT
    //   BUF_NUM_FORMAT_SINT
    //   BUF_NUM_FORMAT_SNORM_NZ
    //   BUF_NUM_FORMAT_FLOAT

    //BUF_DATA_FORMAT_INVALID
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_8
    BUF_FORMAT_8_UNORM,
    BUF_FORMAT_8_SNORM,
    BUF_FORMAT_8_USCALED,
    BUF_FORMAT_8_SSCALED,
    BUF_FORMAT_8_UINT,
    BUF_FORMAT_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_16
    BUF_FORMAT_16_UNORM,
    BUF_FORMAT_16_SNORM,
    BUF_FORMAT_16_USCALED,
    BUF_FORMAT_16_SSCALED,
    BUF_FORMAT_16_UINT,
    BUF_FORMAT_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_FLOAT,

    //BUF_DATA_FORMAT_8_8
    BUF_FORMAT_8_8_UNORM,
    BUF_FORMAT_8_8_SNORM,
    BUF_FORMAT_8_8_USCALED,
    BUF_FORMAT_8_8_SSCALED,
    BUF_FORMAT_8_8_UINT,
    BUF_FORMAT_8_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_UINT,
    BUF_FORMAT_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_FLOAT,

    //BUF_DATA_FORMAT_16_16
    BUF_FORMAT_16_16_UNORM,
    BUF_FORMAT_16_16_SNORM,
    BUF_FORMAT_16_16_USCALED,
    BUF_FORMAT_16_16_SSCALED,
    BUF_FORMAT_16_16_UINT,
    BUF_FORMAT_16_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_16_FLOAT,

    //BUF_DATA_FORMAT_10_11_11
    BUF_FORMAT_10_11_11_UNORM,
    BUF_FORMAT_10_11_11_SNORM,
    BUF_FORMAT_10_11_11_USCALED,
    BUF_FORMAT_10_11_11_SSCALED,
    BUF_FORMAT_10_11_11_UINT,
    BUF_FORMAT_10_11_11_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_10_11_11_FLOAT,

    //BUF_DATA_FORMAT_11_11_10
    BUF_FORMAT_11_11_10_UNORM,
    BUF_FORMAT_11_11_10_SNORM,
    BUF_FORMAT_11_11_10_USCALED,
    BUF_FORMAT_11_11_10_SSCALED,
    BUF_FORMAT_11_11_10_UINT,
    BUF_FORMAT_11_11_10_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_11_11_10_FLOAT,

    //BUF_DATA_FORMAT_10_10_10_2
    BUF_FORMAT_10_10_10_2_UNORM,
    BUF_FORMAT_10_10_10_2_SNORM,
    BUF_FORMAT_10_10_10_2_USCALED,
    BUF_FORMAT_10_10_10_2_SSCALED,
    BUF_FORMAT_10_10_10_2_UINT,
    BUF_FORMAT_10_10_10_2_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_2_10_10_10
    BUF_FORMAT_2_10_10_10_UNORM,
    BUF_FORMAT_2_10_10_10_SNORM,
    BUF_FORMAT_2_10_10_10_USCALED,
    BUF_FORMAT_2_10_10_10_SSCALED,
    BUF_FORMAT_2_10_10_10_UINT,
    BUF_FORMAT_2_10_10_10_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_8_8_8_8
    BUF_FORMAT_8_8_8_8_UNORM,
    BUF_FORMAT_8_8_8_8_SNORM,
    BUF_FORMAT_8_8_8_8_USCALED,
    BUF_FORMAT_8_8_8_8_SSCALED,
    BUF_FORMAT_8_8_8_8_UINT,
    BUF_FORMAT_8_8_8_8_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,

    //BUF_DATA_FORMAT_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_UINT,
    BUF_FORMAT_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_FLOAT,

    //BUF_DATA_FORMAT_16_16_16_16
    BUF_FORMAT_16_16_16_16_UNORM,
    BUF_FORMAT_16_16_16_16_SNORM,
    BUF_FORMAT_16_16_16_16_USCALED,
    BUF_FORMAT_16_16_16_16_SSCALED,
    BUF_FORMAT_16_16_16_16_UINT,
    BUF_FORMAT_16_16_16_16_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_16_16_16_16_FLOAT,

    //BUF_DATA_FORMAT_32_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_UINT,
    BUF_FORMAT_32_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_FLOAT,

    //BUF_DATA_FORMAT_32_32_32_32
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_32_UINT,
    BUF_FORMAT_32_32_32_32_SINT,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_32_32_32_32_FLOAT,

    //BUF_DATA_FORMAT_RESERVED_15
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
    BUF_FORMAT_INVALID,
};
#endif

// =====================================================================================================================
VertexFetch::VertexFetch(
    Function*           pEntryPoint,      // [in] Entry-point of API vertex shader
    ShaderSystemValues* pShaderSysValues, // [in] ShaderSystemValues object for getting vertex buffer pointer from
    PipelineState*      pPipelineState)   // [in] Pipeline state
    :
    m_pModule(pEntryPoint->getParent()),
    m_pContext(static_cast<Context*>(&m_pModule->getContext())),
    m_pShaderSysValues(pShaderSysValues),
    m_pPipelineState(pPipelineState),
    m_pVertexInput(static_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo())->pVertexInput)
{
    LLPC_ASSERT(GetShaderStageFromFunction(pEntryPoint) == ShaderStageVertex); // Must be vertex shader

    auto& entryArgIdxs = m_pContext->GetShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
    auto& builtInUsage = m_pContext->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
    auto pInsertPos = pEntryPoint->begin()->getFirstInsertionPt();

    m_pVertexDivisor = nullptr;
    if (m_pVertexInput != nullptr)
    {
        m_pVertexDivisor = FindVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
            m_pVertexInput->pNext);
    }

    // VertexIndex = BaseVertex + VertexID
    if (builtInUsage.vertexIndex)
    {
        auto pBaseVertex = GetFunctionArgument(pEntryPoint, entryArgIdxs.baseVertex);
        auto pVertexId   = GetFunctionArgument(pEntryPoint, entryArgIdxs.vertexId);
        m_pVertexIndex = BinaryOperator::CreateAdd(pBaseVertex, pVertexId, "", &*pInsertPos);
    }

    // InstanceIndex = BaseInstance + InstanceID
    if (builtInUsage.instanceIndex)
    {
        m_pBaseInstance = GetFunctionArgument(pEntryPoint, entryArgIdxs.baseInstance);
        m_pInstanceId   = GetFunctionArgument(pEntryPoint, entryArgIdxs.instanceId);
        m_pInstanceIndex = BinaryOperator::CreateAdd(m_pBaseInstance, m_pInstanceId, "", &*pInsertPos);
    }

    // Initialize default fetch values
    auto pZero = ConstantInt::get(m_pContext->Int32Ty(), 0);
    auto pOne = ConstantInt::get(m_pContext->Int32Ty(), 1);

    // Int8 (0, 0, 0, 1)
    m_fetchDefaults.pInt8 = ConstantVector::get({ pZero, pZero, pZero, pOne });

    // Int16 (0, 0, 0, 1)
    m_fetchDefaults.pInt16 = ConstantVector::get({ pZero, pZero, pZero, pOne });

    // Int (0, 0, 0, 1)
    m_fetchDefaults.pInt = ConstantVector::get({ pZero, pZero, pZero, pOne });

    // Int64 (0, 0, 0, 1)
    m_fetchDefaults.pInt64 = ConstantVector::get({ pZero, pZero, pZero, pZero, pZero, pZero, pZero, pOne });

    // Float16 (0, 0, 0, 1.0)
    const uint16_t float16One = 0x3C00;
    auto pFloat16One = ConstantInt::get(m_pContext->Int32Ty(), float16One);
    m_fetchDefaults.pFloat16 = ConstantVector::get({ pZero, pZero, pZero, pFloat16One });

    // Float (0.0, 0.0, 0.0, 1.0)
    union
    {
        float    f;
        uint32_t u32;
    } floatOne = { 1.0f };
    auto pFloatOne = ConstantInt::get(m_pContext->Int32Ty(), floatOne.u32);
    m_fetchDefaults.pFloat = ConstantVector::get({ pZero, pZero, pZero, pFloatOne });

    // Double (0.0, 0.0, 0.0, 1.0)
    union
    {
        double   d;
        uint32_t u32[2];
    } doubleOne = { 1.0 };
    auto pDoubleOne0 = ConstantInt::get(m_pContext->Int32Ty(), doubleOne.u32[0]);
    auto pDoubleOne1 = ConstantInt::get(m_pContext->Int32Ty(), doubleOne.u32[1]);
    m_fetchDefaults.pDouble = ConstantVector::get({ pZero, pZero,
                                                    pZero, pZero,
                                                    pZero, pZero,
                                                    pDoubleOne0, pDoubleOne1 });
}

// =====================================================================================================================
// Executes vertex fetch operations based on the specified vertex input type and its location.
Value* VertexFetch::Run(
    Type*        pInputTy,      // [in] Type of vertex input
    uint32_t     location,      // Location of vertex input
    uint32_t     compIdx,       // Index used for vector element indexing
    Instruction* pInsertPos)    // [in] Where to insert vertex fetch instructions
{
    Value* pVertex = nullptr;

    const VkVertexInputBindingDescription*   pBinding = nullptr;
    const VkVertexInputAttributeDescription* pAttrib  = nullptr;
    const VkVertexInputBindingDivisorDescriptionEXT* pDivisor = nullptr;
    ExtractVertexInputInfo(location, &pBinding, &pAttrib, &pDivisor);

    // NOTE: If we could not find vertex input info matching this location, just return undefined value.
    if (pBinding == nullptr)
    {
        return UndefValue::get(pInputTy);
    }

    auto pVbDesc = LoadVertexBufferDescriptor(pBinding->binding, pInsertPos);

    Value* pVbIndex = nullptr;
    if (pBinding->inputRate == VK_VERTEX_INPUT_RATE_VERTEX)
    {
        pVbIndex = GetVertexIndex(); // Use vertex index
    }
    else
    {
        LLPC_ASSERT(pBinding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
        if (pDivisor != nullptr)
        {
            if (pDivisor->divisor == 0)
            {
                // All instances get the same VB record index
                pVbIndex = m_pBaseInstance;
            }
            else
            {
                pVbIndex = BinaryOperator::CreateUDiv(m_pInstanceId,
                                                      ConstantInt::get(m_pContext->Int32Ty(), pDivisor->divisor),
                                                      "",
                                                      pInsertPos);
                pVbIndex = BinaryOperator::CreateAdd(pVbIndex, m_pBaseInstance, "", pInsertPos);
            }
        }
        else
        {
            pVbIndex = GetInstanceIndex(); // Use instance index
        }
    }

    Value* vertexFetch[2] = {}; // Two vertex fetch operations might be required
    Value* pVertexFetch = nullptr; // Coalesced vector by combining the results of two vertex fetch operations

    const VertexFormatInfo* pFormatInfo = GetVertexFormatInfo(pAttrib->format);

    const bool is8bitFetch = (pInputTy->getScalarSizeInBits() == 8);
    const bool is16bitFetch = (pInputTy->getScalarSizeInBits() == 16);

    // Do the first vertex fetch operation
    AddVertexFetchInst(pVbDesc,
                       pFormatInfo->numChannels,
                       is16bitFetch,
                       pVbIndex,
                       pAttrib->offset,
                       pBinding->stride,
                       pFormatInfo->dfmt,
                       pFormatInfo->nfmt,
                       pInsertPos,
                       &vertexFetch[0]);

    // Do post-processing in certain cases
    std::vector<Constant*> shuffleMask;
    bool postShuffle = NeedPostShuffle(pAttrib->format, shuffleMask);
    bool patchA2S = NeedPatchA2S(pAttrib->format);
    if (postShuffle || patchA2S)
    {
        if (postShuffle)
        {
            // NOTE: If we are fetching a swizzled format, we have to add an extra "shufflevector" instruction to
            // get the components in the right order.
            LLPC_ASSERT(shuffleMask.empty() == false);
            vertexFetch[0] = new ShuffleVectorInst(vertexFetch[0],
                                                   vertexFetch[0],
                                                   ConstantVector::get(shuffleMask),
                                                   "",
                                                   pInsertPos);
        }

        if (patchA2S)
        {
            LLPC_ASSERT(vertexFetch[0]->getType()->getVectorNumElements() == 4);

            // Extract alpha channel: %a = extractelement %vf0, 3
            Value* pAlpha = ExtractElementInst::Create(vertexFetch[0],
                                                       ConstantInt::get(m_pContext->Int32Ty(), 3),
                                                       "",
                                                       pInsertPos);

            if (pFormatInfo->nfmt == BUF_NUM_FORMAT_SINT)
            {
                // NOTE: For format "SINT 10_10_10_2", vertex fetches incorrectly return the alpha channel as
                // unsigned. We have to manually sign-extend it here by doing a "shl" 30 then an "ashr" 30.

                // %a = shl %a, 30
                pAlpha = BinaryOperator::CreateShl(pAlpha,
                                                   ConstantInt::get(m_pContext->Int32Ty(), 30),
                                                   "",
                                                   pInsertPos);

                // %a = ashr %a, 30
                pAlpha = BinaryOperator::CreateAShr(pAlpha,
                                                    ConstantInt::get(m_pContext->Int32Ty(), 30),
                                                    "",
                                                    pInsertPos);
            }
            else if (pFormatInfo->nfmt == BUF_NUM_FORMAT_SNORM)
            {
                // NOTE: For format "SNORM 10_10_10_2", vertex fetches incorrectly return the alpha channel
                // as unsigned. We have to somehow remap the values { 0.0, 0.33, 0.66, 1.00 } to { 0.0, 1.0,
                // -1.0, -1.0 } respectively.

                // %a = bitcast %a to f32
                pAlpha = new BitCastInst(pAlpha, m_pContext->FloatTy(), "", pInsertPos);

                // %a = mul %a, 3.0f
                pAlpha = BinaryOperator::CreateFMul(pAlpha,
                                                   ConstantFP::get(m_pContext->FloatTy(), 3.0f),
                                                   "",
                                                   pInsertPos);

                // %cond = ugt %a, 1.5f
                auto pCond = new FCmpInst(pInsertPos,
                                          FCmpInst::FCMP_UGT,
                                          pAlpha,
                                          ConstantFP::get(m_pContext->FloatTy(), 1.5f),
                                          "");

                // %a = select %cond, -1.0f, pAlpha
                pAlpha = SelectInst::Create(pCond,
                                            ConstantFP::get(m_pContext->FloatTy(), -1.0f),
                                            pAlpha,
                                            "",
                                            pInsertPos);

                // %a = bitcast %a to i32
                pAlpha = new BitCastInst(pAlpha, m_pContext->Int32Ty(), "", pInsertPos);
            }
            else if (pFormatInfo->nfmt == BUF_NUM_FORMAT_SSCALED)
            {
                // NOTE: For format "SSCALED 10_10_10_2", vertex fetches incorrectly return the alpha channel
                // as unsigned. We have to somehow remap the values { 0.0, 1.0, 2.0, 3.0 } to { 0.0, 1.0,
                // -2.0, -1.0 } respectively. We can perform the sign extension here by doing a "fptosi", "shl" 30,
                // "ashr" 30, and finally "sitofp".

               // %a = bitcast %a to float
                pAlpha = new BitCastInst(pAlpha, m_pContext->FloatTy(), "", pInsertPos);

                // %a = fptosi %a to i32
                pAlpha = new FPToSIInst(pAlpha, m_pContext->Int32Ty(), "", pInsertPos);

                // %a = shl %a, 30
                pAlpha = BinaryOperator::CreateShl(pAlpha,
                                                   ConstantInt::get(m_pContext->Int32Ty(), 30),
                                                   "",
                                                   pInsertPos);

                // %a = ashr a, 30
                pAlpha = BinaryOperator::CreateAShr(pAlpha,
                                                    ConstantInt::get(m_pContext->Int32Ty(), 30),
                                                    "",
                                                    pInsertPos);

                // %a = sitofp %a to float
                pAlpha = new SIToFPInst(pAlpha, m_pContext->FloatTy(), "", pInsertPos);

                // %a = bitcast %a to i32
                pAlpha = new BitCastInst(pAlpha, m_pContext->Int32Ty(), "", pInsertPos);
            }
            else
            {
                LLPC_NEVER_CALLED();
            }

            // Insert alpha channel: %vf0 = insertelement %vf0, %a, 3
            vertexFetch[0] = InsertElementInst::Create(vertexFetch[0],
                                                       pAlpha,
                                                       ConstantInt::get(m_pContext->Int32Ty(), 3),
                                                       "",
                                                       pInsertPos);
        }
    }

    // Do the second vertex fetch operation
    const bool secondFetch = NeedSecondVertexFetch(pAttrib->format);
    if (secondFetch)
    {
        uint32_t numChannels = pFormatInfo->numChannels;
        uint32_t dfmt = pFormatInfo->dfmt;

        if ((pAttrib->format == VK_FORMAT_R64G64B64_UINT) ||
            (pAttrib->format == VK_FORMAT_R64G64B64_SINT) ||
            (pAttrib->format == VK_FORMAT_R64G64B64_SFLOAT))
        {
            // Valid number of channels and data format have to be revised
            numChannels = 2;
            dfmt = BUF_DATA_FORMAT_32_32;
        }

        AddVertexFetchInst(pVbDesc,
                           numChannels,
                           is16bitFetch,
                           pVbIndex,
                           pAttrib->offset + SizeOfVec4,
                           pBinding->stride,
                           dfmt,
                           pFormatInfo->nfmt,
                           pInsertPos,
                           &vertexFetch[1]);
    }

    if (secondFetch)
    {
        // NOTE: If we performs vertex fetch operations twice, we have to coalesce result values of the two
        // fetch operations and generate a combined one.
        LLPC_ASSERT((vertexFetch[0] != nullptr) && (vertexFetch[1] != nullptr));
        LLPC_ASSERT(vertexFetch[0]->getType()->getVectorNumElements() == 4);

        uint32_t compCount = vertexFetch[1]->getType()->getVectorNumElements();
        LLPC_ASSERT((compCount == 2) || (compCount == 4)); // Should be <2 x i32> or <4 x i32>

        if (compCount == 2)
        {
            // NOTE: We have to enlarge the second vertex fetch, from <2 x i32> to <4 x i32>. Otherwise,
            // vector shuffle operation could not be performed in that it requires the two vectors have
            // the same types.

            // %vf1 = shufflevector %vf1, %vf1, <0, 1, undef, undef>
            Constant* shuffleMask[] = {
                ConstantInt::get(m_pContext->Int32Ty(), 0),
                ConstantInt::get(m_pContext->Int32Ty(), 1),
                UndefValue::get(m_pContext->Int32Ty()),
                UndefValue::get(m_pContext->Int32Ty())
            };
            vertexFetch[1] = new ShuffleVectorInst(vertexFetch[1],
                                                   vertexFetch[1],
                                                   ConstantVector::get(shuffleMask),
                                                   "",
                                                   pInsertPos);
        }

        // %vf = shufflevector %vf0, %vf1, <0, 1, 2, 3, 4, 5, ...>
        shuffleMask.clear();
        for (uint32_t i = 0; i < 4 + compCount; ++i)
        {
            shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), i));
        }
        pVertexFetch = new ShuffleVectorInst(vertexFetch[0],
                                             vertexFetch[1],
                                             ConstantVector::get(shuffleMask),
                                             "",
                                             pInsertPos);
    }
    else
    {
        pVertexFetch = vertexFetch[0];
    }

    // Finalize vertex fetch
    Type* pBasicTy = pInputTy->isVectorTy() ? pInputTy->getVectorElementType() : pInputTy;
    const uint32_t bitWidth = pBasicTy->getScalarSizeInBits();
    LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32) || (bitWidth == 64));

    // Get default fetch values
    Constant* pDefaults = nullptr;

    if (pBasicTy->isIntegerTy())
    {
        if (bitWidth == 8)
        {
            pDefaults = m_fetchDefaults.pInt8;
        }
        else if (bitWidth == 16)
        {
            pDefaults = m_fetchDefaults.pInt16;
        }
        else if (bitWidth == 32)
        {
            pDefaults = m_fetchDefaults.pInt;
        }
        else
        {
            LLPC_ASSERT(bitWidth == 64);
            pDefaults = m_fetchDefaults.pInt64;
        }
    }
    else if (pBasicTy->isFloatingPointTy())
    {
        if (bitWidth == 16)
        {
            pDefaults = m_fetchDefaults.pFloat16;
        }
        else if (bitWidth == 32)
        {
            pDefaults = m_fetchDefaults.pFloat;
        }
        else
        {
            LLPC_ASSERT(bitWidth == 64);
            pDefaults = m_fetchDefaults.pDouble;
        }
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    const uint32_t defaultCompCount = pDefaults->getType()->getVectorNumElements();
    std::vector<Value*> defaultValues(defaultCompCount);

    for (uint32_t i = 0; i < defaultValues.size(); ++i)
    {
        defaultValues[i] = ExtractElementInst::Create(pDefaults,
                                                      ConstantInt::get(m_pContext->Int32Ty(), i),
                                                      "",
                                                      pInsertPos);
    }

    // Get vertex fetch values
    const uint32_t fetchCompCount = pVertexFetch->getType()->isVectorTy() ?
                                        pVertexFetch->getType()->getVectorNumElements() : 1;
    std::vector<Value*> fetchValues(fetchCompCount);

    if (fetchCompCount == 1)
    {
        fetchValues[0] = pVertexFetch;
    }
    else
    {
        for (uint32_t i = 0; i < fetchCompCount; ++i)
        {
            fetchValues[i] = ExtractElementInst::Create(pVertexFetch,
                                                        ConstantInt::get(m_pContext->Int32Ty(), i),
                                                        "",
                                                        pInsertPos);
        }
    }

    // Construct vertex fetch results
    const uint32_t inputCompCount = pInputTy->isVectorTy() ? pInputTy->getVectorNumElements() : 1;
    const uint32_t vertexCompCount = inputCompCount * ((bitWidth == 64) ? 2 : 1);

    std::vector<Value*> vertexValues(vertexCompCount);

    // NOTE: Original component index is based on the basic scalar type.
    compIdx *= ((bitWidth == 64) ? 2 : 1);

    // Vertex input might take values from vertex fetch values or default fetch values
    for (uint32_t i = 0; i < vertexCompCount; i++)
    {
        if (compIdx + i < fetchCompCount)
        {
            vertexValues[i] = fetchValues[compIdx + i];
        }
        else if (compIdx + i < defaultCompCount)
        {
            vertexValues[i] = defaultValues[compIdx + i];
        }
        else
        {
            LLPC_NEVER_CALLED();
            vertexValues[i] = UndefValue::get(m_pContext->Int32Ty());
        }
    }

    if (vertexCompCount == 1)
    {
        pVertex = vertexValues[0];
    }
    else
    {
        Type* pVertexTy = VectorType::get(m_pContext->Int32Ty(), vertexCompCount);
        pVertex = UndefValue::get(pVertexTy);

        for (uint32_t i = 0; i < vertexCompCount; ++i)
        {
            pVertex = InsertElementInst::Create(pVertex,
                                                vertexValues[i],
                                                ConstantInt::get(m_pContext->Int32Ty(), i),
                                                "",
                                                pInsertPos);
        }
    }

    if (is8bitFetch)
    {
        // NOTE: The vertex fetch results are represented as <n x i32> now. For 8-bit vertex fetch, we have to
        // convert them to <n x i8> and the 24 high bits is truncated.
        LLPC_ASSERT(pInputTy->isIntOrIntVectorTy()); // Must be integer type

        Type* pVertexTy = pVertex->getType();
        pVertexTy = pVertexTy->isVectorTy() ?
                        VectorType::get(m_pContext->Int8Ty(), pVertexTy->getVectorNumElements()) :
                        m_pContext->Int8Ty();
        pVertex = new TruncInst(pVertex, pVertexTy, "", pInsertPos);
    }
    else if (is16bitFetch)
    {
        // NOTE: The vertex fetch results are represented as <n x i32> now. For 16-bit vertex fetch, we have to
        // convert them to <n x i16> and the 16 high bits is truncated.
        Type* pVertexTy = pVertex->getType();
        pVertexTy = pVertexTy->isVectorTy() ?
                        VectorType::get(m_pContext->Int16Ty(), pVertexTy->getVectorNumElements()) :
                        m_pContext->Int16Ty();
        pVertex = new TruncInst(pVertex, pVertexTy, "", pInsertPos);
    }

    return pVertex;
}

// =====================================================================================================================
// Gets info from table according to vertex attribute format.
const VertexFormatInfo* VertexFetch::GetVertexFormatInfo(
    VkFormat format) // Vertex attribute format
{
    LLPC_ASSERT(format < VK_FORMAT_RANGE_SIZE);

    const VertexFormatInfo* pFormatInfo = &m_vertexFormatInfo[format];
    LLPC_ASSERT(pFormatInfo->format == format);

    return pFormatInfo;
}

// =====================================================================================================================
// Gets component info from table according to vertex buffer data format.
const VertexCompFormatInfo* VertexFetch::GetVertexComponentFormatInfo(
    uint32_t dfmt) // Date format of vertex buffer
{
    LLPC_ASSERT(dfmt < sizeof(m_vertexCompFormatInfo) / sizeof(m_vertexCompFormatInfo[0]));
    return &m_vertexCompFormatInfo[dfmt];
}

// =====================================================================================================================
// Maps separate buffer data and numeric formats to the combined buffer format
uint32_t VertexFetch::MapVertexFormat(
    uint32_t dfmt,  // Data format
    uint32_t nfmt   // Numeric format
    ) const
{
    LLPC_ASSERT(dfmt < 16);
    LLPC_ASSERT(nfmt < 8);
    uint32_t format = 0;

#if LLPC_BUILD_GFX10
    GfxIpVersion gfxIp = m_pContext->GetGfxIpVersion();
    if (gfxIp.major >= 10)
    {
        uint32_t index = (dfmt * 8) + nfmt;
        LLPC_ASSERT(index < sizeof(m_vertexFormatMap) / sizeof(m_vertexFormatMap[0]));
        format = m_vertexFormatMap[index];
    }
    else
#endif
    {
        CombineFormat formatOprd = {};
        formatOprd.bits.dfmt = dfmt;
        formatOprd.bits.nfmt = nfmt;
        format = formatOprd.u32All;
    }
    return format;
}

// =====================================================================================================================
// Loads vertex descriptor based on the specified vertex input location.
Value* VertexFetch::LoadVertexBufferDescriptor(
    uint32_t     binding,       // ID of vertex buffer binding
    Instruction* pInsertPos     // [in] Where to insert instructions
    ) const
{
    Value* idxs[] = {
        ConstantInt::get(m_pContext->Int64Ty(), 0, false),
        ConstantInt::get(m_pContext->Int64Ty(), binding, false)
    };

    auto pVbTablePtr = m_pShaderSysValues->GetVertexBufTablePtr();
    auto pVbDescPtr = GetElementPtrInst::Create(nullptr, pVbTablePtr, idxs, "", pInsertPos);
    pVbDescPtr->setMetadata(m_pContext->MetaIdUniform(), m_pContext->GetEmptyMetadataNode());

    auto pVbDesc = new LoadInst(pVbDescPtr, "", pInsertPos);
    pVbDesc->setMetadata(m_pContext->MetaIdInvariantLoad(), m_pContext->GetEmptyMetadataNode());
    pVbDesc->setAlignment(MaybeAlign(16));

    return pVbDesc;
}

// =====================================================================================================================
// Extracts vertex input binding and attribute info based on the specified vertex input location.
void VertexFetch::ExtractVertexInputInfo(
    uint32_t                                          location,   // Location of vertex input
    const VkVertexInputBindingDescription**           ppBinding,  // [out] Vertex binding
    const VkVertexInputAttributeDescription**         ppAttrib,   // [out] Vertex attribute
    const VkVertexInputBindingDivisorDescriptionEXT** ppDivisor   // [out] Vertex divisor
    ) const
{
    LLPC_ASSERT((ppBinding != nullptr) && (ppAttrib != nullptr));

    *ppBinding = nullptr;
    *ppAttrib  = nullptr;
    *ppDivisor = nullptr;

    for (uint32_t i = 0; i < m_pVertexInput->vertexAttributeDescriptionCount; ++i)
    {
        auto pAttrib = &m_pVertexInput->pVertexAttributeDescriptions[i];
        if (pAttrib->location == location)
        {
            *ppAttrib = pAttrib;
            break;
        }
    }

    if (*ppAttrib != nullptr) // Vertex attribute exists
    {
        for (uint32_t i = 0; i < m_pVertexInput->vertexBindingDescriptionCount; ++i)
        {
            auto pBinding = &m_pVertexInput->pVertexBindingDescriptions[i];
            if (pBinding->binding == (*ppAttrib)->binding)
            {
                *ppBinding = pBinding;
                break;
            }
        }
        LLPC_ASSERT(*ppBinding != nullptr); // Vertex binding exists

        if (m_pVertexDivisor != nullptr)
        {
            for (uint32_t i = 0;i < m_pVertexDivisor->vertexBindingDivisorCount; ++i)
            {
                auto pDivisor = &m_pVertexDivisor->pVertexBindingDivisors[i];
                if (pDivisor->binding == (*ppAttrib)->binding)
                {
                    *ppDivisor = pDivisor;
                    break;
                }
            }
        }
    }
}

// =====================================================================================================================
// Inserts instructions to do vertex fetch operations.
void VertexFetch::AddVertexFetchInst(
    Value*       pVbDesc,       // [in] Vertex buffer descriptor
    uint32_t     numChannels,   // Valid number of channels
    bool         is16bitFetch,  // Whether it is 16-bit vertex fetch
    Value*       pVbIndex,      // [in] Index of vertex fetch in buffer
    uint32_t     offset,        // Vertex attribute offset (in bytes)
    uint32_t     stride,        // Vertex attribute stride (in bytes)
    uint32_t     dfmt,          // Date format of vertex buffer
    uint32_t     nfmt,          // Numeric format of vertex buffer
    Instruction* pInsertPos,    // [in] Where to insert instructions
    Value**      ppFetch        // [out] Destination of vertex fetch
    ) const
{
    const VertexCompFormatInfo* pFormatInfo = GetVertexComponentFormatInfo(dfmt);

    // NOTE: If the vertex attribute offset and stride are aligned on data format boundaries, we can do a vertex fetch
    // operation to read the whole vertex. Otherwise, we have to do vertex per-component fetch operations.
    if ((((offset % pFormatInfo->vertexByteSize) == 0) && ((stride % pFormatInfo->vertexByteSize) == 0)) ||
        (pFormatInfo->compDfmt == dfmt))
    {
        // NOTE: If the vertex attribute offset is greater than vertex attribute stride, we have to adjust both vertex
        // buffer index and vertex attribute offset accordingly. Otherwise, vertex fetch might behave unexpectedly.
        if ((stride != 0) && (offset > stride))
        {
            pVbIndex = BinaryOperator::CreateAdd(pVbIndex,
                                                 ConstantInt::get(m_pContext->Int32Ty(), offset / stride),
                                                 "",
                                                 pInsertPos);
            offset = offset % stride;
        }

        // Do vertex fetch
        Value* args[] = {
            pVbDesc,                                                                // rsrc
            pVbIndex,                                                               // vindex
            ConstantInt::get(m_pContext->Int32Ty(), offset),                        // offset
            ConstantInt::get(m_pContext->Int32Ty(), 0),                             // soffset
            ConstantInt::get(m_pContext->Int32Ty(), MapVertexFormat(dfmt, nfmt)),   // dfmt, nfmt
            ConstantInt::get(m_pContext->Int32Ty(), 0)                              // glc, slc
        };

        StringRef suffix = "";
        Type* pFetchTy = nullptr;

        if (is16bitFetch)
        {
            switch (numChannels)
            {
            case 1:
                suffix = ".f16";
                pFetchTy = m_pContext->Float16Ty();
                break;
            case 2:
                suffix = ".v2f16";
                pFetchTy = m_pContext->Float16x2Ty();
                break;
            case 3:
            case 4:
                suffix = ".v4f16";
                pFetchTy = m_pContext->Float16x4Ty();
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }
        }
        else
        {
            switch (numChannels)
            {
            case 1:
                suffix = ".i32";
                pFetchTy = m_pContext->Int32Ty();
                break;
            case 2:
                suffix = ".v2i32";
                pFetchTy = m_pContext->Int32x2Ty();
                break;
            case 3:
            case 4:
                suffix = ".v4i32";
                pFetchTy = m_pContext->Int32x4Ty();
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }
        }

        Value* pFetch = EmitCall((Twine("llvm.amdgcn.struct.tbuffer.load") + suffix).str(),
                                 pFetchTy,
                                 args,
                                 NoAttrib,
                                 pInsertPos);

        if (is16bitFetch)
        {
            // NOTE: The fetch values are represented by <n x i32>, so we will bitcast the float16 values to
            // int32 eventually.
            pFetch = new BitCastInst(pFetch,
                                        (numChannels == 1) ?
                                            m_pContext->Int16Ty() :
                                            VectorType::get(m_pContext->Int16Ty(), numChannels),
                                        "",
                                        pInsertPos);

            pFetch = new ZExtInst(pFetch,
                                    (numChannels == 1) ?
                                        m_pContext->Int32Ty() :
                                        VectorType::get(m_pContext->Int32Ty(), numChannels),
                                    "",
                                    pInsertPos);
        }

        if (numChannels == 3)
        {
            // NOTE: If valid number of channels is 3, the actual fetch type should be revised from <4 x i32>
            // to <3 x i32>.
            Constant* shuffleMask[] = {
                ConstantInt::get(m_pContext->Int32Ty(), 0),
                ConstantInt::get(m_pContext->Int32Ty(), 1),
                ConstantInt::get(m_pContext->Int32Ty(), 2)
            };
            *ppFetch = new ShuffleVectorInst(pFetch, pFetch, ConstantVector::get(shuffleMask), "", pInsertPos);
        }
        else
        {
            *ppFetch = pFetch;
        }
    }
    else
    {
        // NOTE: Here, we split the vertex into its components and do per-component fetches. The expectation
        // is that the vertex per-component fetches always match the hardware requirements.
        LLPC_ASSERT(numChannels == pFormatInfo->compCount);

        Value* compVbIndices[4]  = {};
        uint32_t compOffsets[4] = {};

        for (uint32_t i = 0; i < pFormatInfo->compCount; ++i)
        {
            uint32_t compOffset = offset + i * pFormatInfo->compByteSize;

            // NOTE: If the vertex attribute per-component offset is greater than vertex attribute stride, we have
            // to adjust both vertex buffer index and vertex per-component offset accordingly. Otherwise, vertex
            // fetch might behave unexpectedly.
            if ((stride != 0) && (compOffset > stride))
            {
                compVbIndices[i] = BinaryOperator::CreateAdd(
                                       pVbIndex,
                                       ConstantInt::get(m_pContext->Int32Ty(), compOffset / stride),
                                       "",
                                       pInsertPos);
                compOffsets[i] = compOffset % stride;
            }
            else
            {
                compVbIndices[i] = pVbIndex;
                compOffsets[i] = compOffset;
            }
        }

        Type* pFetchTy = VectorType::get(m_pContext->Int32Ty(), numChannels);
        Value* pFetch = UndefValue::get(pFetchTy);

        // Do vertex per-component fetches
        for (uint32_t i = 0; i < pFormatInfo->compCount; ++i)
        {
            Value* args[] = {
                pVbDesc,                                                        // rsrc
                compVbIndices[i],                                               // vindex
                ConstantInt::get(m_pContext->Int32Ty(), compOffsets[i]),        // offset
                ConstantInt::get(m_pContext->Int32Ty(), 0),                     // soffset
                ConstantInt::get(m_pContext->Int32Ty(),
                                 MapVertexFormat(pFormatInfo->compDfmt, nfmt)), // dfmt, nfmt
                ConstantInt::get(m_pContext->Int32Ty(), 0)                      // glc, slc
            };

            Value* pCompFetch = nullptr;
            if (is16bitFetch)
            {
                pCompFetch = EmitCall("llvm.amdgcn.struct.tbuffer.load.f16",
                                      m_pContext->Float16Ty(),
                                      args,
                                      NoAttrib,
                                      pInsertPos);

                pCompFetch = new BitCastInst(pCompFetch, m_pContext->Int16Ty(), "", pInsertPos);
                pCompFetch = new ZExtInst(pCompFetch, m_pContext->Int32Ty(), "", pInsertPos);
            }
            else
            {
                pCompFetch = EmitCall("llvm.amdgcn.struct.tbuffer.load.i32",
                                      m_pContext->Int32Ty(),
                                      args,
                                      NoAttrib,
                                      pInsertPos);
            }

            pFetch = InsertElementInst::Create(pFetch,
                                               pCompFetch,
                                               ConstantInt::get(m_pContext->Int32Ty(), i),
                                               "",
                                               pInsertPos);
        }

        *ppFetch = pFetch;
    }
}

// =====================================================================================================================
// Checks whether post shuffle is required for vertex fetch oepration.
bool VertexFetch::NeedPostShuffle(
    VkFormat                format,     // Vertex attribute format
    std::vector<Constant*>& shuffleMask // [out] Vector shuffle mask
    ) const
{
    bool needShuffle = false;

    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_USCALED:
    case VK_FORMAT_B8G8R8A8_SSCALED:
    case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
    case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_SINT_PACK32:
        shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), 2));
        shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), 1));
        shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), 3));
        needShuffle = true;
        break;
    default:
        break;
    }

    return needShuffle;
}

// =====================================================================================================================
// Checks whether patching 2-bit signed alpha channel is required for vertex fetch operation.
bool VertexFetch::NeedPatchA2S(
    VkFormat format  // Vertex attribute format
    ) const
{
    bool needPatch = false;

    switch (format)
    {
    case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
    case VK_FORMAT_A2R10G10B10_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        needPatch = (m_pContext->GetGfxIpVersion().major < 9);
        break;
    default:
        break;
    }

    return needPatch;
}

// =====================================================================================================================
// Checks whether the second vertex fetch operation is required (particularly for certain 64-bit typed formats).
bool VertexFetch::NeedSecondVertexFetch(
    VkFormat format // Vertex attribute format
    ) const
{
    return ((format == VK_FORMAT_R64G64B64_UINT)        ||
            (format == VK_FORMAT_R64G64B64_SINT)        ||
            (format == VK_FORMAT_R64G64B64_SFLOAT)      ||
            (format == VK_FORMAT_R64G64B64A64_UINT)     ||
            (format == VK_FORMAT_R64G64B64A64_SINT)     ||
            (format == VK_FORMAT_R64G64B64A64_SFLOAT));
}

} // Llpc
