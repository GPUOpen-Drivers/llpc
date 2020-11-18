/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ResourceUsage.h
 * @brief LLPC header file: contains declaration of ResourceUsage and InterfaceData structs
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/state/Defs.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/util/Internal.h"
#include <unordered_map>
#include <unordered_set>

namespace lgc {

static const unsigned MaxColorTargets = 8;

/// Represents the base data type
enum class BasicType : unsigned {
  Unknown = 0, // Unknown
  Float,       // Float
  Double,      // Double
  Int,         // Signed integer
  Uint,        // Unsigned integer
  Int64,       // 64-bit signed integer
  Uint64,      // 64-bit unsigned integer
  Float16,     // 16-bit floating-point
  Int16,       // 16-bit signed integer
  Uint16,      // 16-bit unsigned integer
  Int8,        // 8-bit signed integer
  Uint8,       // 8-bit unsigned integer
};

// Represents descriptor set/binding pair.
union DescriptorPair {
  struct {
    unsigned descSet; // ID of descriptor set
    unsigned binding; // ID of descriptor binding
  };
  uint64_t u64All;
};

// Represents transform feedback output info
union XfbOutInfo {
  struct {
    unsigned xfbBuffer : 2;       // Transform feedback buffer
    unsigned xfbOffset : 16;      // Transform feedback offset
    unsigned xfbExtraOffset : 13; // Transform feedback extra offset
    unsigned is16bit : 1;         // Whether it is 16-bit data for transform feedback
  };
  unsigned u32All;
};

// Represents interpolation info of fragment shader input
struct FsInterpInfo {
  unsigned loc; // Mapped input location (tightly packed)
  bool flat;    // Whether it is "flat" interpolation
  bool custom;  // Whether it is "custom" interpolation
  bool is16bit; // Whether it is 16-bit interpolation
  bool attr0Valid; // Whether the location has a valid low half
  bool attr1Valid; // Wheterh the location has a valid high half
};

// Invalid interpolation info
static const FsInterpInfo InvalidFsInterpInfo = {InvalidValue, false, false, false, false, false};

// Represents the location info of input/output
union InOutLocationInfo {
  struct {
    uint16_t half : 1;      // High half in case of 16-bit attriburtes
    uint16_t component : 2; // The component index
    uint16_t location : 10; // The location
    uint16_t isBuiltIn : 1; // Whether location is actually built-in ID
    uint16_t streamId : 2;  // Output vertex stream ID
  };
  uint16_t u16All;
};

// Enumerate the workgroup layout options.
enum class WorkgroupLayout : unsigned {
  Unknown = 0,   // ?x?
  Linear,        // 4x1
  Quads,         // 2x2
  SexagintiQuads // 8x8
};

// Represents the usage info of shader resources.
//
// NOTE: All fields must be initialized in InitShaderResourceUsage().
struct ResourceUsage {
  std::unordered_set<uint64_t> descPairs;  // Pairs of descriptor set/binding
  bool resourceWrite = false;              // Whether shader does resource-write operations (UAV)
  bool resourceRead = false;               // Whether shader does resource-read operrations (UAV)
  bool perShaderTable = false;             // Whether per shader stage table is used
  unsigned numSgprsAvailable = UINT32_MAX; // Number of available SGPRs
  unsigned numVgprsAvailable = UINT32_MAX; // Number of available VGPRs
  bool useImages = false;                  // Whether images are used

  // Usage of built-ins
  struct {
    // Per-stage built-in usage
    union {
      // Vertex shader
      struct {
        // Input
        unsigned vertexIndex : 1;   // Whether gl_VertexIndex is used
        unsigned instanceIndex : 1; // Whether gl_InstanceIndex is used
        unsigned baseVertex : 1;    // Whether gl_BaseVertex is used
        unsigned baseInstance : 1;  // Whether gl_BaseInstance is used
        unsigned primitiveId : 1;   // Whether an implicit gl_PrimitiveID is required
        unsigned viewIndex : 1;     // Whether gl_ViewIndex is used
        // Output
        unsigned pointSize : 1;     // Whether gl_PointSize is used
        unsigned position : 1;      // Whether gl_Position is used
        unsigned clipDistance : 4;  // Array size of gl_ClipDistance[] (0 means unused)
        unsigned cullDistance : 4;  // Array size of gl_CullDistance[] (0 means unused)
        unsigned viewportIndex : 1; // Whether gl_ViewportIndex is used
        unsigned layer : 1;         // Whether gl_Layer is used
        unsigned : 1; // Reserved
        unsigned primitiveShadingRate : 1; // Whether gl_PrimitiveShadingRate is used
      } vs;

      // Tessellation control shader
      struct {
        // Input
        unsigned pointSizeIn : 1;    // Whether gl_in[].gl_PointSize is used
        unsigned positionIn : 1;     // Whether gl_in[].gl_Position is used
        unsigned clipDistanceIn : 4; // Array size of gl_in[].gl_ClipDistance[] (0 means unused)
        unsigned cullDistanceIn : 4; // Array size of gl_in[].gl_CullDistance[] (0 means unused)
        unsigned patchVertices : 1;  // Whether gl_PatchVerticesIn is used
        unsigned primitiveId : 1;    // Whether gl_PrimitiveID is used
        unsigned invocationId : 1;   // Whether gl_InvocationID is used
        // Output
        unsigned pointSize : 1;      // Whether gl_out[].gl_PointSize is used
        unsigned position : 1;       // Whether gl_out[].gl_Position is used
        unsigned clipDistance : 4;   // Array size of gl_out[].gl_ClipDistance[] (0 means unused)
        unsigned cullDistance : 4;   // Array size of gl_out[].gl_CullDistance[] (0 means unused)
        unsigned tessLevelOuter : 1; // Whether gl_TessLevelOuter[] is used
        unsigned tessLevelInner : 1; // Whether gl_TessLevelInner[] is used
      } tcs;

      // Tessellation evaluation shader
      struct {
        // Input
        unsigned pointSizeIn : 1;    // Whether gl_in[].gl_PointSize is used
        unsigned positionIn : 1;     // Whether gl_in[].gl_Position is used
        unsigned clipDistanceIn : 4; // Array size of gl_in[].gl_ClipDistance[] (0 means unused)
        unsigned cullDistanceIn : 4; // Array size of gl_in[].gl_CullDistance[] (0 means unused)
        unsigned patchVertices : 1;  // Whether gl_PatchVerticesIn is used
        unsigned primitiveId : 1;    // Whether gl_PrimitiveID is used
        unsigned tessCoord : 1;      // Whether gl_TessCoord is used
        unsigned tessLevelOuter : 1; // Whether gl_TessLevelOuter[] is used
        unsigned tessLevelInner : 1; // Whether gl_TessLevelInner[] is used
        unsigned viewIndex : 1;      // Whether gl_ViewIndex is used
        // Output
        unsigned pointSize : 1;     // Whether gl_PointSize is used
        unsigned position : 1;      // Whether gl_Position is used
        unsigned clipDistance : 4;  // Array size gl_ClipDistance[] (0 means unused)
        unsigned cullDistance : 4;  // Array size gl_CullDistance[] (0 means unused)
        unsigned viewportIndex : 1; // Whether gl_ViewportIndex is used
        unsigned layer : 1;         // Whether gl_Layer is used
        unsigned : 1; // Reserved
      } tes;

      // Geometry shader
      struct {
        // Input
        unsigned pointSizeIn : 1;    // Whether gl_in[].gl_PointSize is used
        unsigned positionIn : 1;     // Whether gl_in[].gl_Position is used
        unsigned clipDistanceIn : 4; // Array size of gl_in[].gl_ClipDistance[] (0 means unused)
        unsigned cullDistanceIn : 4; // Array size of gl_in[].gl_CullDistance[] (0 means unused)
        unsigned primitiveIdIn : 1;  // Whether gl_PrimitiveIDIn is used
        unsigned invocationId : 1;   // Whether gl_InvocationID is used
        unsigned viewIndex : 1;      // Whether gl_ViewIndex is used
        // Output
        unsigned pointSize : 1;     // Whether gl_PointSize is used
        unsigned position : 1;      // Whether gl_Position is used
        unsigned clipDistance : 4;  // Array size gl_ClipDistance[] (0 means unused)
        unsigned cullDistance : 4;  // Array size gl_CullDistance[] (0 means unused)
        unsigned primitiveId : 1;   // Whether gl_PrimitiveID is used
        unsigned viewportIndex : 1; // Whether gl_ViewportIndex is used
        unsigned layer : 1;         // Whether gl_Layer is used
        unsigned : 1; // Reserved
        unsigned primitiveShadingRate : 1; // Whether gl_PrimitiveShadingRate is used
      } gs;

      // Fragment shader
      struct {
        // Interpolation
        unsigned smooth : 1;        // Whether "smooth" qualifier is used
        unsigned noperspective : 1; // Whether "noperspective" qualifier is used
        unsigned flat : 1;          // Whether "flat" qualifier is used
        unsigned centroid : 1;      // Whether "centroid" qualifier is used
        unsigned sample : 1;        // Whether "sample" qualifier is used
        unsigned center : 1;        // Whether location qualifiers are not used (default: "center")
        unsigned pullMode : 1;      // Whether pull mode interpolation is used
        unsigned custom : 1;        // Whether custom interpolation is used
        // Input
        unsigned fragCoord : 1;        // Whether gl_FragCoord is used
        unsigned frontFacing : 1;      // Whether gl_FrontFacing is used
        unsigned clipDistance : 4;     // Array size of gl_ClipDistance[] (0 means unused)
        unsigned cullDistance : 4;     // Array size of gl_CullDistance[] (0 means unused)
        unsigned pointCoord : 1;       // Whether gl_PointCoord is used
        unsigned primitiveId : 1;      // Whether gl_PrimitiveID is used
        unsigned sampleId : 1;         // Whether gl_SampleID is used
        unsigned samplePosition : 1;   // Whether gl_SamplePosition is used
        unsigned sampleMaskIn : 1;     // Whether gl_SampleMaskIn[] is used
        unsigned layer : 1;            // Whether gl_Layer is used
        unsigned viewportIndex : 1;    // Whether gl_ViewportIndex is used
        unsigned helperInvocation : 1; // Whether gl_HelperInvocation is used
        unsigned viewIndex : 1;        // Whether gl_ViewIndex is used
        unsigned shadingRate : 1; // Whether gl_ShadingRate is used
        unsigned baryCoordNoPersp : 1;         // Whether gl_BaryCoordNoPersp is used (AMD extension)
        unsigned baryCoordNoPerspCentroid : 1; // Whether gl_BaryCoordNoPerspCentroid is used (AMD extension)
        unsigned baryCoordNoPerspSample : 1;   // Whether gl_BaryCoordNoPerspSample is used (AMD extension)
        unsigned baryCoordSmooth : 1;          // Whether gl_BaryCoordSmooth is used (AMD extension)
        unsigned baryCoordSmoothCentroid : 1;  // Whether gl_BaryCoordSmoothCentroid is used (AMD extension)
        unsigned baryCoordSmoothSample : 1;    // Whether gl_BaryCoordSmoothSample is used (AMD extension)
        unsigned baryCoordPullModel : 1;       // Whether gl_BaryCoordPullModel is used (AMD extension)
        // Output
        unsigned fragDepth : 1;      // Whether gl_FragDepth is used
        unsigned sampleMask : 1;     // Whether gl_SampleMask[] is used
        unsigned fragStencilRef : 1; // Whether gl_FragStencilRef is used
        // Statements
        unsigned discard : 1;         // Whether "discard" statement is used
        unsigned runAtSampleRate : 1; // Whether fragment shader run at sample rate
      } fs;

      // Compute shader
      struct {
        // Workgroup layout
        unsigned workgroupLayout : 2; // The layout of the workgroup
        // Input
        unsigned numWorkgroups : 1;     // Whether gl_NumWorkGroups is used
        unsigned localInvocationId : 1; // Whether gl_LocalInvocationID is used
        unsigned workgroupId : 1;       // Whether gl_WorkGroupID is used
        unsigned numSubgroups : 1;      // Whether gl_NumSubgroups is used
        unsigned subgroupId : 1;        // Whether gl_SubgroupID is used
      } cs;
    };

    // Common built-in usage
    struct {
      unsigned subgroupSize : 1;              // Whether gl_SubGroupSize is used
      unsigned subgroupLocalInvocationId : 1; // Whether gl_SubGroupInvocation is used
      unsigned subgroupEqMask : 1;            // Whether gl_SubGroupEqMask is used
      unsigned subgroupGeMask : 1;            // Whether gl_SubGroupGeMask is used
      unsigned subgroupGtMask : 1;            // Whether gl_SubGroupGtMask is used
      unsigned subgroupLeMask : 1;            // Whether gl_SubGroupLeMask is used
      unsigned subgroupLtMask : 1;            // Whether gl_SubGroupLtMask is used
      unsigned deviceIndex : 1;               // Whether gl_DeviceIndex is used
    } common;

  } builtInUsage;

  // Usage of generic input/output
  struct {
    // Map from shader specified locations to tightly packed locations
    std::map<unsigned, unsigned> inputLocMap;
    std::map<unsigned, unsigned> outputLocMap;

    // The original and new InOutLocations for shader cache
    std::map<unsigned, unsigned> inOutLocMap;

    std::map<unsigned, unsigned> perPatchInputLocMap;
    std::map<unsigned, unsigned> perPatchOutputLocMap;

    // Map from built-in IDs to specially assigned locations
    std::map<unsigned, unsigned> builtInInputLocMap;
    std::map<unsigned, unsigned> builtInOutputLocMap;

    std::map<unsigned, unsigned> perPatchBuiltInInputLocMap;
    std::map<unsigned, unsigned> perPatchBuiltInOutputLocMap;

    // Transform feedback strides
    unsigned xfbStrides[MaxTransformFeedbackBuffers] = {};

    // Transform feedback enablement
    bool enableXfb = false;

    // Stream to transform feedback buffers
    unsigned streamXfbBuffers[MaxGsStreams] = {};

    // Count of mapped location for inputs/outputs (including those special locations to which the built-ins
    // are mapped)
    unsigned inputMapLocCount = 0;
    unsigned outputMapLocCount = 0;
    unsigned perPatchInputMapLocCount = 0;
    unsigned perPatchOutputMapLocCount = 0;

    unsigned expCount = 0; // Export count (number of "exp" instructions) for generic outputs

    struct {
      struct {
        unsigned inVertexStride;           // Stride of vertices of input patch (in dword, correspond to
                                           // "lsStride")
        unsigned outVertexStride;          // Stride of vertices of output patch (in dword, correspond to
                                           // "hsCpStride")
        unsigned patchCountPerThreadGroup; // Count of patches per thread group (in dword, correspond to
                                           // "hsNumPatch")
        // On-chip caculation factors
        struct {
          unsigned outPatchStart;   // Offset into LDS where vertices of output patches start
                                    // (in dword, correspond to "hsOutputBase")
          unsigned patchConstStart; // Offset into LDS where patch constants start (in dword,
                                    // correspond to "patchConstBase")
        } onChip;

        // Off-chip caculation factors
        struct {
          unsigned outPatchStart;   // Offset into LDS where vertices of output patches start
                                    // (in dword, correspond to "hsOutputBase")
          unsigned patchConstStart; // Offset into LDS where patch constants start (in dword,
                                    // correspond to "patchConstBase")
        } offChip;

        unsigned inPatchSize; // size of an input patch size (in dword)

        unsigned outPatchSize; // Size of an output patch output (in dword, correspond to
                               // "patchOutputSize")

        unsigned patchConstSize;   // Size of an output patch constants (in dword)
        unsigned tessFactorStride; // Size of tess factor stride (in dword)

      } calcFactor;
    } tcs = {};

    struct {
      // Map from IDs of built-in outputs to locations of generic outputs (used by copy shader to export built-in
      // outputs to fragment shader, always from vertex stream 0)
      std::unordered_map<unsigned, unsigned> builtInOutLocs;

      // Map from tightly packed locations to byte sizes of generic outputs (used by copy shader to
      // export generic outputs to fragment shader, always from vertex stream 0):
      //   <location, <component, byteSize>>
      std::unordered_map<unsigned, std::vector<unsigned>> genericOutByteSizes[MaxGsStreams];

      // Map from output location to the transform feedback info
      std::map<unsigned, unsigned> xfbOutsInfo;

      // ID of the vertex stream sent to rasterizor
      unsigned rasterStream = 0;

      struct {
        unsigned esGsRingItemSize;   // Size of each vertex written to the ES -> GS Ring, in dwords.
        unsigned gsVsRingItemSize;   // Size of each primitive written to the GS -> VS Ring, in dwords.
        unsigned esVertsPerSubgroup; // Number of vertices ES exports.
        unsigned gsPrimsPerSubgroup; // Number of prims GS exports.
        unsigned esGsLdsSize;        // ES -> GS ring LDS size (GS in)
        unsigned gsOnChipLdsSize;    // Total LDS size for GS on-chip mode.
        unsigned inputVertices;      // Number of GS input vertices
        unsigned primAmpFactor;      // GS primitive amplification factor
        bool enableMaxVertOut;       // Whether to allow each GS instance to emit maximum vertices (NGG)
      } calcFactor = {};

      unsigned outLocCount[MaxGsStreams] = {};
    } gs;

    struct {
      // Original shader specified locations before location map (from tightly packed locations to shader
      // specified locations)
      //
      // NOTE: This collected info is used to revise the calculated CB shader channel mask. Hardware requires
      // the targets of fragment color export (MRTs) to be tightly packed while the CB shader channel masks
      // should correspond to original shader specified targets.
      unsigned outputOrigLocs[MaxColorTargets];

      std::vector<FsInterpInfo> interpInfo;   // Array of interpolation info
      BasicType outputTypes[MaxColorTargets]; // Array of basic types of fragment outputs
      unsigned cbShaderMask;                  // CB shader channel mask (correspond to register CB_SHADER_MASK)
      bool isNullFs;                          // Is null FS, so should set final cbShaderMask to 0
    } fs;
  } inOutUsage;

  ResourceUsage(ShaderStage shaderStage);
};

// Represents stream-out data
struct StreamOutData {
  unsigned tablePtr;                                   // Table pointer for stream-out
  unsigned streamInfo;                                 // Stream-out info (ID, vertex count, enablement)
  unsigned writeIndex;                                 // Write index for stream-out
  unsigned streamOffsets[MaxTransformFeedbackBuffers]; // Stream-out Offset
};

// Represents interface data used by shader stages
//
// NOTE: All fields must be initialized in InitShaderInterfaceData().
struct InterfaceData {
  static const unsigned MaxDescTableCount = 64; // Must greater than (vk::MaxDynamicDescriptors +
                                                // vk::MaxDescriptorSets + special descriptors)
  static const unsigned MaxUserDataCount = 32;  // Max count of allowed user data (consider GFX IP version info)
  static const unsigned MaxSpillTableSize = 512;
  static const unsigned MaxDynDescCount = 32;
  static const unsigned MaxEsGsOffsetCount = 6;
  static const unsigned MaxCsUserDataCount = 10;
  static const unsigned CsStartUserData = 2;

  unsigned userDataCount = 0;                  // User data count

  struct {
    unsigned sizeInDwords = 0;              // Spill table size in dwords
  } spillTable;

  // Usage of user data registers for internal-use variables
  struct {
    // Geometry shader
    struct {
      unsigned copyShaderEsGsLdsSize;    // ES -> GS ring LDS size (for copy shader)
      unsigned copyShaderStreamOutTable; // Stream-out table (for copy shader)
    } gs;

    unsigned spillTable; // Spill table user data map

  } userDataUsage = {};

  // Indices of the arguments in shader entry-point
  struct {
    union {
      // Vertex shader
      struct {
        unsigned baseVertex;         // Base vertex
        unsigned baseInstance;       // Base instance
        unsigned vertexId;           // Vertex ID
        unsigned relVertexId;        // Relative vertex ID (index of vertex within thread group)
        unsigned instanceId;         // Instance ID
        unsigned primitiveId;        // Primitive ID
        unsigned viewIndex;          // View Index
        unsigned vbTablePtr;         // Pointer of vertex buffer table
        unsigned esGsOffset;         // ES-GS ring buffer offset
        StreamOutData streamOutData; // Stream-out Data
      } vs;

      // Tessellation control shader
      struct {
        unsigned patchId;        // Patch ID
        unsigned relPatchId;     // Relative patch ID (control point ID included)
        unsigned tfBufferBase;   // Base offset of tessellation factor(TF) buffer
        unsigned offChipLdsBase; // Base offset of off-chip LDS buffer
      } tcs;

      // Tessellation evaluation shader
      struct {
        unsigned tessCoordX;         // X channel of gl_TessCoord (U)
        unsigned tessCoordY;         // Y channel of gl_TessCoord (V)
        unsigned relPatchId;         // Relative patch id
        unsigned patchId;            // Patch ID
        unsigned esGsOffset;         // ES-GS ring buffer offset
        unsigned offChipLdsBase;     // Base offset of off-chip LDS buffer
        unsigned viewIndex;          // View Index
        StreamOutData streamOutData; // Stream-out Data
      } tes;

      // Geometry shader
      struct {
        unsigned gsVsOffset;                      // GS -> VS ring offset
        unsigned waveId;                          // GS wave ID
        unsigned esGsOffsets[MaxEsGsOffsetCount]; // ES -> GS ring offset
        unsigned primitiveId;                     // Primitive ID
        unsigned invocationId;                    // Invocation ID
        unsigned viewIndex;                       // View Index
        StreamOutData streamOutData;              // Stream-out Data
      } gs;

      // Fragment shader
      struct {
        unsigned primMask; // Primitive mask

        // Perspective interpolation (I/J)
        struct {
          unsigned sample;   // Sample
          unsigned center;   // Center
          unsigned centroid; // Centroid
          unsigned pullMode; // Pull-mode
        } perspInterp;

        // Linear interpolation (I/J)
        struct {
          unsigned sample;   // Sample
          unsigned center;   // Center
          unsigned centroid; // Centroid
        } linearInterp;

        // FragCoord
        struct {
          unsigned x; // X channel
          unsigned y; // Y channel
          unsigned z; // Z channel
          unsigned w; // W channel
        } fragCoord;

        unsigned frontFacing;    // FrontFacing
        unsigned ancillary;      // Ancillary
        unsigned sampleCoverage; // Sample coverage
      } fs;

      // Compute shader
      struct {
        unsigned numWorkgroupsPtr;  // Pointer of NumWorkGroups
        unsigned localInvocationId; // LocalInvocationID
        unsigned workgroupId;       // WorkGroupID
      } cs;
    };

    bool initialized;                          // Whether entryArgIdxs has been initialized
                                               //   by PatchEntryPointMutate
  } entryArgIdxs = {};

  InterfaceData();
};

} // namespace lgc
