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
// in a platform-independent way in IR metdata.
enum NggFlag : unsigned {
  NggFlagDisable = 0x0001,                      // Disable NGG
  NggFlagEnableGsUse = 0x0002,                  // Enable NGG when pipeline has GS
  NggFlagForceCullingMode = 0x0004,             // Force NGG to run in culling mode
  NggFlagDontAlwaysUsePrimShaderTable = 0x0008, // Don't always use primitive shader table to fetch culling-control
                                                //   registers
  NggFlagCompactDisable = 0x0010,               // Vertex compaction is disabled
  NggFlagEnableVertexReuse = 0x0020,            // Enable optimization to cull duplicate vertices
  NggFlagEnableBackfaceCulling = 0x0040,        // Enable culling of primitives that don't meet facing criteria
  NggFlagEnableFrustumCulling = 0x0080,         // Enable discarding of primitives outside of view frustum
  NggFlagEnableBoxFilterCulling = 0x0100,       // Enable simpler frustum culler that is less accurate
  NggFlagEnableSphereCulling = 0x0200,          // Enable frustum culling based on a sphere
  NggFlagEnableSmallPrimFilter = 0x0400,        // Enable trivial sub-sample primitive culling
  NggFlagEnableCullDistanceCulling = 0x0800,    // Enable culling when "cull distance" exports are present
};

// Enumerates various sizing options of sub-group size for NGG primitive shader.
enum class NggSubgroupSizing : unsigned {
  Auto,             ///< Sub-group size is allocated as optimally determined
  MaximumSize,      ///< Sub-group size is allocated to the maximum allowable size by the hardware
  HalfSize,         ///< Sub-group size is allocated as to allow half of the maximum allowable size
                    ///  by the hardware
  OptimizeForVerts, ///< Sub-group size is optimized for vertex thread utilization
  OptimizeForPrims, ///< Sub-group size is optimized for primitive thread utilization
  Explicit,         ///< Sub-group size is allocated based on explicitly-specified vertsPerSubgroup and
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
  None = 0x0,     ///< No wave break by region
  _8x8 = 0x1,     ///< Outside a 8x8 pixel region
  _16x16 = 0x2,   ///< Outside a 16x16 pixel region
  _32x32 = 0x3,   ///< Outside a 32x32 pixel region
};

// Value for shadowDescriptorTable pipeline option.
static const unsigned ShadowDescriptorTableDisable = ~0U;

// Middle-end per-pipeline options to pass to SetOptions.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
struct Options {
  uint64_t hash[2];                    // Pipeline hash to set in ELF PAL metadata
  unsigned includeDisassembly;         // If set, the disassembly for all compiled shaders will be included
                                       //   in the pipeline ELF.
  unsigned reconfigWorkgroupLayout;    // If set, allows automatic workgroup reconfigure to take place on
                                       //   compute shaders.
  unsigned includeIr;                  // If set, the IR for all compiled shaders will be included in the
                                       //   pipeline ELF.
  unsigned nggFlags;                   // Flags to control NGG (NggFlag* values ored together)
  unsigned nggBackfaceExponent;        // Value from 1 to UINT32_MAX that will cause the backface culling
                                       // algorithm to ignore area calculations that are less than
                                       // (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
                                       //  Only valid if the NGG backface culler is enabled.
                                       //  A value of 0 will disable the threshold.
  NggSubgroupSizing nggSubgroupSizing; // NGG subgroup sizing type
  unsigned nggVertsPerSubgroup;        // How to determine NGG verts per subgroup
  unsigned nggPrimsPerSubgroup;        // How to determine NGG prims per subgroup
  unsigned shadowDescriptorTable;      // High dword of shadow descriptor table address, or
                                       //   ShadowDescriptorTableDisable to disable shadow descriptor tables
  unsigned allowNullDescriptor;        // Allow and give defined behavior for null descriptor
  unsigned disableImageResourceCheck;  // Don't do image resource type check
};

// Middle-end per-shader options to pass to SetShaderOptions.
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

  ResourceNodeType type;   // Type of this node
  unsigned sizeInDwords;   // Size in dwords
  unsigned offsetInDwords; // Offset in dwords

  union {
    // Info for generic descriptor nodes.
    struct {
      unsigned set;                   // Descriptor set
      unsigned binding;               // Binding
      unsigned stride;                // Size of each descriptor in the indexable range in dwords.
      llvm::Constant *immutableValue; // Array of vectors of i32 constants for immutable value
    };

    // Info for DescriptorTableVaPtr
    llvm::ArrayRef<ResourceNode> innerTable;

    // Info for indirect data nodes (IndirectUserDataVaPtr, StreamOutVaTablePtr)
    unsigned indirectSizeInDwords;
  };
};

/// Represents the info of immutable descriptor.
struct ImmutableDescriptor {
  ResourceNodeType type; ///< Type of this resource node (currently, only sampler is supported)
  unsigned set;          ///< ID of descriptor set
  unsigned binding;      ///< ID of descriptor binding
  unsigned arraySize;    ///< Element count for arrayed binding
  const unsigned *value; ///< Static SRDs
};

// =====================================================================================================================
// Structs for setting pipeline state.
// The front-end should zero-initialize a struct with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.

// Primitive topology. These happen to have the same values as the corresponding Vulkan enum.
enum class PrimitiveTopology : unsigned {
  PointList = 0,
  LineList = 1,
  LineStrip = 2,
  TriangleList = 3,
  TriangleStrip = 4,
  TriangleFan = 5,
  LineListWithAdjacency = 6,
  LineStripWithAdjacency = 7,
  TriangleListWithAdjacency = 8,
  TriangleStripWithAdjacency = 9,
  PatchList = 10,
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
  PrimitiveTopology topology;  // Primitive topology
  unsigned patchControlPoints; // Number of control points for PrimitiveTopology::PatchList
  unsigned disableVertexReuse; // Disable reusing vertex shader output for indexed draws
  unsigned switchWinding;      // Whether to reverse vertex ordering for tessellation
  unsigned enableMultiView;    // Whether to enable multi-view support
};

// Struct to pass to SetViewportState.
struct ViewportState {
  unsigned depthClipEnable; // Enable clipping based on Z coordinate
};

// Polygon mode. These happen to have the same values as the corresponding Vulkan enum.
enum PolygonMode : unsigned {
  PolygonModeFill = 0,
  PolygonModeLine = 1,
  PolygonModePoint = 2,
};

// Fragment cull mode flags. These happen to have the same values as the corresponding Vulkan enum.
enum CullModeFlags : unsigned {
  CullModeNone = 0,
  CullModeFront = 1,
  CullModeBack = 2,
  CullModeFrontAndBack = 3,
};

// Shading rate flags. These happen to have the same values as the corresponding SPIR-V enum.
enum ShadingRateFlags : unsigned {
  ShadingRateNone = 0,
  ShadingRateVertical2Pixels = 1,
  ShadingRateVertical4Pixels = 2,
  ShadingRateHorizontal2Pixels = 4,
  ShadingRateHorizontal4Pixels = 8,
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
  PolygonMode polygonMode;          // Polygon mode
  CullModeFlags cullMode;           // Fragment culling mode
  unsigned frontFaceClockwise;      // Front-facing triangle orientation: false=counter, true=clockwise
  unsigned depthBiasEnable;         // Whether to bias fragment depth values
};

// =====================================================================================================================
// Structs for setting shader modes, e.g. Builder::SetCommonShaderMode

// FP rounding mode. These happen to have values one more than the corresponding register field in current
// hardware, so we can make the zero initializer equivalent to DontCare.
enum class FpRoundMode : unsigned {
  DontCare, // Don't care
  Even,     // Round to nearest even
  Positive, // Round up towards positive infinity
  Negative, // Round down tiwards negative infinity
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
};

// Kind of GS input primitives.
enum class InputPrimitives : unsigned { Points, Lines, LinesAdjacency, Triangles, TrianglesAdjacency };

// Kind of GS output primitives
enum class OutputPrimitives : unsigned { Points, LineStrip, TriangleStrip };

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

// Kind of conservative depth
enum class ConservativeDepth : unsigned { Any, LessEqual, GreaterEqual };

// Struct to pass to SetFragmentShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct FragmentShaderMode {
  unsigned pixelCenterInteger;
  unsigned earlyFragmentTests;
  unsigned postDepthCoverage;
  ConservativeDepth conservativeDepth;
};

// Struct to pass to SetComputeShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are unsigned, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct ComputeShaderMode {
  unsigned workgroupSizeX; // X dimension of workgroup size. 0 is taken to be 1
  unsigned workgroupSizeY; // Y dimension of workgroup size. 0 is taken to be 1
  unsigned workgroupSizeZ; // Z dimension of workgroup size. 0 is taken to be 1
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
  // State setting methods

  // Set the shader stage mask
  virtual void setShaderStageMask(unsigned mask) = 0;

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

  // Set graphics state (input-assembly, viewport, rasterizer).
  // The front-end should zero-initialize each struct with "= {}" in case future changes add new fields.
  virtual void setGraphicsState(const InputAssemblyState &iaState, const ViewportState &vpState,
                                const RasterizerState &rsState) = 0;

  // Set entire pipeline state from metadata in an IR module. This is used by the lgc command-line utility
  // for its link option.
  virtual void setStateFromModule(llvm::Module *module) = 0;

  // -----------------------------------------------------------------------------------------------------------------
  // IR link and generate pipeline/library methods

  // Mark a function as a shader entry-point. This must be done before linking shader modules into a pipeline
  // with irLink(). This is a static method in Pipeline, as it does not need a Pipeline object, and can be used
  // in the front-end before a shader is associated with a pipeline.
  //
  // @param func : Shader entry-point function
  // @param stage : Shader stage
  static void markShaderEntryPoint(llvm::Function *func, ShaderStage stage);

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
  // @param unlinked : True if generating an "unlinked" half-pipeline ELF that then needs further linking to
  //                   generate a pipeline ELF. In that case, using the methods above to set pipeline state items
  //                   (setUserDataNodes, setDeviceIndex, setVertexInputDescriptions, setColorExportState,
  //                   setGraphicsState) becomes optional, as LGC will generate relocs or user data entries
  //                   that are fixed up in the ELF link step.
  //                   TODO: That isn't implemented yet.
  virtual llvm::Module *irLink(llvm::ArrayRef<llvm::Module *> modules, bool unlinked) = 0;

  // Typedef of function passed in to Generate to check the shader cache.
  // Returns the updated shader stage mask, allowing the client to decide not to compile shader stages
  // that got a hit in the cache.
  typedef std::function<unsigned(const llvm::Module *module,                         // [in] Module
                                 unsigned stageMask,                                 // Shader stage mask
                                 llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes // Per-stage hash of in/out usage
                                 )>
      CheckShaderCacheFunc;

  // Do an early check for ability to use shader/half-pipeline compilation then ELF linking.
  // Intended to be used when doing shader/half-pipeline compilation with pipeline state already available.
  // It gives an early indication that there is something in the pipeline state (such as compact buffer
  // descriptors) that stops ELF linking working. It does not necessarily spot all such conditions, but
  // it can be useful in avoiding an unnecessary shader compile before falling back to full-pipeline
  // compilation.
  //
  // @returns : True for success, false if some reason for failure found, in which case getLastError()
  //           returns a textual description
  virtual bool checkElfLinkable() = 0;

  // Generate pipeline/library module or unlinked half-pipeline module by running patch, middle-end optimization
  // and backend codegen passes.
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
  // @param otherElf : Optional ELF for the other half-pipeline when compiling an unlinked half-pipeline ELF.
  //                   Supplying this could allow more optimal code for writing/reading attribute values between
  //                   the two pipeline halves
  // @returns : True for success.
  //           False if irLink asked for an "unlinked" shader or half-pipeline, and there is some reason why the
  //           module cannot be compiled that way.  The client typically then does a whole-pipeline compilation
  //           instead. The client can call getLastError() to get a textual representation of the error, for
  //           use in logging or in error reporting in a command-line utility.
  virtual bool generate(std::unique_ptr<llvm::Module> pipelineModule, llvm::raw_pwrite_stream &outStream,
                        CheckShaderCacheFunc checkShaderCacheFunc, llvm::ArrayRef<llvm::Timer *> timers,
                        llvm::MemoryBufferRef otherElf) = 0;

  // Create an ELF linker object for linking unlinked half-pipeline ELFs into a pipeline ELF using the pipeline state.
  // This needs to be deleted after use.
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
