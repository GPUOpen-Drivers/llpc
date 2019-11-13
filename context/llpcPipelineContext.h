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
 * @file  llpcPipelineContext.h
 * @brief LLPC header file: contains declaration of class Llpc::PipelineContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"

#include <unordered_map>
#include <unordered_set>
#include "spirvExt.h"

#include "llpc.h"
#include "llpcCompiler.h"
#include "llpcDebug.h"
#include "llpcInternal.h"
#include "llpcIntrinsDefs.h"
#include "llpcMetroHash.h"

namespace Llpc
{

class Builder;

// Enumerates types of descriptor.
enum class DescriptorType : uint32_t
{
    UniformBlock = 0,     // Uniform block
    ShaderStorageBlock,   // Shader storage block
    Texture,              // Combined texture
    TextureResource,      // Separated texture resource
    TextureSampler,       // Separated texture sampler
    TexelBuffer,          // Texture buffer and image buffer
    Image,                // Image
    SubpassInput,         // Subpass input
};

// Represents floating-point control setting.
struct FloatControl
{
    bool    denormPerserve;             // Preserve denormals
    bool    denormFlushToZero;          // Flush denormals to zero
    bool    signedZeroInfNanPreserve;   // Preserve signed zero/INF/NaN
    bool    roundingModeRTE;            // Rounding mode: to nearest even
    bool    roundingModeRTZ;            // Rounding mode: to zero
};

// Enumerate the workgroup layout options.
enum class WorkgroupLayout : uint32_t
{
    Unknown = 0,   // ?x?
    Linear,        // 4x1
    Quads,         // 2x2
    SexagintiQuads // 8x8
};

// Represents the info of a descriptor binding
struct DescriptorBinding
{
    DescriptorType descType;        // Type of the descriptor
    uint32_t       arraySize;       // Element count of arrayed binding (flattened)
    bool           isMultisampled;  // Whether multisampled texture is used
};

typedef std::vector<DescriptorBinding> DescriptorSet;

// Represents interpolation info of fragment shader input
struct FsInterpInfo
{
    uint32_t loc;           // Mapped input location (tightly packed)
    bool     flat;          // Whether it is "flat" interpolation
    bool     custom;        // Whether it is "custom" interpolation
    bool     is16bit;       // Whether it is 16-bit interpolation
};

// Invalid interpolation info
static const FsInterpInfo InvalidFsInterpInfo = { InvalidValue, false, false, false };

// Represents descriptor set/binding pair.
union DescriptorPair
{
    struct
    {
        uint32_t descSet;  // ID of descriptor set
        uint32_t binding;  // ID of descriptor binding
    };
    uint64_t u64All;
};

// Represents GS output location info (including location, built-in ID, and vertex stream ID)
//
// NOTE: Be careful to add new fields in this structure. It will be used as 32-bit hash map key in doing
// location map for GS. The change of 32-bit value has impacts on ordering of entries. Thus, the mapping
// result is changed accordingly.
union GsOutLocInfo
{
    struct
    {
        uint32_t location  : 16;    // Location of the output
        uint32_t isBuiltIn : 1;     // Whether location is actually built-in ID
        uint32_t streamId  : 2;     // Output vertex stream ID
    };
    uint32_t  u32All;
};

// Represents transform feedback output info
union XfbOutInfo
{
    struct
    {
        uint32_t xfbBuffer      : 2;   // Transform feedback buffer
        uint32_t xfbOffset      : 16;  // Transform feedback offset
        uint32_t xfbExtraOffset : 13;  // Transform feedback extra offset
        uint32_t is16bit        : 1;   // Whether it is 16-bit data for transform feedback
    };
    uint32_t u32All;
};

// Represents the usage info of shader resources.
//
// NOTE: All fields must be initialized in InitShaderResourceUsage().
struct ResourceUsage
{
    std::unordered_set<uint64_t> descPairs;           // Pairs of descriptor set/binding
    uint32_t                   pushConstSizeInBytes;  // Push constant size (in bytes)
    bool                       resourceWrite;         // Whether shader does resource-write operations (UAV)
    bool                       resourceRead;          // Whether shader does resource-read operrations (UAV)
    bool                       perShaderTable;        // Whether per shader stage table is used
    bool                       globalConstant;        // Whether global constant is used
    uint32_t                   numSgprsAvailable;     // Number of available SGPRs
    uint32_t                   numVgprsAvailable;     // Number of available VGPRs

    // Usage of built-ins
    struct
    {
        // Per-stage built-in usage
        union
        {
            // Vertex shader
            struct
            {
                // Input
                uint32_t vertexIndex          : 1;      // Whether gl_VertexIndex is used
                uint32_t instanceIndex        : 1;      // Whether gl_InstanceIndex is used
                uint32_t baseVertex           : 1;      // Whether gl_BaseVertex is used
                uint32_t baseInstance         : 1;      // Whether gl_BaseInstance is used
                uint32_t drawIndex            : 1;      // Whether gl_DrawID is used
                uint32_t primitiveId          : 1;      // Whether an implicit gl_PrimitiveID is required
                uint32_t viewIndex            : 1;      // Whether gl_ViewIndex is used
                // Output
                uint32_t pointSize            : 1;      // Whether gl_PointSize is used
                uint32_t position             : 1;      // Whether gl_Position is used
                uint32_t clipDistance         : 4;      // Array size of gl_ClipDistance[] (0 means unused)
                uint32_t cullDistance         : 4;      // Array size of gl_CullDistance[] (0 means unused)
                uint32_t viewportIndex        : 1;      // Whether gl_ViewportIndex is used
                uint32_t layer                : 1;      // Whether gl_Layer is used

                uint64_t unused               : 45;
            } vs;

            // Tessellation control shader
            struct
            {
                // Input
                uint32_t pointSizeIn          : 1;      // Whether gl_in[].gl_PointSize is used
                uint32_t positionIn           : 1;      // Whether gl_in[].gl_Position is used
                uint32_t clipDistanceIn       : 4;      // Array size of gl_in[].gl_ClipDistance[] (0 means unused)
                uint32_t cullDistanceIn       : 4;      // Array size of gl_in[].gl_CullDistance[] (0 means unused)
                uint32_t patchVertices        : 1;      // Whether gl_PatchVerticesIn is used
                uint32_t primitiveId          : 1;      // Whether gl_PrimitiveID is used
                uint32_t invocationId         : 1;      // Whether gl_InvocationID is used
                // Output
                uint32_t pointSize            : 1;      // Whether gl_out[].gl_PointSize is used
                uint32_t position             : 1;      // Whether gl_out[].gl_Position is used
                uint32_t clipDistance         : 4;      // Array size of gl_out[].gl_ClipDistance[] (0 means unused)
                uint32_t cullDistance         : 4;      // Array size of gl_out[].gl_CullDistance[] (0 means unused)
                uint32_t tessLevelOuter       : 1;      // Whether gl_TessLevelOuter[] is used
                uint32_t tessLevelInner       : 1;      // Whether gl_TessLevelInner[] is used
                // Execution mode (shared with tessellation evaluation shader)
                uint32_t vertexSpacing        : 2;      // Vertex spacing
                uint32_t vertexOrder          : 2;      // Vertex ordering
                uint32_t primitiveMode        : 2;      // Tesselllation primitive mode
                uint32_t pointMode            : 1;      // Whether point mode is specified
                uint32_t outputVertices       : 6;      // Number of produced vertices in the output patch

                uint64_t unused               : 26;
            } tcs;

            // Tessellation evaluation shader
            struct
            {
                // Input
                uint32_t pointSizeIn          : 1;      // Whether gl_in[].gl_PointSize is used
                uint32_t positionIn           : 1;      // Whether gl_in[].gl_Position is used
                uint32_t clipDistanceIn       : 4;      // Array size of gl_in[].gl_ClipDistance[] (0 means unused)
                uint32_t cullDistanceIn       : 4;      // Array size of gl_in[].gl_CullDistance[] (0 means unused)
                uint32_t patchVertices        : 1;      // Whether gl_PatchVerticesIn is used
                uint32_t primitiveId          : 1;      // Whether gl_PrimitiveID is used
                uint32_t tessCoord            : 1;      // Whether gl_TessCoord is used
                uint32_t tessLevelOuter       : 1;      // Whether gl_TessLevelOuter[] is used
                uint32_t tessLevelInner       : 1;      // Whether gl_TessLevelInner[] is used
                uint32_t viewIndex            : 1;      // Whether gl_ViewIndex is used
                // Output
                uint32_t pointSize            : 1;      // Whether gl_PointSize is used
                uint32_t position             : 1;      // Whether gl_Position is used
                uint32_t clipDistance         : 4;      // Array size gl_ClipDistance[] (0 means unused)
                uint32_t cullDistance         : 4;      // Array size gl_CullDistance[] (0 means unused)
                uint32_t viewportIndex        : 1;      // Whether gl_ViewportIndex is used
                uint32_t layer                : 1;      // Whether gl_Layer is used
                // Execution mode (shared with tessellation control shader)
                uint32_t vertexSpacing        : 2;      // Vertex spacing
                uint32_t vertexOrder          : 2;      // Vertex ordering
                uint32_t primitiveMode        : 2;      // Tesselllation primitive mode
                uint32_t pointMode            : 1;      // Whether point mode is specified
                uint32_t outputVertices       : 6;      // Number of produced vertices in the output patch

                uint64_t unused               : 23;
            } tes;

            // Geometry shader
            struct
            {
                // Input
                uint32_t pointSizeIn          : 1;      // Whether gl_in[].gl_PointSize is used
                uint32_t positionIn           : 1;      // Whether gl_in[].gl_Position is used
                uint32_t clipDistanceIn       : 4;      // Array size of gl_in[].gl_ClipDistance[] (0 means unused)
                uint32_t cullDistanceIn       : 4;      // Array size of gl_in[].gl_CullDistance[] (0 means unused)
                uint32_t primitiveIdIn        : 1;      // Whether gl_PrimitiveIDIn is used
                uint32_t invocationId         : 1;      // Whether gl_InvocationID is used
                uint32_t viewIndex            : 1;      // Whether gl_ViewIndex is used
                // Output
                uint32_t pointSize            : 1;      // Whether gl_PointSize is used
                uint32_t position             : 1;      // Whether gl_Position is used
                uint32_t clipDistance         : 4;      // Array size gl_ClipDistance[] (0 means unused)
                uint32_t cullDistance         : 4;      // Array size gl_CullDistance[] (0 means unused)
                uint32_t primitiveId          : 1;      // Whether gl_PrimitiveID is used
                uint32_t viewportIndex        : 1;      // Whether gl_ViewportIndex is used
                uint32_t layer                : 1;      // Whether gl_Layer is used
                // Execution mode
                uint32_t inputPrimitive       : 3;      // Type of input primitive
                uint32_t outputPrimitive      : 2;      // Type of output primitive
                uint32_t invocations          : 7;      // Number of times to invoke shader for each input primitive
                uint32_t outputVertices       : 11;     // Max number of vertices the shader will emit (one invocation)

                uint64_t unused               : 15;
            } gs;

            // Fragment shader
            struct
            {
                // Interpolation
                uint32_t smooth               : 1;      // Whether "smooth" qualifier is used
                uint32_t noperspective        : 1;      // Whether "noperspective" qualifier is used
                uint32_t flat                 : 1;      // Whether "flat" qualifier is used
                uint32_t centroid             : 1;      // Whether "centroid" qualifier is used
                uint32_t sample               : 1;      // Whether "sample" qualifier is used
                uint32_t center               : 1;      // Whether location qualifiers are not used (default: "center")
                uint32_t pullMode             : 1;      // Whether pull mode interpolation is used
                uint32_t custom               : 1;      // Whether custom interpolation is used
                // Input
                uint32_t fragCoord            : 1;      // Whether gl_FragCoord is used
                uint32_t frontFacing          : 1;      // Whether gl_FrontFacing is used
                uint32_t clipDistance         : 4;      // Array size of gl_ClipDistance[] (0 means unused)
                uint32_t cullDistance         : 4;      // Array size of gl_CullDistance[] (0 means unused)
                uint32_t pointCoord           : 1;      // Whether gl_PointCoord is used
                uint32_t primitiveId          : 1;      // Whether gl_PrimitiveID is used
                uint32_t sampleId             : 1;      // Whether gl_SampleID is used
                uint32_t samplePosition       : 1;      // Whether gl_SamplePosition is used
                uint32_t sampleMaskIn         : 1;      // Whether gl_SampleMaskIn[] is used
                uint32_t layer                : 1;      // Whether gl_Layer is used
                uint32_t viewportIndex        : 1;      // Whether gl_ViewportIndex is used
                uint32_t helperInvocation     : 1;      // Whether gl_HelperInvocation is used
                uint32_t viewIndex            : 1;      // Whether gl_ViewIndex is used
                uint32_t baryCoordNoPersp     : 1;      // Whether gl_BaryCoordNoPersp is used (AMD extension)
                uint32_t baryCoordNoPerspCentroid: 1;   // Whether gl_BaryCoordNoPerspCentroid is used (AMD extension)
                uint32_t baryCoordNoPerspSample  : 1;   // Whether gl_BaryCoordNoPerspSample is used (AMD extension)
                uint32_t baryCoordSmooth      : 1;      // Whether gl_BaryCoordSmooth is used (AMD extension)
                uint32_t baryCoordSmoothCentroid : 1;   // Whether gl_BaryCoordSmoothCentroid is used (AMD extension)
                uint32_t baryCoordSmoothSample: 1;      // Whether gl_BaryCoordSmoothSample is used (AMD extension)
                uint32_t baryCoordPullModel   : 1;      // Whether gl_BaryCoordPullModel is used (AMD extension)
                // Output
                uint32_t fragDepth            : 1;      // Whether gl_FragDepth is used
                uint32_t sampleMask           : 1;      // Whether gl_SampleMask[] is used
                uint32_t fragStencilRef       : 1;      // Whether gl_FragStencilRef is used
                // Execution mode
                uint32_t originUpperLeft      : 1;      // Whether "origin_upper_left" qualifier is used
                uint32_t pixelCenterInteger   : 1;      // Whether "pixel_center_integer" qualifier is used
                uint32_t earlyFragmentTests   : 1;      // Whether "early_fragment_tests" qualifier is used
                uint32_t depthMode            : 2;      // Mode of gl_FragDepth
                uint32_t postDepthCoverage    : 1;      // Whether "post_depth_coverage" qualifier is used
                // Statements
                uint32_t discard              : 1;      // Whether "discard" statement is used
                uint32_t runAtSampleRate      : 1;      // Whether fragment shader run at sample rate

                uint64_t unused               : 26;
            } fs;

            // Compute shader
            struct
            {
                // Execution mode
                uint32_t workgroupSizeX         : 16;     // X value of gl_WorkGroupSize
                uint32_t workgroupSizeY         : 16;     // Y value of gl_WorkGroupSize
                uint32_t workgroupSizeZ         : 16;     // Z value of gl_WorkGroupSize
                // Workgroup layout
                uint32_t workgroupLayout        : 2;      // The layout of the workgroup
                // Input
                uint32_t numWorkgroups          : 1;      // Whether gl_NumWorkGroups is used
                uint32_t localInvocationId      : 1;      // Whether gl_LocalInvocationID is used
                uint32_t workgroupId            : 1;      // Whether gl_WorkGroupID is used
                uint32_t numSubgroups           : 1;      // Whether gl_NumSubgroups is used
                uint32_t subgroupId             : 1;      // Whether gl_SubgroupID is used

                uint64_t unused                 : 9;
            } cs;

            struct
            {
                uint64_t u64All;
            } perStage;
        };

        // Common built-in usage
        union
        {
            struct
            {
                uint32_t subgroupSize              : 1;  // Whether gl_SubGroupSize is used
                uint32_t subgroupLocalInvocationId : 1;  // Whether gl_SubGroupInvocation is used
                uint32_t subgroupEqMask            : 1;  // Whether gl_SubGroupEqMask is used
                uint32_t subgroupGeMask            : 1;  // Whether gl_SubGroupGeMask is used
                uint32_t subgroupGtMask            : 1;  // Whether gl_SubGroupGtMask is used
                uint32_t subgroupLeMask            : 1;  // Whether gl_SubGroupLeMask is used
                uint32_t subgroupLtMask            : 1;  // Whether gl_SubGroupLtMask is used
                uint32_t deviceIndex               : 1;  // Whether gl_DeviceIndex is used
                uint32_t denormPerserve            : 4;  // Bitmask of denormPerserve flags
                uint32_t denormFlushToZero         : 4;  // Bitmask of denormFlushToZero flags
                uint32_t signedZeroInfNanPreserve  : 4;  // Bitmask of signedZeroInfNanPreserve flags
                uint32_t roundingModeRTE           : 4;  // Bitmask of roundingModeRTE flags
                uint32_t roundingModeRTZ           : 4;  // Bitmask of roundingModeRTZ flags

                uint64_t unused                    : 36;
            } common;

            struct
            {
                uint64_t u64All;
            } allStage;
        };

    } builtInUsage;

    // Usage of generic input/output
    struct
    {
        // Map from shader specified locations to tightly packed locations
        std::map<uint32_t, uint32_t> inputLocMap;
        std::map<uint32_t, uint32_t> outputLocMap;

        std::map<uint32_t, uint32_t> perPatchInputLocMap;
        std::map<uint32_t, uint32_t> perPatchOutputLocMap;

        // Map from built-in IDs to specially assigned locations
        std::map<uint32_t, uint32_t> builtInInputLocMap;
        std::map<uint32_t, uint32_t> builtInOutputLocMap;

        std::map<uint32_t, uint32_t> perPatchBuiltInInputLocMap;
        std::map<uint32_t, uint32_t> perPatchBuiltInOutputLocMap;

        // Transform feedback strides
        uint32_t xfbStrides[MaxTransformFeedbackBuffers];

        // Transform feedback enablement
        bool enableXfb;

        // Stream to transform feedback buffers
        uint32_t streamXfbBuffers[MaxGsStreams];

        // Count of mapped location for inputs/outputs (including those special locations to which the built-ins
        // are mapped)
        uint32_t    inputMapLocCount;
        uint32_t    outputMapLocCount;
        uint32_t    perPatchInputMapLocCount;
        uint32_t    perPatchOutputMapLocCount;

        uint32_t    expCount;   // Export count (number of "exp" instructions) for generic outputs

        struct
        {
            struct
            {
                uint32_t inVertexStride;                // Stride of vertices of input patch (in DWORD, correspond to
                                                        // "lsStride")
                uint32_t outVertexStride;               // Stride of vertices of output patch (in DWORD, correspond to
                                                        // "hsCpStride")
                uint32_t patchCountPerThreadGroup;      // Count of patches per thread group (in DWORD, correspond to
                                                        // "hsNumPatch")
                // On-chip caculation factors
                struct
                {
                    uint32_t outPatchStart;                 // Offset into LDS where vertices of output patches start
                                                            // (in DWORD, correspond to "hsOutputBase")
                    uint32_t patchConstStart;               // Offset into LDS where patch constants start (in DWORD,
                } onChip;

                // Off-chip caculation factors
                struct
                {
                    uint32_t outPatchStart;                 // Offset into LDS where vertices of output patches start
                                                            // (in DWORD, correspond to "hsOutputBase")
                    uint32_t patchConstStart;               // Offset into LDS where patch constants start (in DWORD,
                } offChip;

                uint32_t inPatchSize;                   // size of an input patch size (in DWORD)

                                                        // correspond to "patchConstBase")
                uint32_t outPatchSize;                  // Size of an output patch output (in DWORD, correspond to
                                                        // "patchOutputSize")

                uint32_t patchConstSize;                // Size of an output patch constants (in DWORD)
                uint32_t tessFactorStride;              // Size of tess factor stride (in DWORD)

            } calcFactor;
        } tcs;

        struct
        {
            // Map from IDs of built-in outputs to locations of generic outputs (used by copy shader to export built-in
            // outputs to fragment shader, always from vertex stream 0)
            std::unordered_map<uint32_t, uint32_t> builtInOutLocs;

            // Map from tightly packed locations to byte sizes of generic outputs (used by copy shader to
            // export generic outputs to fragment shader, always from vertex stream 0):
            //   <location, <component, byteSize>>
            std::unordered_map<uint32_t, std::vector<uint32_t>> genericOutByteSizes[MaxGsStreams];

            // Map from output location to the transform feedback info
            std::map<uint32_t, uint32_t> xfbOutsInfo;

            // ID of the vertex stream sent to rasterizor
            uint32_t rasterStream;

            struct
            {
                uint32_t esGsRingItemSize;          // Size of each vertex written to the ES -> GS Ring.
                uint32_t gsVsRingItemSize;          // Size of each primitive written to the GS -> VS Ring.
                uint32_t esVertsPerSubgroup;        // Number of vertices ES exports.
                uint32_t gsPrimsPerSubgroup;        // Number of prims GS exports.
                uint32_t esGsLdsSize;               // ES -> GS ring LDS size (GS in)
                uint32_t gsOnChipLdsSize;           // Total LDS size for GS on-chip mode.
                uint32_t inputVertices;             // Number of GS input vertices
#if LLPC_BUILD_GFX10
                uint32_t primAmpFactor;             // GS primitive amplification factor
#endif
            } calcFactor;

            uint32_t    outLocCount[MaxGsStreams];
        } gs;

        struct
        {
            // Original shader specified locations before location map (from tightly packed locations to shader
            // specified locations)
            //
            // NOTE: This collected info is used to revise the calculated CB shader channel mask. Hardware requires
            // the targets of fragment color export (MRTs) to be tightly packed while the CB shader channel masks
            // should correspond to original shader specified targets.
            uint32_t outputOrigLocs[MaxColorTargets];

            std::vector<FsInterpInfo> interpInfo;       // Array of interpolation info
            ExportFormat expFmts[MaxColorTargets];      // Shader export formats
            BasicType    outputTypes[MaxColorTargets];  // Array of basic types of fragment outputs
            uint32_t     cbShaderMask;                  // CB shader channel mask (correspond to register CB_SHADER_MASK)
            bool         dummyExport;                   // Control to generate fragment shader dummy export
        } fs;
    } inOutUsage;
};

// Represents stream-out data
struct StreamOutData
{
    uint32_t tablePtr;                                    // Table pointer for stream-out
    uint32_t streamInfo;                                  // Stream-out info (ID, vertex count, enablement)
    uint32_t writeIndex;                                  // Write index for stream-out
    uint32_t streamOffsets[MaxTransformFeedbackBuffers];  // Stream-out Offset
};

// Represents interface data used by shader stages
//
// NOTE: All fields must be initialized in InitShaderInterfaceData().
struct InterfaceData
{
    static const uint32_t MaxDescTableCount  = 64; // Must greater than (vk::MaxDynamicDescriptors +
                                                   // vk::MaxDescriptorSets + special descriptors)
    static const uint32_t MaxUserDataCount   = 32; // Max count of allowed user data (consider GFX IP version info)
    static const uint32_t MaxSpillTableSize  = 512;
    static const uint32_t MaxDynDescCount    = 32;
    static const uint32_t MaxEsGsOffsetCount = 6;
    static const uint32_t MaxCsUserDataCount = 10;
    static const uint32_t CsStartUserData     = 2;
    static const uint32_t UserDataUnmapped = InvalidValue;

    uint32_t                    userDataCount;                    // User data count
    uint32_t                    userDataMap[MaxUserDataCount];    // User data map (from SGPR No. to API logical ID)

    struct
    {
        uint32_t                resNodeIdx;                       // Resource node index for push constant
    } pushConst;

    struct
    {
        uint32_t                sizeInDwords;                     // Spill table size in dwords
        uint32_t                offsetInDwords;                   // Start offset of Spill table
    } spillTable;

    // Usage of user data registers for internal-use variables
    struct
    {
        union
        {
            // Vertex shader
            struct
            {
                uint32_t baseVertex;                // Base vertex
                uint32_t baseInstance;              // Base instance
                uint32_t drawIndex;                 // Draw index
                uint32_t vbTablePtr;                // Pointer of vertex buffer table
                uint32_t viewIndex;                 // View Index
                uint32_t streamOutTablePtr;         // Pointer of stream-out buffer table
                uint32_t esGsLdsSize;               // ES -> GS ring LDS size for GS on-chip mode (for GFX9 and NGG)
            } vs;

            struct
            {
                uint32_t viewIndex;                 // View Index
                uint32_t streamOutTablePtr;         // Pointer of stream-out buffer table
#if LLPC_BUILD_GFX10
                uint32_t esGsLdsSize;               // ES -> GS ring LDS size for GS on-chip mode (for NGG)
#endif
            } tes;

            // Geometry shader
            struct
            {
                uint32_t esGsLdsSize;               // ES -> GS ring LDS size for GS on-chip mode (for GFX8 and NGG)
                uint32_t viewIndex;                 // View Index
                uint32_t copyShaderEsGsLdsSize;     // ES -> GS ring LDS size (for copy shader)
                uint32_t copyShaderStreamOutTable;  // Stream-out table (for copy shader)
            } gs;

            // Compute shader
            struct
            {
                uint32_t numWorkgroupsPtr;          // Pointer of NumWorkGroups
            } cs;
        };

        uint32_t spillTable;                        // Spill table user data map

    } userDataUsage;

    // Indices of the arguments in shader entry-point
    struct
    {
        union
        {
            // Vertex shader
            struct
            {
                uint32_t baseVertex;                // Base vertex
                uint32_t baseInstance;              // Base instance
                uint32_t vertexId;                  // Vertex ID
                uint32_t relVertexId;               // Relative vertex ID (index of vertex within thread group)
                uint32_t instanceId;                // Instance ID
                uint32_t drawIndex;                 // Draw index
                uint32_t primitiveId;               // Primitive ID
                uint32_t viewIndex;                 // View Index
                uint32_t vbTablePtr;                // Pointer of vertex buffer table
                uint32_t esGsOffset;                // ES-GS ring buffer offset
                StreamOutData streamOutData;        // Stream-out Data
            } vs;

            // Tessellation control shader
            struct
            {
                uint32_t patchId;               // Patch ID
                uint32_t relPatchId;            // Relative patch ID (control point ID included)
                uint32_t tfBufferBase;          // Base offset of tessellation factor(TF) buffer
                uint32_t offChipLdsBase;        // Base offset of off-chip LDS buffer
            } tcs;

            // Tessellation evaluation shader
            struct
            {
                uint32_t tessCoordX;          // X channel of gl_TessCoord (U)
                uint32_t tessCoordY;          // Y channel of gl_TessCoord (V)
                uint32_t relPatchId;          // Relative patch id
                uint32_t patchId;             // Patch ID
                uint32_t esGsOffset;          // ES-GS ring buffer offset
                uint32_t offChipLdsBase;      // Base offset of off-chip LDS buffer
                uint32_t viewIndex;           // View Index
                StreamOutData streamOutData;  // Stream-out Data
            } tes;

            // Geometry shader
            struct
            {
                uint32_t gsVsOffset;                        // GS -> VS ring offset
                uint32_t waveId;                            // GS wave ID
                uint32_t esGsOffsets[MaxEsGsOffsetCount];   // ES -> GS ring offset
                uint32_t primitiveId;                       // Primitive ID
                uint32_t invocationId;                      // Invocation ID
                uint32_t viewIndex;                         // View Index
                StreamOutData streamOutData;                // Stream-out Data
            } gs;

            // Fragment shader
            struct
            {
                uint32_t primMask;                  // Primitive mask

                // Perspective interpolation (I/J)
                struct
                {
                    uint32_t sample;                // Sample
                    uint32_t center;                // Center
                    uint32_t centroid;              // Centroid
                    uint32_t pullMode;              // Pull-mode
                } perspInterp;

                // Linear interpolation (I/J)
                struct
                {
                    uint32_t sample;                // Sample
                    uint32_t center;                // Center
                    uint32_t centroid;              // Centroid
                } linearInterp;

                // FragCoord
                struct
                {
                    uint32_t x;                     // X channel
                    uint32_t y;                     // Y channel
                    uint32_t z;                     // Z channel
                    uint32_t w;                     // W channel
                } fragCoord;

                uint32_t frontFacing;               // FrontFacing
                uint32_t ancillary;                 // Ancillary
                uint32_t sampleCoverage;            // Sample coverage
            } fs;

            // Compute shader
            struct
            {
                uint32_t numWorkgroupsPtr;          // Pointer of NumWorkGroups
                uint32_t localInvocationId;         // LocalInvocationID
                uint32_t workgroupId;               // WorkGroupID
            } cs;
        };

        uint32_t resNodeValues[MaxDescTableCount];  // Resource node values
        uint32_t spillTable;                        // Spill table
        bool     initialized;                       // Whether entryArgIdxs has been initialized
                                                    //   by PatchEntryPointMutate
    } entryArgIdxs;
};

#if LLPC_BUILD_GFX10
// Represents NGG (implicit primitive shader) control settings (valid for GFX10+)
struct NggControl : NggState
{
    bool                            passthroughMode; // Whether NGG passthrough mode is enabled
    Util::Abi::PrimShaderCbLayout   primShaderTable; // Primitive shader table (only some registers are used)
};
#endif

// =====================================================================================================================
// Represents pipeline-specific context for pipeline compilation, it is a part of LLPC context
class PipelineContext
{
public:
    PipelineContext(GfxIpVersion           gfxIp,
                    const GpuProperty*     pGpuProp,
                    const WorkaroundFlags* pGpuWorkarounds,
                    MetroHash::Hash*       pPipelineHash,
                    MetroHash::Hash*       pCacheHash);
    virtual ~PipelineContext();

    // Gets resource usage of the specified shader stage
    virtual ResourceUsage* GetShaderResourceUsage(ShaderStage shaderStage) = 0;

    // Gets interface data of the specified shader stage
    virtual InterfaceData* GetShaderInterfaceData(ShaderStage shaderStage) = 0;

    // Checks whether the pipeline is graphics or compute
    virtual bool IsGraphics() const = 0;

    // Gets pipeline shader info of the specified shader stage
    virtual const PipelineShaderInfo* GetPipelineShaderInfo(ShaderStage shaderStage) const = 0;

    // Gets pipeline build info
    virtual const void* GetPipelineBuildInfo() const = 0;

    // Gets the mask of active shader stages bound to this pipeline
    virtual uint32_t GetShaderStageMask() const = 0;

    // Gets the count of active shader stages
    virtual uint32_t GetActiveShaderStageCount() const = 0;

    // Gets the previous active shader stage in this pipeline
    virtual ShaderStage GetPrevShaderStage(ShaderStage shaderStage) const { return ShaderStageInvalid; }

    // Gets the next active shader stage in this pipeline
    virtual ShaderStage GetNextShaderStage(ShaderStage shaderStage) const { return ShaderStageInvalid; }

    // Checks whether tessellation off-chip mode is enabled
    virtual bool IsTessOffChip() const = 0;

    // Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
    virtual bool CheckGsOnChipValidity() = 0;

    // Checks whether GS on-chip mode is enabled
    virtual bool IsGsOnChip() const = 0;

    // Enables GS on-chip mode
    virtual void SetGsOnChip(bool gsOnChip) = 0;

    // Does user data node merge for merged shader
    virtual void DoUserDataNodeMerge() = 0;

#if LLPC_BUILD_GFX10
    // Sets NGG control settings
    virtual void SetNggControl() = 0;

    // Gets NGG control settings
    virtual const NggControl* GetNggControl() const = 0;

    // Gets WGP mode enablement for the specified shader stage
    virtual bool GetShaderWgpMode(ShaderStage shaderStage) const = 0;
#endif

    // Gets the count of vertices per primitive
    virtual uint32_t GetVerticesPerPrimitive() const = 0;

    // Gets wave size for the specified shader stage
    virtual uint32_t GetShaderWaveSize(ShaderStage stage) = 0;

    static const char* GetGpuNameString(GfxIpVersion gfxIp);
    static const char* GetGpuNameAbbreviation(GfxIpVersion gfxIp);

    // Gets graphics IP version info
    GfxIpVersion GetGfxIpVersion() const { return m_gfxIp; }

    const GpuProperty* GetGpuProperty() const { return m_pGpuProperty; }

    const WorkaroundFlags* GetGpuWorkarounds() const { return m_pGpuWorkarounds; }

    // Gets pipeline hash code
    uint64_t GetPiplineHashCode() const { return MetroHash::Compact64(&m_pipelineHash); }
    uint64_t GetCacheHashCode() const { return MetroHash::Compact64(&m_cacheHash); }

    virtual ShaderHash GetShaderHashCode(ShaderStage stage) const;

    // Gets per pipeline options
    virtual const PipelineOptions* GetPipelineOptions() const = 0;

    // Set pipeline state in Builder
    void SetBuilderPipelineState(Builder* pBuilder) const;

    static void InitShaderResourceUsage(ShaderStage shaderStage, ResourceUsage* pResUsage);

    static void InitShaderInterfaceData(InterfaceData* pIntfData);
protected:
    // Gets dummy vertex input create info
    virtual VkPipelineVertexInputStateCreateInfo* GetDummyVertexInputInfo() { return nullptr; }

    // Gets dummy vertex binding info
    virtual std::vector<VkVertexInputBindingDescription>* GetDummyVertexBindings() { return nullptr; }

    // Gets dummy vertex attribute info
    virtual std::vector<VkVertexInputAttributeDescription>* GetDummyVertexAttributes() { return nullptr; }

    // -----------------------------------------------------------------------------------------------------------------

    GfxIpVersion           m_gfxIp;         // Graphics IP version info
    MetroHash::Hash        m_pipelineHash;  // Pipeline hash code
    MetroHash::Hash        m_cacheHash;     // Cache hash code
    const GpuProperty*     m_pGpuProperty;  // GPU Property
    const WorkaroundFlags* m_pGpuWorkarounds;  // GPU workarounds

private:
    LLPC_DISALLOW_DEFAULT_CTOR(PipelineContext);
    LLPC_DISALLOW_COPY_AND_ASSIGN(PipelineContext);
};

} // Llpc
