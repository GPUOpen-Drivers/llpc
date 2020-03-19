/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Defs.h
 * @brief LLPC header file: contains interface types exposed by both LGC interface and LLPC interface
 ***********************************************************************************************************************
 */
#pragma once

#include <stdint.h>

namespace Llpc
{

static const uint32_t  MaxColorTargets = 8;

/// Enumerates LLPC shader stages.
enum ShaderStage : uint32_t
{
    ShaderStageVertex = 0,                                ///< Vertex shader
    ShaderStageTessControl,                               ///< Tessellation control shader
    ShaderStageTessEval,                                  ///< Tessellation evaluation shader
    ShaderStageGeometry,                                  ///< Geometry shader
    ShaderStageFragment,                                  ///< Fragment shader
    ShaderStageCompute,                                   ///< Compute shader
    ShaderStageCount,                                     ///< Count of shader stages
    ShaderStageInvalid = ~0u,                             ///< Invalid shader stage
    ShaderStageNativeStageCount = ShaderStageCompute + 1, ///< Native supported shader stage count
    ShaderStageGfxCount = ShaderStageFragment + 1,        ///< Count of shader stages for graphics pipeline

    ShaderStageCopyShader = ShaderStageCount,             ///< Copy shader (internal-use)
    ShaderStageCountInternal,                             ///< Count of shader stages (internal-use)
};

/// Enumerates the function of a particular node in a shader's resource mapping graph.
enum class ResourceMappingNodeType : uint32_t
{
    Unknown,                        ///< Invalid type
    DescriptorResource,             ///< Generic descriptor: resource, including texture resource, image, input
                                    ///  attachment
    DescriptorSampler,              ///< Generic descriptor: sampler
    DescriptorCombinedTexture,      ///< Generic descriptor: combined texture, combining resource descriptor with
                                    ///  sampler descriptor of the same texture, starting with resource descriptor
    DescriptorTexelBuffer,          ///< Generic descriptor: texel buffer, including texture buffer and image buffer
    DescriptorFmask,                ///< Generic descriptor: F-mask
    DescriptorBuffer,               ///< Generic descriptor: buffer, including uniform buffer and shader storage buffer
    DescriptorTableVaPtr,           ///< Descriptor table VA pointer
    IndirectUserDataVaPtr,          ///< Indirect user data VA pointer
    PushConst,                      ///< Push constant
    DescriptorBufferCompact,        ///< Compact buffer descriptor, only contains the buffer address
    StreamOutTableVaPtr,            ///< Stream-out buffer table VA pointer
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 40
    DescriptorReserved12,
#elif LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 29
#endif
    DescriptorYCbCrSampler,         ///< Generic descriptor: YCbCr sampler
    Count,                          ///< Count of resource mapping node types.
};

/// Represents one node in a graph defining how the user data bound in a command buffer at draw/dispatch time maps to
/// resources referenced by a shader (t#, u#, etc.).
struct ResourceMappingNode
{
    ResourceMappingNodeType     type;   ///< Type of this node

    uint32_t    sizeInDwords;   ///< Size of this node in DWORD
    uint32_t    offsetInDwords; ///< Offset of this node (from the beginning of the resource mapping table) in DWORD

    union
    {
        /// Info for generic descriptor nodes (DescriptorResource, DescriptorSampler, DescriptorCombinedTexture,
        /// DescriptorTexelBuffer, DescriptorBuffer and DescriptorBufferCompact)
        struct
        {
            uint32_t                    set;         ///< Descriptor set
            uint32_t                    binding;     ///< Descriptor binding
        } srdRange;
        /// Info for hierarchical nodes (DescriptorTableVaPtr)
        struct
        {
            uint32_t                    nodeCount;  ///< Number of entries in the "pNext" array
            const ResourceMappingNode*  pNext;      ///< Array of node structures describing the next hierarchical
                                                    ///  level of mapping
        } tablePtr;
        /// Info for hierarchical nodes (IndirectUserDataVaPtr)
        struct
        {
            uint32_t                    sizeInDwords; ///< Size of the pointed table in DWORDS
        } userDataPtr;
    };
};

/// Represents the info of static descriptor.
struct DescriptorRangeValue
{
    ResourceMappingNodeType type;       ///< Type of this resource mapping node (currently, only sampler is supported)
    uint32_t                set;        ///< ID of descriptor set
    uint32_t                binding;    ///< ID of descriptor binding
    uint32_t                arraySize;  ///< Element count for arrayed binding
    const uint32_t*         pValue;     ///< Static SRDs
};

/// Represents the base data type
enum class BasicType : uint32_t
{
    Unknown = 0,          ///< Unknown
    Float,                ///< Float
    Double,               ///< Double
    Int,                  ///< Signed integer
    Uint,                 ///< Unsigned integer
    Int64,                ///< 64-bit signed integer
    Uint64,               ///< 64-bit unsigned integer
    Float16,              ///< 16-bit floating-point
    Int16,                ///< 16-bit signed integer
    Uint16,               ///< 16-bit unsigned integer
    Int8,                 ///< 8-bit signed integer
    Uint8,                ///< 8-bit unsigned integer
};

/// Enumerates various sizing options of sub-group size for NGG primitive shader.
enum class NggSubgroupSizingType : uint32_t
{
    Auto,                           ///< Sub-group size is allocated as optimally determined
    MaximumSize,                    ///< Sub-group size is allocated to the maximum allowable size by the hardware
    HalfSize,                       ///< Sub-group size is allocated as to allow half of the maximum allowable size
                                    ///  by the hardware
    OptimizeForVerts,               ///< Sub-group size is optimized for vertex thread utilization
    OptimizeForPrims,               ///< Sub-group size is optimized for primitive thread utilization
    Explicit,                       ///< Sub-group size is allocated based on explicitly-specified vertsPerSubgroup and
                                    ///  primsPerSubgroup
};

/// Enumerates compaction modes after culling operations for NGG primitive shader.
enum NggCompactMode : uint32_t
{
    NggCompactSubgroup,             ///< Compaction is based on the whole sub-group
    NggCompactVertices,             ///< Compaction is based on vertices
};

/// If next available quad falls outside tile aligned region of size defined by this enumeration the SC will force end
/// of vector in the SC to shader wavefront.
enum class WaveBreakSize : uint32_t
{
    None     = 0x0,        ///< No wave break by region
    _8x8     = 0x1,        ///< Outside a 8x8 pixel region
    _16x16   = 0x2,        ///< Outside a 16x16 pixel region
    _32x32   = 0x3,        ///< Outside a 32x32 pixel region
    DrawTime = 0xF,        ///< Choose wave break size per draw
};

/// Represents NGG tuning options
struct NggState
{
    bool    enableNgg;                  ///< Enable NGG mode, use an implicit primitive shader
    bool    enableGsUse;                ///< Enable NGG use on geometry shader
    bool    forceNonPassthrough;        ///< Force NGG to run in non pass-through mode
    bool    alwaysUsePrimShaderTable;   ///< Always use primitive shader table to fetch culling-control registers
    NggCompactMode compactMode;         ///< Compaction mode after culling operations

    bool    enableFastLaunch;           ///< Enable the hardware to launch subgroups of work at a faster rate
    bool    enableVertexReuse;          ///< Enable optimization to cull duplicate vertices
    bool    enableBackfaceCulling;      ///< Enable culling of primitives that don't meet facing criteria
    bool    enableFrustumCulling;       ///< Enable discarding of primitives outside of view frustum
    bool    enableBoxFilterCulling;     ///< Enable simpler frustum culler that is less accurate
    bool    enableSphereCulling;        ///< Enable frustum culling based on a sphere
    bool    enableSmallPrimFilter;      ///< Enable trivial sub-sample primitive culling
    bool    enableCullDistanceCulling;  ///< Enable culling when "cull distance" exports are present

    /// Following fields are used for NGG tuning
    uint32_t backfaceExponent;          ///< Value from 1 to UINT32_MAX that will cause the backface culling
                                        ///  algorithm to ignore area calculations that are less than
                                        ///  (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
                                        ///  Only valid if the NGG backface culler is enabled.
                                        ///  A value of 0 will disable the threshold.

    NggSubgroupSizingType subgroupSizing;   ///< NGG sub-group sizing type

    uint32_t primsPerSubgroup;          ///< Preferred number of GS primitives to pack into a primitive shader
                                        ///  sub-group

    uint32_t vertsPerSubgroup;          ///< Preferred number of vertices consumed by a primitive shader sub-group
};

} // Llpc
