/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Pipeline.h
 * @brief LLPC header file: contains declaration of class lgc::Pipeline
 ***********************************************************************************************************************
 */
#pragma once

#include "CommonDefs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

class Timer;

} // namespace llvm

namespace lgc {

class ElfLinker;
class LgcContext;

// =====================================================================================================================
// Per-pipeline and per-shader options for setting in pipeline state

// Bit values of NGG flags. This is done as bit values rather than bitfields so the flags word appears
// in a platform-independent way in IR metadata.
enum NggFlag : unsigned {
  NggFlagDisable = 0x0001,                   // Disable NGG
  NggFlagEnableGsUse = 0x0002,               // Enable NGG when pipeline has GS
  NggFlagForceCullingMode = 0x0004,          // Force NGG to run in culling mode
  NggFlagCompactVertex = 0x0008,             // Enable Vertex compaction
  NggFlagEnableBackfaceCulling = 0x0010,     // Enable culling of primitives that don't meet facing criteria
  NggFlagEnableFrustumCulling = 0x0020,      // Enable discarding of primitives outside of view frustum
  NggFlagEnableBoxFilterCulling = 0x0040,    // Enable simpler frustum culler that is less accurate
  NggFlagEnableSphereCulling = 0x0080,       // Enable frustum culling based on a sphere
  NggFlagEnableSmallPrimFilter = 0x0100,     // Enable trivial sub-sample primitive culling
  NggFlagEnableCullDistanceCulling = 0x0200, // Enable culling when "cull distance" exports are present
};

// Enumerates various sizing options of subgroup size for NGG primitive shader.
enum class NggSubgroupSizing : unsigned {
  Auto,             ///< Subgroup size is allocated as optimally determined
  MaximumSize,      ///< Subgroup size is allocated to the maximum allowable size by the hardware
  HalfSize,         ///< Subgroup size is allocated as to allow half of the maximum allowable size
                    ///  by the hardware
  OptimizeForVerts, ///< Subgroup size is optimized for vertex thread utilization
  OptimizeForPrims, ///< Subgroup size is optimized for primitive thread utilization
  Explicit,         ///< Subgroup size is allocated based on explicitly-specified vertsPerSubgroup and
                    ///  primsPerSubgroup
};

/// Enumerate denormal override modes.
enum class DenormalMode : unsigned {
  Auto = 0x0,        ///< No denormal override (default behaviour)
  FlushToZero = 0x1, ///< Denormals flushed to zero
  Preserve = 0x2,    ///< Denormals preserved
};

// If next available quad falls outside tile aligned region of size defined by this enumeration, the compiler
// will force end of vector in the compiler to shader wavefront.
// All of these values correspond to settings of WAVE_BREAK_REGION_SIZE in PA_SC_SHADER_CONTROL.
enum class WaveBreak : unsigned {
  None = 0x0,   ///< No wave break by region
  _8x8 = 0x1,   ///< Outside a 8x8 pixel region
  _16x16 = 0x2, ///< Outside a 16x16 pixel region
  _32x32 = 0x3, ///< Outside a 32x32 pixel region
};

// Enumerate the thread group swizzle modes.
enum class ThreadGroupSwizzleMode : unsigned {
  Default = 0, // Use the default layout. There is no swizzling conducted.
  _4x4 = 1,    // The tile size is 4x4 in x and y dimension.
  _8x8 = 2,    // The tile size is 8x8 in x and y dimension.
  _16x16 = 3,  // The tile size is 16x16 in x and y dimension.
  Count,
};

// Value for shadowDescriptorTable pipeline option.
static const unsigned ShadowDescriptorTableDisable = ~0U;

static const char XfbStateMetadataName[] = "lgc.xfb.state";
static const char SampleShadingMetaName[] = "lgc.sample.shading";

// Middle-end per-pipeline options to pass to SetOptions.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// Note: new fields must be added to the end of this structure to maintain test compatibility.
struct Options {
  uint64_t hash[2];                    // Pipeline hash to set in ELF PAL metadata
  unsigned includeDisassembly;         // If set, the disassembly for all compiled shaders will be included
                                       //   in the pipeline ELF.
  unsigned reconfigWorkgroupLayout;    // If set, allows automatic workgroup reconfigure to take place on
                                       //   compute shaders.
  bool forceCsThreadIdSwizzling;       // Force rearranges threadId within group into blocks of 8*8 or 8*4.
  unsigned overrideThreadGroupSizeX;   // Override value for thread group size.X
  unsigned overrideThreadGroupSizeY;   // Override value for thread group size.Y
  unsigned overrideThreadGroupSizeZ;   // Override value for thread group size.Z
  unsigned includeIr;                  // If set, the IR for all compiled shaders will be included in the
                                       //   pipeline ELF.
  unsigned nggFlags;                   // Flags to control NGG (NggFlag* values ored together)
  unsigned nggBackfaceExponent;        // Value from 1 to UINT32_MAX that will cause the backface culling
                                       // algorithm to ignore area calculations that are less than
                                       // (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
                                       //  Only valid if the NGG backface culler is enabled.
                                       //  A value of 0 will disable the threshold.
  NggSubgroupSizing nggSubgroupSizing; // NGG subgroup sizing type
  bool fullSubgroups;                  // Use full subgroup lanes
  unsigned nggVertsPerSubgroup;        // How to determine NGG verts per subgroup
  unsigned nggPrimsPerSubgroup;        // How to determine NGG prims per subgroup
  unsigned highAddrOfFmask;            // High dword of Fmask address
  bool enableFmask;                    // Whether to use Fmasks when loading from MSAA images
  unsigned allowNullDescriptor;        // Allow and give defined behavior for null descriptor
  unsigned disableImageResourceCheck;  // Don't do image resource type check
  unsigned reserved0f;                 // Reserved for future functionality
  unsigned useResourceBindingRange;    // A resource node binding is the start of a range whose size is
                                       //  sizeInDwords/stride.
  unsigned optimizeTessFactor;    // If set, we can determine either send HT_TessFactor message or write to TF buffer
                                  // depending the values of tessellation factors.
  unsigned enableInterpModePatch; // Enable to do per-sample interpolation for nonperspective and smooth input
  unsigned pageMigrationEnabled;  // Enable page migration
  ResourceLayoutScheme resourceLayoutScheme;     // Resource layout scheme
  ThreadGroupSwizzleMode threadGroupSwizzleMode; // Thread group swizzle mode
  unsigned reverseThreadGroupBufferDescSet;      // Descriptor set ID of the internal buffer for reverse thread group
                                                 // optimization
  unsigned reverseThreadGroupBufferBinding; // Binding ID of the internal buffer for reverse thread group optimization
#if VKI_RAY_TRACING
  bool internalRtShaders; // Enable internal RT shader intrinsics
#else
  bool reserved15;
#endif
  bool enableUberFetchShader; // Enable UberShader
  bool reserved16;
};

/// Represent a pipeline option which can be automatic as well as explicitly set.
enum InvariantLoadsOption : unsigned { Auto = 0, EnableOptimization = 1, DisableOptimization = 2, ClearInvariants = 3 };

// Middle-end per-shader options to pass to SetShaderOptions.
// Note: new fields must be added to the end of this structure to maintain test compatibility.
struct ShaderOptions {
  uint64_t hash[2];     // Shader hash to set in ELF PAL metadata
  unsigned trapPresent; // Indicates a trap handler will be present when this pipeline is executed,
                        //  and any trap conditions encountered in this shader should call the trap
                        //  handler. This could include an arithmetic exception, an explicit trap
                        //  request from the host, or a trap after every instruction when in debug
                        //  mode.
  unsigned debugMode;   // When set, this shader should cause the trap handler to be executed after
                        //  every instruction.  Only valid if trapPresent is set.
  unsigned allowReZ;    // Allow the DB ReZ feature to be enabled.  This will cause an early-Z test
                        //  to potentially kill PS waves before launch, and also issues a late-Z test
                        //  in case the PS kills pixels.  Only valid for pixel shaders.

  // Maximum VGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
  // of this value and whatever the target GPU supports. To effectively disable this limit, set this to 0.
  unsigned vgprLimit;

  // Maximum SGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
  // of this value and whatever the target GPU supports. To effectively disable this limit, set this to 0.
  unsigned sgprLimit;

  /// Overrides the number of CS thread-groups which the GPU will launch per compute-unit. This throttles the
  /// shader, which can sometimes enable more graphics shader work to complete in parallel. A value of zero
  /// disables limiting the number of thread-groups to launch. This field is ignored for graphics shaders.
  unsigned maxThreadGroupsPerComputeUnit;

  unsigned waveSize;       // Control the number of threads per wavefront (GFX10+)
  unsigned subgroupSize;   // Override for the wave size when the shader uses gl_SubgroupSize, 0 for no override
  unsigned wgpMode;        // Whether to choose WGP mode or CU mode (GFX10+)
  WaveBreak waveBreakSize; // Size of region to force the end of a wavefront (GFX10+).
                           // Only valid for fragment shaders.

  // Vector size threshold for load scalarizer. 0 means do not scalarize loads at all.
  unsigned loadScalarizerThreshold;

  // Use the LLVM backend's SI scheduler instead of the default scheduler.
  bool useSiScheduler;

  // Disable various LLVM IR code sinking passes.
  bool disableCodeSinking;

  // Schedule for latency even if it reduces occupancy.
  bool favorLatencyHiding;

  // Whether update descriptor root offset in ELF
  bool updateDescInElf;

  // Default unroll threshold for LLVM.
  unsigned unrollThreshold;

  /// Override FP32 denormal handling.
  DenormalMode fp32DenormalMode;

  /// Whether enable adjustment of the fragment shader depth import for the variable shading rate
  bool adjustDepthImportVrs;

  // Unroll loops by specified amount. 0 is default, 1 is no unroll.
  unsigned forceLoopUnrollCount;

  // Disable loop unrolling.
  bool disableLoopUnroll;

  // Threshold for minimum number of blocks in a loop to disable the LICM pass.
  unsigned disableLicmThreshold;

  // Threshold to use for loops with Unroll hint. 0 to use llvm.loop.unroll.full metadata.
  unsigned unrollHintThreshold;

  // Threshold to use for loops with DontUnroll hint. 0 to use llvm.loop.unroll.disable metadata.
  unsigned dontUnrollHintThreshold;

  // Maximum amount of LDS space to be used for spilling.
  unsigned ldsSpillLimitDwords;

  // Attempt to scalarize waterfall descriptor loads.
  bool scalarizeWaterfallLoads;

  /// Override value for ThreadGroupSizeX
  unsigned overrideShaderThreadGroupSizeX;

  /// Override value for ThreadGroupSizeY
  unsigned overrideShaderThreadGroupSizeY;

  /// Override value for ThreadGroupSizeZ
  unsigned overrideShaderThreadGroupSizeZ;

  // When there is a valid "feedback loop" in renderpass, lateZ needs to be enabled
  // In Vulkan a "feedback loop" is described as a subpass where there is at least
  // one input attachment that is also a color or depth/stencil attachment
  // Feedback loops are allowed and their behavior is well defined under certain conditions.
  // When there is a feedback loop it is possible for the shaders to read
  // the contents of the color and depth/stencil attachments
  // from the shader during draw. Because of that possibility you have to use late-z
  bool forceLateZ;

  /// Minimum number of addresses to use NSA encoding on GFX10+ (0 = backend decides).
  unsigned nsaThreshold;

  /// Aggressively mark shader loads as invariant (where it is safe to do so).
  InvariantLoadsOption aggressiveInvariantLoads;

  ShaderOptions() {
    // The memory representation of this struct gets written into LLVM metadata. To prevent uninitialized values from
    // being written, we force everything to 0, including alignment gaps.
    memset(this, 0, sizeof(ShaderOptions));
  }

  ShaderOptions(const ShaderOptions &opts) { *this = opts; }

  ShaderOptions &operator=(const ShaderOptions &opts) {
    // Copy everything, including data in alignment because this is used to implement the copy constructor
    memcpy(this, &opts, sizeof(ShaderOptions));
    return *this;
  }
};

// =====================================================================================================================
// Definitions for user data resource nodes

// ResourceNodeType declaration now in CommonDefs.h.

// The representation of a user data resource node
struct ResourceNode {
  ResourceNode() {}

  ResourceNodeType concreteType; // Underlying actual type of this node
  ResourceNodeType abstractType; // Node type for resource node matching
  unsigned sizeInDwords;         // Size in dwords
  unsigned offsetInDwords;       // Offset in dwords

  union {
    // Info for generic descriptor nodes.
    struct {
      unsigned set;                   // Descriptor set
      unsigned binding;               // Binding
                                      // If pipeline option "useResourceBindingRange" is set, then this is the
                                      //  start of a range of bindings whose size is sizeInDwords/stride.
      unsigned stride;                // Size of each descriptor in the indexable range in dwords.
      unsigned immutableSize;         // Size (in units of DescriptorSizeSampler bytes) of immutableValue array
      const uint32_t *immutableValue; // Array of dwords for immutable sampler.
    };

    // Info for DescriptorTableVaPtr
    llvm::ArrayRef<ResourceNode> innerTable;

    // Info for indirect data nodes (IndirectUserDataVaPtr, StreamOutVaTablePtr)
    unsigned indirectSizeInDwords;
  };
};

// =====================================================================================================================
// Structs for setting pipeline state.
// The front-end should zero-initialize a struct with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.

// Primitive type.
enum class PrimitiveType : unsigned {
  Point = 0,
  LineList = 1,
  LineStrip = 2,
  TriangleList = 3,
  TriangleStrip = 4,
  TriangleFan = 5,
  TriangleListAdjacency = 6,
  TriangleStripAdjacency = 7,
  Rect = 8,
  Quad = 9,
  Patch = 10,
};

// Data format of vertex buffer entry. For ones that exist in GFX9 hardware, these match the hardware
// encoding. But this also includes extra formats.
enum BufDataFormat {
  BufDataFormatInvalid = 0,
  BufDataFormat8 = 1,
  BufDataFormat16 = 2,
  BufDataFormat8_8 = 3,
  BufDataFormat32 = 4,
  BufDataFormat16_16 = 5,
  BufDataFormat10_11_11 = 6,
  BufDataFormat11_11_10 = 7,
  BufDataFormat10_10_10_2 = 8,
  BufDataFormat2_10_10_10 = 9,
  BufDataFormat8_8_8_8 = 10,
  BufDataFormat32_32 = 11,
  BufDataFormat16_16_16_16 = 12,
  BufDataFormat32_32_32 = 13,
  BufDataFormat32_32_32_32 = 14,
  BufDataFormatReserved = 15,
  // Extra formats not in GFX9 hardware encoding:
  BufDataFormat8_8_8_8_Bgra,
  BufDataFormat8_8_8,
  BufDataFormat8_8_8_Bgr,
  BufDataFormat2_10_10_10_Bgra,
  BufDataFormat64,
  BufDataFormat64_64,
  BufDataFormat64_64_64,
  BufDataFormat64_64_64_64,
  BufDataFormat4_4,
  BufDataFormat4_4_4_4,
  BufDataFormat4_4_4_4_Bgra,
  BufDataFormat5_6_5,
  BufDataFormat5_6_5_Bgr,
  BufDataFormat5_6_5_1,
  BufDataFormat5_6_5_1_Bgra,
  BufDataFormat1_5_6_5,
  BufDataFormat5_9_9_9,
};

// Numeric format of vertex buffer entry. These match the GFX9 hardware encoding.
enum BufNumFormat {
  BufNumFormatUnorm = 0,
  BufNumFormatSnorm = 1,
  BufNumFormatUscaled = 2,
  BufNumFormatSscaled = 3,
  BufNumFormatUint = 4,
  BufNumFormatSint = 5,
  BufNumFormatSnorm_Ogl = 6,
  BufNumFormatFloat = 7,
  // Extra formats not in GFX9 hardware encoding:
  BufNumFormatSrgb,
  BufNumFormatOther,
};

// Rate of vertex input. This encodes both the "rate" (none/vertex/instance), and, for "instance",
// the divisor that determines how many instances share the same vertex buffer element.
enum VertexInputRate {
  VertexInputRateVertex = ~0,  // Vertex buffer has one element per vertex
  VertexInputRateNone = 0,     // Vertex buffer has one element shared between all instances
  VertexInputRateInstance = 1, // Vertex buffer has one element per instance
                               // Other value N means vertex buffer has one element per N instances;
                               //  N is the divisor.
};

// Structure for a vertex input
struct VertexInputDescription {
  unsigned location;  // Location of input, as provided to CreateReadGenericInput
  unsigned binding;   // Index of the vertex buffer descriptor in the vertex buffer table
  unsigned offset;    // Byte offset of the input in the binding's vertex buffer
  unsigned stride;    // Byte stride of per-vertex/per-instance elements in the vertex buffer, 0 if unknown.
                      // The stride is passed only to ensure that a valid load is used, not to actually calculate
                      // the load address. Instead, we use the index as the index in a structured tbuffer load
                      // instruction, and rely on the driver setting up the descriptor with the correct stride.
  BufDataFormat dfmt; // Data format of input; one of the BufDataFormat* values
  BufNumFormat nfmt;  // Numeric format of input; one of the BufNumFormat* values
  unsigned inputRate; // Vertex input rate for the binding
};

// Represents assistant info for each vertex attribute in uber fetch shader
struct UberFetchShaderAttribInfo {
  uint32_t binding : 8;       //< Attribute binding in vertex buffer table
  uint32_t perInstance : 1;   //< Whether vertex input rate is per-instance
  uint32_t isCurrent : 1;     //< Whether it is a current attribute
  uint32_t isPacked : 1;      //< Whether it is a packed format
  uint32_t isFixed : 1;       //< Whether it is a fixed format
  uint32_t componentSize : 4; //< Byte size per component
  uint32_t componentMask : 4; //< Component mask of this attribute.
  uint32_t isBgra : 1;        //< Whether is BGRA format
  uint32_t reserved : 11;     //< reserved bits in DWORD 0
  uint32_t offset;            //< Attribute offset
  uint32_t instanceDivisor;   //< Reciprocal of instance divisor
  uint32_t bufferFormat;      //< Buffer format info. it is a copy of buffer SRD DWORD3.
};

// A single color export format.
struct ColorExportFormat {
  BufDataFormat dfmt;            // Data format
  BufNumFormat nfmt;             // Numeric format
  unsigned blendEnable;          // Blend will be enabled for this target at draw time
  unsigned blendSrcAlphaToColor; // Whether source alpha is blended to color channels for this target
                                 //  at draw time
};

// Struct to pass to SetColorExportState
struct ColorExportState {
  unsigned alphaToCoverageEnable; // Enable alpha to coverage
  unsigned dualSourceBlendEnable; // Blend state bound at draw time will use a dual source blend mode
};

// Struct to pass to SetInputAssemblyState.
struct InputAssemblyState {
  PrimitiveType primitiveType; // Primitive type
  unsigned disableVertexReuse; // Disable reusing vertex shader output for indexed draws
  unsigned switchWinding;      // Whether to reverse vertex ordering for tessellation
  unsigned enableMultiView;    // Whether to enable multi-view support
};

// Shading rate flags. These happen to have the same values as the corresponding SPIR-V enum.
enum ShadingRateFlags : unsigned {
  ShadingRateNone = 0,
  ShadingRateVertical2Pixels = 1,
  ShadingRateVertical4Pixels = 2,
  ShadingRateHorizontal2Pixels = 4,
  ShadingRateHorizontal4Pixels = 8,
};

// Specifies which vertex of a primitive is the _provoking vertex_, this impacts which vertex's
// "flat" VS outputs are passed to the PS.
enum ProvokingVertexMode : unsigned {
  ProvokingVertexFirst = 0, // The provoking vertex is the first non-adjacency vertex used by a primitive.
  ProvokingVertexLast = 1,  // The provoking vertex is the last non-adjacency vertex used by a primitive.
};

// Struct to pass to SetRasterizerState
struct RasterizerState {
  unsigned rasterizerDiscardEnable; // Kill all rasterized pixels. This is implicitly true if stream out
                                    //  is enabled and no streams are rasterized
  unsigned innerCoverage;           // Related to conservative rasterization.  Must be false if
                                    //  conservative rasterization is disabled.
  unsigned perSampleShading;        // Enable per sample shading
  unsigned numSamples;              // Number of coverage samples used when rendering with this pipeline
  unsigned samplePatternIdx;        // Index into the currently bound MSAA sample pattern table that
                                    //  matches the sample pattern used by the rasterizer when rendering
                                    //  with this pipeline.
  unsigned usrClipPlaneMask;        // Mask to indicate the enabled user defined clip planes

  ProvokingVertexMode provokingVertexMode; // Specifies which vertex of a primitive is the _provoking vertex_,
                                           // this impacts which vertex's "flat" VS outputs are passed to the PS.
};

// Struct to pass to depth/stencil state
struct DepthStencilState {
  bool depthTestEnable;           // Whether enable depth test
  unsigned depthCompareOp;        // Depth compare operation
  bool stencilTestEnable;         // Whether enable stencil test
  unsigned stencilCompareOpFront; // Stencil compare operation for front face
  unsigned stencilCompareOpBack;  // Stencil compare operation for back face
};

// =====================================================================================================================
// Structs for setting shader modes, e.g. Builder::SetCommonShaderMode

// FP rounding mode. These happen to have values one more than the corresponding register field in current
// hardware, so we can make the zero initializer equivalent to DontCare.
enum class FpRoundMode : unsigned {
  DontCare, // Don't care
  Even,     // Round to nearest even
  Positive, // Round up towards positive infinity
  Negative, // Round down towards negative infinity
  Zero      // Round towards zero
};

// Denormal flush mode. These happen to have values one more than the corresponding register field in current
// hardware, so we can make the zero initializer equivalent to DontCare.
enum class FpDenormMode : unsigned {
  DontCare,   // Don't care
  FlushInOut, // Flush input/output denormals
  FlushOut,   // Flush only output denormals
  FlushIn,    // Flush only input denormals
  FlushNone   // Don't flush any denormals
};

// Struct to pass to SetCommonShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct CommonShaderMode {
  FpRoundMode fp16RoundMode;
  FpDenormMode fp16DenormMode;
  FpRoundMode fp32RoundMode;
  FpDenormMode fp32DenormMode;
  FpRoundMode fp64RoundMode;
  FpDenormMode fp64DenormMode;
  unsigned useSubgroupSize; // True if shader relies on SubgroupSize
};

// Tessellation vertex spacing
enum class VertexSpacing : unsigned {
  Unknown,
  Equal,
  FractionalEven,
  FractionalOdd,
};

// Tessellation vertex order
enum class VertexOrder : unsigned {
  Unknown,
  Ccw,
  Cw,
};

// Tessellation primitive mode
enum class PrimitiveMode : unsigned {
  Unknown,
  Triangles,
  Quads,
  Isolines,
};

// Struct to pass to SetTessellationMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct TessellationMode {
  VertexSpacing vertexSpacing; // Vertex spacing
  VertexOrder vertexOrder;     // Vertex ordering
  PrimitiveMode primitiveMode; // Tessellation primitive mode
  unsigned pointMode;          // Whether point mode is specified
  unsigned outputVertices;     // Number of produced vertices in the output patch
  unsigned inputVertices;      // Number of input vertices in the input patch.
};

// Kind of GS input primitives.
enum class InputPrimitives : unsigned { Points, Lines, LinesAdjacency, Triangles, TrianglesAdjacency };

// Kind of GS/mesh shader output primitives
enum class OutputPrimitives : unsigned {
  Points,       // GS or mesh shader
  Lines,        // Mesh shader only
  LineStrip,    // GS only
  Triangles,    // Mesh shader only
  TriangleStrip // GS only
};

// Struct to pass to SetGeometryShaderMode. The front-end should zero-initialize it with "= {}" in case
// future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct GeometryShaderMode {
  InputPrimitives inputPrimitive;   // Kind of input primitives
  OutputPrimitives outputPrimitive; // Kind of output primitives
  unsigned invocations;             // Number of times to invoke shader for each input primitive
  unsigned outputVertices;          // Max number of vertices the shader will emit in one invocation
};

// Struct to pass to MeshShaderMode. The front-end should zero-initialize it with "= {}" in case
// future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct MeshShaderMode {
  OutputPrimitives outputPrimitive; // Kind of output primitives
  unsigned outputVertices;          // Max number of vertices the shader will emit in the invocation group
  unsigned outputPrimitives;        // Max number of primitives the shader will emit in the invocation group
  unsigned workgroupSizeX;          // X dimension of workgroup size. 0 is taken to be 1
  unsigned workgroupSizeY;          // Y dimension of workgroup size. 0 is taken to be 1
  unsigned workgroupSizeZ;          // Z dimension of workgroup size. 0 is taken to be 1
};

// Kind of conservative depth/stencil
enum class ConservativeDepth : unsigned { Any, LessEqual, GreaterEqual };

// Struct to pass to SetFragmentShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct FragmentShaderMode {
  unsigned pixelCenterInteger;
  unsigned earlyFragmentTests;
  unsigned postDepthCoverage;
  unsigned earlyAndLatFragmentTests;
  ConservativeDepth conservativeDepth;
  ConservativeDepth conservativeStencilFront;
  ConservativeDepth conservativeStencilBack;
};

// Kind of derivativeMode:
// None: Return 0 for derivatives calculation of compute shader
// Linear: Calculating derivatives in linear mode(4*1)
// Quads: Calculating derivatives in Quads mode(2*2)
enum class DerivativeMode : unsigned { None, Linear, Quads };

// Struct to pass to SetComputeShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct ComputeShaderMode {
  unsigned workgroupSizeX;    // X dimension of workgroup size. 0 is taken to be 1
  unsigned workgroupSizeY;    // Y dimension of workgroup size. 0 is taken to be 1
  unsigned workgroupSizeZ;    // Z dimension of workgroup size. 0 is taken to be 1
  DerivativeMode derivatives; // derivativeMode for computeShader
};

// Enum passed to Pipeline::irLink to give information on whether this is a whole or part pipeline.
enum class PipelineLink : unsigned {
  WholePipeline, // Compiling a whole pipeline
  Unlinked,      // Compiling a shader or part-pipeline that will be ELF linked later
  PartPipeline,  // Compiling in the part-pipeline scheme, compiling the FS first and then using metadata to
                 //  pass its packed input mapping to the compile of the rest of the pipeline.
};

// =====================================================================================================================
// The public API of the middle-end pipeline state exposed to the front-end for setting state and linking and
// generating the pipeline
class Pipeline {
public:
  Pipeline(LgcContext *builderContext) : m_builderContext(builderContext) {}

  virtual ~Pipeline() {}

  // Get LgcContext
  LgcContext *getLgcContext() const { return m_builderContext; }

  // Get LLVMContext
  llvm::LLVMContext &getContext() const;

  // -----------------------------------------------------------------------------------------------------------------
  // Methods to set shader modes (FP modes, tessellation modes, fragment modes, workgroup size) for the current
  // shader that come from the input language.

  // Set the common shader mode for the given shader stage, containing hardware FP round and denorm modes.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  static void setCommonShaderMode(llvm::Module &module, ShaderStage shaderStage,
                                  const CommonShaderMode &commonShaderMode);

  // Get the common shader mode for the given shader stage.
  static CommonShaderMode getCommonShaderMode(llvm::Module &module, ShaderStage shaderStage);

  // Set the tessellation mode. This can be called in multiple shaders, and the values are merged
  // together -- a zero value in one call is overridden by a non-zero value in another call. LLPC needs
  // that because SPIR-V allows some of these execution mode items to appear in either the TCS or TES.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  static void setTessellationMode(llvm::Module &module, ShaderStage shaderStage,
                                  const TessellationMode &tessellationMode);

  // Get the tessellation mode for the given shader stage.
  static TessellationMode getTessellationMode(llvm::Module &module, ShaderStage shaderStage);

  // Set the geometry shader state.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  static void setGeometryShaderMode(llvm::Module &module, const GeometryShaderMode &geometryShaderMode);

  // Set the mesh shader state.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  static void setMeshShaderMode(llvm::Module &module, const MeshShaderMode &meshShaderMode);

  // Set the fragment shader mode.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  static void setFragmentShaderMode(llvm::Module &module, const FragmentShaderMode &fragmentShaderMode);

  // Set the compute shader modes.
  // The client should always zero-initialize the struct before setting it up, in case future versions
  // add more fields. A local struct variable can be zero-initialized with " = {}".
  static void setComputeShaderMode(llvm::Module &module, const ComputeShaderMode &computeShaderMode);

  // Set subgroup size usage.
  static void setSubgroupSizeUsage(llvm::Module &module, ShaderStage stage, bool usage);

  // Get the compute shader mode (workgroup size)
  static ComputeShaderMode getComputeShaderMode(llvm::Module &module);

  // -----------------------------------------------------------------------------------------------------------------
  // State setting methods

  // Set whether pre-rasterization part has a geometry shader
  // NOTE: Only applicable in the part pipeline compilation mode.
  virtual void setPreRasterHasGs(bool preRasterHasGs) = 0;

  // Set client name
  virtual void setClient(llvm::StringRef client) = 0;

  // Set and get per-pipeline options
  virtual void setOptions(const Options &options) = 0;
  virtual const Options &getOptions() const = 0;

  // Set per-shader options
  virtual void setShaderOptions(ShaderStage stage, const ShaderOptions &options) = 0;

  // Set the resource mapping nodes for the pipeline. "nodes" describes the user data
  // supplied to the shader as a hierarchical table (max two levels) of descriptors.
  // "immutableDescs" contains descriptors (currently limited to samplers), whose values are hard
  // coded by the application. Each one is a duplicate of one in "nodes". A use of one of these immutable
  // descriptors in the applicable Create* method is converted directly to the constant value.
  //
  // If using a BuilderImpl, this method must be called before any Create* methods.
  // If using a BuilderRecorder, it can be delayed until after linking.
  //
  // @param nodes : The resource mapping nodes. Only used for the duration of the call; the call copies the nodes.
  virtual void setUserDataNodes(llvm::ArrayRef<ResourceNode> nodes) = 0;

  // Set device index.
  virtual void setDeviceIndex(unsigned deviceIndex) = 0;

  // Set vertex input descriptions. Each location referenced in a call to CreateReadGenericInput in the
  // vertex shader must have a corresponding description provided here.
  virtual void setVertexInputDescriptions(llvm::ArrayRef<VertexInputDescription> inputs) = 0;

  // Set color export state.
  // The client should always zero-initialize the ColorExportState struct before setting it up, in case future
  // versions add more fields. A local struct variable can be zero-initialized with " = {}".
  //
  // @param formats : Array of ColorExportFormat structs
  // @param exportState : Color export flags
  virtual void setColorExportState(llvm::ArrayRef<ColorExportFormat> formats, const ColorExportState &exportState) = 0;

  // Set graphics state (input-assembly, rasterizer).
  // The front-end should zero-initialize each struct with "= {}" in case future changes add new fields.
  virtual void setGraphicsState(const InputAssemblyState &iaState, const RasterizerState &rsState) = 0;

  // Set depth/stencil state
  virtual void setDepthStencilState(const DepthStencilState &dsState) = 0;

  // Set the finalized 128-bit cache hash that is used to find this pipeline in the cache for the given version of LLPC.
  virtual void set128BitCacheHash(const Hash128 &finalizedCacheHash, const llvm::VersionTuple &version) = 0;

  // Set entire pipeline state from metadata in an IR module. This is used by the lgc command-line utility
  // for its link option.
  virtual void setStateFromModule(llvm::Module *module) = 0;

  // Set the "other part-pipeline" from the given other Pipeline object. This is used when doing a part-pipeline
  // compile of the non-FS part of the pipeline, to inherit required information from the FS part-pipeline.
  //
  // @param otherPartPipeline : The other part-pipeline, containing metadata for FS input mappings
  // @param module : If called before Pipeline::irLink(), should be nullptr. If called after Pipeline::irLink(), should
  //                 be the linked IR module, so the PAL metadata that needs to be inherited from otherPartPipeline
  //                 can be recorded in the module. The latter is provided as a hook for the LGC tool, which does not
  //                 do an irLink() at all.
  virtual void setOtherPartPipeline(Pipeline &otherPartPipeline, llvm::Module *linkedModule = nullptr) = 0;

  // Set the client-defined metadata to be stored inside the ELF
  virtual void setClientMetadata(llvm::StringRef clientMetadata) = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // IR link and generate pipeline/library methods

  // Mark a function as a shader entry-point. This must be done before linking shader modules into a pipeline
  // with irLink(). This is a static method in Pipeline, as it does not need a Pipeline object, and can be used
  // in the front-end before a shader is associated with a pipeline.
  //
  // @param func : Function to mark
  // @param stage : Shader stage, or ShaderStageInvalid if none
  static void markShaderEntryPoint(llvm::Function *func, ShaderStage stage);

  // Get a function's shader stage.
  //
  // @param func : Function to check
  // @returns stage : Shader stage, or ShaderStageInvalid if none
  static ShaderStage getShaderStage(llvm::Function *func);

  // Link the individual shader modules into a single pipeline module. The front-end must have
  // finished calling Builder::Create* methods and finished building the IR. In the case that
  // there are multiple shader modules, they are all freed by this call, and the linked pipeline
  // module is returned. If there is a single shader module, this might instead just return that.
  //
  // Before calling this, each shader module needs to have exactly one public (external linkage) function
  // for the shader entry-point that was marked by calling markShaderEntryPoint(). Any other functions in the
  // module must not have a non-default DLL storage class, and typically have internal linkage. However, for
  // a compute shader containing functions accessed by libraries, those functions need to be public (external
  // linkage).
  //
  // In the case of a compute library, there is no shader entry-point marked by calling
  // markShaderEntryPoint(). All functions must have default DLL storage class, and any that need to
  // be externally accessible need to be public (external linkage).
  //
  // Returns the pipeline/library module, or nullptr on link failure.
  //
  // @param modules : Array of modules
  // @param pipelineLink : Enum saying whether this is a pipeline, unlinked or part-pipeline compile.
  virtual llvm::Module *irLink(llvm::ArrayRef<llvm::Module *> modules, PipelineLink pipelineLink) = 0;

  // Typedef of function passed in to Generate to check the shader cache.
  // Returns the updated shader stage mask, allowing the client to decide not to compile shader stages
  // that got a hit in the cache.
  //    @param module : Module
  //    @param stageMask : Shader stage mask
  //    @param stageHashes : Per-stage hash of in/out usage
  //    @returns : Stage mask of stages not found in cache
  using CheckShaderCacheFunc = std::function<unsigned(const llvm::Module *module, unsigned stageMask,
                                                      llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes)>;

  // Do an early check for ability to use unlinked shader compilation then ELF linking.
  // Intended to be used when doing unlinked shader compilation with pipeline state already available.
  // It gives an early indication that there is something in the pipeline state (such as compact buffer
  // descriptors) that stops ELF linking working. It does not necessarily spot all such conditions, but
  // it can be useful in avoiding an unnecessary shader compile before falling back to full-pipeline
  // compilation.
  //
  // @returns : True for success, false if some reason for failure found, in which case getLastError()
  //           returns a textual description
  virtual bool checkElfLinkable() = 0;

  // Generate pipeline/library module or unlinked shader or part-pipeline module by running patch, middle-end
  // optimization and backend codegen passes.
  // The output is normally ELF, but IR assembly if an option is used to stop compilation early,
  // or ISA assembly if -filetype=asm is specified.
  // Output is written to outStream.
  //
  // Like other LGC and LLVM library functions, an internal compiler error could cause an assert or report_fatal_error.
  //
  // @param pipelineModule : IR pipeline module
  // @param [in/out] outStream : Stream to write ELF or IR disassembly output
  // @param checkShaderCacheFunc : Function to check shader cache in graphics pipeline
  // @param timers : Optional timers for 0 or more of:
  //                 timers[0]: patch passes
  //                 timers[1]: LLVM optimizations
  //                 timers[2]: codegen
  // @param otherElf : Optional ELF for the other part-pipeline when compiling an unlinked part-pipeline ELF.
  // @returns : True for success.
  //           False if irLink asked for an "unlinked" shader or part-pipeline, and there is some reason why the
  //           module cannot be compiled that way.  The client typically then does a whole-pipeline compilation
  //           instead. The client can call getLastError() to get a textual representation of the error, for
  //           use in logging or in error reporting in a command-line utility.
  virtual bool generate(std::unique_ptr<llvm::Module> pipelineModule, llvm::raw_pwrite_stream &outStream,
                        CheckShaderCacheFunc checkShaderCacheFunc, llvm::ArrayRef<llvm::Timer *> timers) = 0;

  // Version of generate() that does not take ownership of the Module.
  virtual bool generate(llvm::Module *pipelineModule, llvm::raw_pwrite_stream &outStream,
                        CheckShaderCacheFunc checkShaderCacheFunc, llvm::ArrayRef<llvm::Timer *> timers) = 0;

  // Create an ELF linker object for linking unlinked shader or part-pipeline ELFs into a pipeline ELF using
  // the pipeline state. This needs to be deleted after use.
  virtual ElfLinker *createElfLinker(llvm::ArrayRef<llvm::MemoryBufferRef> elfs) = 0;

  // Get a textual error message for the last recoverable error caused by generate() or one of the ElfLinker
  // methods finding something about the shaders or pipeline state that means that shader compilation then
  // linking cannot be done. This error message is intended only for logging or command-line error reporting.
  //
  // @returns : Error message from last such recoverable error; remains valid until next time generate() or
  //           one of the ElfLinker methods is called, or the Pipeline object is destroyed
  virtual llvm::StringRef getLastError() = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // Non-compiling methods

  // Compute the ExportFormat (as an opaque int) of the specified color export location with the specified output
  // type. Only the number of elements of the type is significant.
  // This is not used in a normal compile; it is only used by amdllpc's -check-auto-layout-compatible option.
  virtual unsigned computeExportFormat(llvm::Type *outputTy, unsigned location) = 0;

private:
  LgcContext *m_builderContext; // Builder context
};

} // namespace lgc
