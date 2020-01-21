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
 * @file  llpcPipeline.h
 * @brief LLPC header file: contains declaration of class Llpc::Pipeline
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm
{

class Timer;

} // llvm

namespace Llpc
{

using namespace llvm;

class BuilderContext;

// =====================================================================================================================
// Per-pipeline and per-shader options for setting in pipeline state

// Bit values of NGG flags. This is done as bit values rather than bitfields so the flags word appears
// in a platform-independent way in IR metdata.
enum NggFlag : uint32_t
{
    NggFlagDisable = 0x0001,                      // Disable NGG
    NggFlagEnableGsUse = 0x0002,                  // Enable NGG when pipeline has GS
    NggFlagForceNonPassthrough = 0x0004,          // Force NGG to run in non-passthrough mode
    NggFlagDontAlwaysUsePrimShaderTable = 0x0008, // Don't always use primitive shader table to fetch culling-control
                                                  //   registers
    NggFlagCompactSubgroup = 0x0010,              // Compaction is based on the whole sub-group rather than on vertices
    NggFlagEnableFastLaunch = 0x0020,             // Enable the hardware to launch subgroups of work at a faster rate
    NggFlagEnableVertexReuse = 0x0040,            // Enable optimization to cull duplicate vertices
    NggFlagEnableBackfaceCulling = 0x0080,        // Enable culling of primitives that don't meet facing criteria
    NggFlagEnableFrustumCulling = 0x0100,         // Enable discarding of primitives outside of view frustum
    NggFlagEnableBoxFilterCulling = 0x0200,       // Enable simpler frustum culler that is less accurate
    NggFlagEnableSphereCulling = 0x0400,          // Enable frustum culling based on a sphere
    NggFlagEnableSmallPrimFilter = 0x0800,        // Enable trivial sub-sample primitive culling
    NggFlagEnableCullDistanceCulling = 0x1000,    // Enable culling when "cull distance" exports are present
};

// Middle-end per-pipeline options to pass to SetOptions.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct Options
{
    uint64_t              hash[2];                 // Pipeline hash to set in ELF PAL metadata
    uint32_t              includeDisassembly;      // If set, the disassembly for all compiled shaders will be included
                                                   //   in the pipeline ELF.
    uint32_t              reconfigWorkgroupLayout; // If set, allows automatic workgroup reconfigure to take place on
                                                   //   compute shaders.
    uint32_t              includeIr;               // If set, the IR for all compiled shaders will be included in the
                                                   //   pipeline ELF.
    uint32_t              nggFlags;                // Flags to control NGG (NggFlag* values ored together)
    uint32_t              nggBackfaceExponent;     // Value from 1 to UINT32_MAX that will cause the backface culling
                                                   // algorithm to ignore area calculations that are less than
                                                   // (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
                                                   //  Only valid if the NGG backface culler is enabled.
                                                   //  A value of 0 will disable the threshold.
    NggSubgroupSizingType nggSubgroupSizing;       // NGG subgroup sizing type
    uint32_t              nggVertsPerSubgroup;     // How to determine NGG verts per subgroup
    uint32_t              nggPrimsPerSubgroup;     // How to determine NGG prims per subgroup
};

// Middle-end per-shader options to pass to SetShaderOptions.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct ShaderOptions
{
    uint64_t      hash[2];        // Shader hash to set in ELF PAL metadata
    uint32_t      trapPresent;    // Indicates a trap handler will be present when this pipeline is executed,
                                  //  and any trap conditions encountered in this shader should call the trap
                                  //  handler. This could include an arithmetic exception, an explicit trap
                                  //  request from the host, or a trap after every instruction when in debug
                                  //  mode.
    uint32_t      debugMode;      // When set, this shader should cause the trap handler to be executed after
                                  //  every instruction.  Only valid if trapPresent is set.
    uint32_t      allowReZ;       // Allow the DB ReZ feature to be enabled.  This will cause an early-Z test
                                  //  to potentially kill PS waves before launch, and also issues a late-Z test
                                  //  in case the PS kills pixels.  Only valid for pixel shaders.

    // Maximum VGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
    // of this value and whatever the target GPU supports. To effectively disable this limit, set this to 0.
    uint32_t  vgprLimit;

    // Maximum SGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
    // of this value and whatever the target GPU supports. To effectively disable this limit, set this to 0.
    uint32_t  sgprLimit;

    /// Overrides the number of CS thread-groups which the GPU will launch per compute-unit. This throttles the
    /// shader, which can sometimes enable more graphics shader work to complete in parallel. A value of zero
    /// disables limiting the number of thread-groups to launch. This field is ignored for graphics shaders.
    uint32_t  maxThreadGroupsPerComputeUnit;

#if LLPC_BUILD_GFX10
    uint32_t      waveSize;       // Control the number of threads per wavefront (GFX10+)
    uint32_t      subgroupSize;   // Override for the wave size when the shader uses gl_SubgroupSize, 0 for no override
    uint32_t      wgpMode;        // Whether to choose WGP mode or CU mode (GFX10+)
    WaveBreakSize waveBreakSize;  // Size of region to force the end of a wavefront (GFX10+).
                                  // Only valid for fragment shaders.
#endif

    // Vector szie threshold for load scalarizer. 0 means do not scalarize loads at all.
    uint32_t  loadScalarizerThreshold;

    // Use the LLVM backend's SI scheduler instead of the default scheduler.
    bool      useSiScheduler;

    /// Default unroll threshold for LLVM.
    uint32_t  unrollThreshold;
};

// =====================================================================================================================
// Structs for setting pipeline state.
// The front-end should zero-initialize a struct with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.

// Primitive topology. These happen to have the same values as the corresponding Vulkan enum.
enum class PrimitiveTopology : uint32_t
{
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
enum BufDataFormat
{
    BufDataFormatInvalid                     = 0,
    BufDataFormat8                           = 1,
    BufDataFormat16                          = 2,
    BufDataFormat8_8                         = 3,
    BufDataFormat32                          = 4,
    BufDataFormat16_16                       = 5,
    BufDataFormat10_11_11                    = 6,
    BufDataFormat11_11_10                    = 7,
    BufDataFormat10_10_10_2                  = 8,
    BufDataFormat2_10_10_10                  = 9,
    BufDataFormat8_8_8_8                     = 10,
    BufDataFormat32_32                       = 11,
    BufDataFormat16_16_16_16                 = 12,
    BufDataFormat32_32_32                    = 13,
    BufDataFormat32_32_32_32                 = 14,
    BufDataFormatReserved                    = 15,
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
enum BufNumFormat
{
    BufNumFormatUnorm                        = 0,
    BufNumFormatSnorm                        = 1,
    BufNumFormatUscaled                      = 2,
    BufNumFormatSscaled                      = 3,
    BufNumFormatUint                         = 4,
    BufNumFormatSint                         = 5,
    BufNumFormatSnorm_Ogl                    = 6,
    BufNumFormatFloat                        = 7,
    // Extra formats not in GFX9 hardware encoding:
    BufNumFormatSrgb,
    BufNumFormatOther,
};

// Rate of vertex input. This encodes both the "rate" (none/vertex/instance), and, for "instance",
// the divisor that determines how many instances share the same vertex buffer element.
enum VertexInputRate
{
    VertexInputRateVertex = ~0,     // Vertex buffer has one element per vertex
    VertexInputRateNone = 0,        // Vertex buffer has one element shared between all instances
    VertexInputRateInstance = 1,    // Vertex buffer has one element per instance
                                    // Other value N means vertex buffer has one element per N instances;
                                    //  N is the divisor.
};

// Structure for a vertex input
struct VertexInputDescription
{
    uint32_t        location;       // Location of input, as provided to CreateReadGenericInput
    uint32_t        binding;        // Index of the vertex buffer descriptor in the vertex buffer table
    uint32_t        offset;         // Byte offset of the input in the binding's vertex buffer
    uint32_t        stride;         // Byte stride of per-vertex/per-instance elements in the vertex buffer
    BufDataFormat   dfmt;           // Data format of input; one of the BufDataFormat* values
    BufNumFormat    nfmt;           // Numeric format of input; one of the BufNumFormat* values
    uint32_t        inputRate;      // Vertex input rate for the binding
};

// A single color export format.
struct ColorExportFormat
{
    BufDataFormat dfmt;                 // Data format
    BufNumFormat  nfmt;                 // Numeric format
    uint32_t      blendEnable;          // Blend will be enabled for this target at draw time
    uint32_t      blendSrcAlphaToColor; // Whether source alpha is blended to color channels for this target
                                        //  at draw time
};

// Struct to pass to SetColorExportState
struct ColorExportState
{
    uint32_t alphaToCoverageEnable;          // Enable alpha to coverage
    uint32_t dualSourceBlendEnable;          // Blend state bound at draw time will use a dual source blend mode
};

// Struct to pass to SetInputAssemblyState.
struct InputAssemblyState
{
    PrimitiveTopology topology;           // Primitive topology
    uint32_t          patchControlPoints; // Number of control points for PrimitiveTopology::PatchList
    uint32_t          disableVertexReuse; // Disable reusing vertex shader output for indexed draws
    uint32_t          switchWinding;      // Whether to reverse vertex ordering for tessellation
    uint32_t          enableMultiView;    // Whether to enable multi-view support
};

// Struct to pass to SetViewportState.
struct ViewportState
{
    uint32_t          depthClipEnable;    // Enable clipping based on Z coordinate
};

// Polygon mode. These happen to have the same values as the corresponding Vulkan enum.
enum PolygonMode : uint32_t
{
    PolygonModeFill = 0,
    PolygonModeLine = 1,
    PolygonModePoint = 2,
};

// Fragment cull mode flags. These happen to have the same values as the corresponding Vulkan enum.
enum CullModeFlags : uint32_t
{
    CullModeNone = 0,
    CullModeFront = 1,
    CullModeBack = 2,
    CullModeFrontAndBack = 3,
};

// Struct to pass to SetRasterizerState
struct RasterizerState
{
    uint32_t      rasterizerDiscardEnable;  // Kill all rasterized pixels. This is implicitly true if stream out
                                            //  is enabled and no streams are rasterized
    uint32_t      innerCoverage;            // Related to conservative rasterization.  Must be false if
                                            //  conservative rasterization is disabled.
    uint32_t      perSampleShading;         // Enable per sample shading
    uint32_t      numSamples;               // Number of coverage samples used when rendering with this pipeline
    uint32_t      samplePatternIdx;         // Index into the currently bound MSAA sample pattern table that
                                            //  matches the sample pattern used by the rasterizer when rendering
                                            //  with this pipeline.
    uint32_t      usrClipPlaneMask;         // Mask to indicate the enabled user defined clip planes
    PolygonMode   polygonMode;              // Polygon mode
    CullModeFlags cullMode;                 // Fragment culling mode
    uint32_t      frontFaceClockwise;       // Front-facing triangle orientation: false=counter, true=clockwise
    uint32_t      depthBiasEnable;          // Whether to bias fragment depth values
};

// =====================================================================================================================
// Structs for setting shader modes, e.g. Builder::SetCommonShaderMode

// FP rounding mode. These happen to have values one more than the corresponding register field in current
// hardware, so we can make the zero initializer equivalent to DontCare.
enum class FpRoundMode : uint32_t
{
    DontCare,   // Don't care
    Even,       // Round to nearest even
    Positive,   // Round up towards positive infinity
    Negative,   // Round down tiwards negative infinity
    Zero        // Round towards zero
};

// Denormal flush mode. These happen to have values one more than the corresponding register field in current
// hardware, so we can make the zero initializer equivalent to DontCare.
enum class FpDenormMode :uint32_t
{
    DontCare,     // Don't care
    FlushInOut,   // Flush input/output denormals
    FlushOut,     // Flush only output denormals
    FlushIn,      // Flush only input denormals
    FlushNone     // Don't flush any denormals
};

// Struct to pass to SetCommonShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct CommonShaderMode
{
    FpRoundMode   fp16RoundMode;
    FpDenormMode  fp16DenormMode;
    FpRoundMode   fp32RoundMode;
    FpDenormMode  fp32DenormMode;
    FpRoundMode   fp64RoundMode;
    FpDenormMode  fp64DenormMode;
    uint32_t      useSubgroupSize;  // True if shader relies on SubgroupSize
};

// Tessellation vertex spacing
enum class VertexSpacing : uint32_t
{
    Unknown,
    Equal,
    FractionalEven,
    FractionalOdd,
};

// Tessellation vertex order
enum class VertexOrder : uint32_t
{
    Unknown,
    Ccw,
    Cw,
};

// Tessellation primitive mode
enum class PrimitiveMode : uint32_t
{
    Unknown,
    Triangles,
    Quads,
    Isolines,
};

// Struct to pass to SetTessellationMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct TessellationMode
{
    VertexSpacing vertexSpacing;  // Vertex spacing
    VertexOrder   vertexOrder;    // Vertex ordering
    PrimitiveMode primitiveMode;  // Tessellation primitive mode
    uint32_t      pointMode;      // Whether point mode is specified
    uint32_t      outputVertices; // Number of produced vertices in the output patch
};

// Kind of GS input primitives.
enum class InputPrimitives : uint32_t
{
    Points,
    Lines,
    LinesAdjacency,
    Triangles,
    TrianglesAdjacency
};

// Kind of GS output primitives
enum class OutputPrimitives : uint32_t
{
    Points,
    LineStrip,
    TriangleStrip
};

// Struct to pass to SetGeometryShaderMode. The front-end should zero-initialize it with "= {}" in case
// future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct GeometryShaderMode
{
    InputPrimitives   inputPrimitive;   // Kind of input primitives
    OutputPrimitives  outputPrimitive;  // Kind of output primitives
    uint32_t          invocations;      // Number of times to invoke shader for each input primitive
    uint32_t          outputVertices;   // Max number of vertices the shader will emit in one invocation
};

// Struct to pass to SetFragmentShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct FragmentShaderMode
{
    uint32_t  pixelCenterInteger;
    uint32_t  earlyFragmentTests;
    uint32_t  postDepthCoverage;
};

// Struct to pass to SetComputeShaderMode.
// The front-end should zero-initialize it with "= {}" in case future changes add new fields.
// All fields are uint32_t, even those that could be bool, because the way the state is written to and read
// from IR metadata relies on that.
struct ComputeShaderMode
{
    uint32_t  workgroupSizeX;     // X dimension of workgroup size. 0 is taken to be 1
    uint32_t  workgroupSizeY;     // Y dimension of workgroup size. 0 is taken to be 1
    uint32_t  workgroupSizeZ;     // Z dimension of workgroup size. 0 is taken to be 1
};

// =====================================================================================================================
// The public API of the middle-end pipeline state exposed to the front-end for setting state and linking and
// generating the pipeline
class Pipeline
{
public:
    Pipeline(BuilderContext* pBuilderContext)
        : m_pBuilderContext(pBuilderContext)
    {}

    virtual ~Pipeline() {}

    // Get BuilderContext
    BuilderContext* GetBuilderContext() const { return m_pBuilderContext; }

    // Get LLVMContext
    LLVMContext& GetContext() const;

    // -----------------------------------------------------------------------------------------------------------------
    // State setting methods

    // Set the shader stage mask
    virtual void SetShaderStageMask(uint32_t mask) = 0;

    // Set and get per-pipeline options
    virtual void SetOptions(const Options& options) = 0;
    virtual const Options& GetOptions() = 0;

    // Set per-shader options
    virtual void SetShaderOptions(ShaderStage stage, const ShaderOptions& options) = 0;

    // Set the resource mapping nodes for the pipeline. "nodes" describes the user data
    // supplied to the shader as a hierarchical table (max two levels) of descriptors.
    // "immutableDescs" contains descriptors (currently limited to samplers), whose values are hard
    // coded by the application. Each one is a duplicate of one in "nodes". A use of one of these immutable
    // descriptors in the applicable Create* method is converted directly to the constant value.
    //
    // If using a BuilderImpl, this method must be called before any Create* methods.
    // If using a BuilderRecorder, it can be delayed until after linking.
    virtual void SetUserDataNodes(
        ArrayRef<ResourceMappingNode>   nodes,            // The resource mapping nodes
        ArrayRef<DescriptorRangeValue>  rangeValues) = 0; // The descriptor range values

    // Set device index.
    virtual void SetDeviceIndex(uint32_t deviceIndex) = 0;

    // Set vertex input descriptions. Each location referenced in a call to CreateReadGenericInput in the
    // vertex shader must have a corresponding description provided here.
    virtual void SetVertexInputDescriptions(ArrayRef<VertexInputDescription> inputs) = 0;

    // Set color export state.
    // The client should always zero-initialize the ColorExportState struct before setting it up, in case future
    // versions add more fields. A local struct variable can be zero-initialized with " = {}".
    virtual void SetColorExportState(
        ArrayRef<ColorExportFormat> formats,          // Array of ColorExportFormat structs
        const ColorExportState&     exportState) = 0; // [in] Color export flags

    // Set graphics state (input-assembly, viewport, rasterizer).
    // The front-end should zero-initialize each struct with "= {}" in case future changes add new fields.
    virtual void SetGraphicsState(const InputAssemblyState& iaState,
                                  const ViewportState&      vpState,
                                  const RasterizerState&    rsState) = 0;

    // -----------------------------------------------------------------------------------------------------------------
    // Link and generate pipeline methods

    // Link the individual shader modules into a single pipeline module. The front-end must have
    // finished calling Builder::Create* methods and finished building the IR. In the case that
    // there are multiple shader modules, they are all freed by this call, and the linked pipeline
    // module is returned. If there is a single shader module, this might instead just return that.
    // Before calling this, each shader module needs to have one global function for the shader
    // entrypoint, then all other functions with internal linkage.
    // Returns the pipeline module, or nullptr on link failure.
    virtual Module* Link(
        ArrayRef<Module*> modules) = 0; // Array of modules indexed by shader stage, with nullptr entry
                                        //  for any stage not present in the pipeline

    // Typedef of function passed in to Generate to check the shader cache.
    // Returns the updated shader stage mask, allowing the client to decide not to compile shader stages
    // that got a hit in the cache.
    typedef std::function<uint32_t(
        const Module*               pModule,      // [in] Module
        uint32_t                    stageMask,    // Shader stage mask
        ArrayRef<ArrayRef<uint8_t>> stageHashes   // Per-stage hash of in/out usage
    )> CheckShaderCacheFunc;

    // Generate pipeline module by running patch, middle-end optimization and backend codegen passes.
    // The output is normally ELF, but IR disassembly if an option is used to stop compilation early.
    // Output is written to outStream.
    // Like other Builder methods, on error, this calls report_fatal_error, which you can catch by setting
    // a diagnostic handler with LLVMContext::setDiagnosticHandler.
    virtual void Generate(
        std::unique_ptr<Module>   pipelineModule,       // IR pipeline module
        raw_pwrite_stream&        outStream,            // [in/out] Stream to write ELF or IR disassembly output
        CheckShaderCacheFunc      checkShaderCacheFunc, // Function to check shader cache in graphics pipeline
        ArrayRef<Timer*>          timers) = 0;          // Timers for: patch passes, llvm optimizations, codegen

    // -----------------------------------------------------------------------------------------------------------------
    // Non-compiling methods

    // Compute the ExportFormat (as an opaque int) of the specified color export location with the specified output
    // type. Only the number of elements of the type is significant.
    // This is not used in a normal compile; it is only used by amdllpc's -check-auto-layout-compatible option.
    virtual uint32_t ComputeExportFormat(Type* pOutputTy, uint32_t location) = 0;

private:
    BuilderContext*                 m_pBuilderContext;                  // Builder context
};

} // Llpc
