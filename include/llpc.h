/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpc.h
 * @brief LLPC header file: contains LLPC basic definitions (including interfaces and data types).
 ***********************************************************************************************************************
 */
#pragma once

#include "vulkan.h"

// Confliction of Xlib and LLVM headers
#undef True
#undef False
#undef DestroyAll
#undef Status
#undef Bool

/// LLPC major interface version.
#define LLPC_INTERFACE_MAJOR_VERSION 32

/// LLPC minor interface version.
#define LLPC_INTERFACE_MINOR_VERSION 0

//**
//**********************************************************************************************************************
//* @page VersionHistory
//* %Version History
//* | %Version | Change Description                                                                                    |
//* | -------- | ----------------------------------------------------------------------------------------------------- |
//* |     33.0 | Add enableLoadScalarizer option into PipelineShaderOptions.                                           |
//* |     32.0 | Add ShdaerModuleOptions in ShaderModuleBuildInfo                                                      |
//* |     31.0 | Add PipelineShaderOptions::allowVaryWaveSize                                                          |
//* |     30.0 | Removed PipelineOptions::autoLayoutDesc                                                               |
//* |     28.0 | Added reconfigWorkgroupLayout to PipelineOptions and useSiScheduler to PipelineShaderOptions          |
//* |     27.0 | Remove the includeIrBinary option from PipelineOptions as only IR disassembly is now dumped           |
//* |     25.0 | Add includeIrBinary option into PipelineOptions for including IR binaries into ELF files.             |
//* |     24.0 | Add forceLoopUnrollCount option into PipelineShaderOptions.                                           |
//* |     23.0 | Add flag robustBufferAccess in PipelineOptions to check out of bounds of private array.               |
//* |     22.0 | Internal revision.                                                                                    |
//* |     21.0 | Add stage in Pipeline shader info and struct PipelineBuildInfo to simplify pipeline dump interface.   |
//**/

namespace Llpc
{

static const uint32_t  Version = LLPC_INTERFACE_MAJOR_VERSION;
static const uint32_t  MaxColorTargets = 8;
static const uint32_t  MaxViewports = 16;
static const char      VkIcdName[]     = "amdvlk";
static const uint32_t  InternalDescriptorSetId = static_cast<uint32_t>(-1);

// Forward declarations
class IShaderCache;

/// Enumerates result codes of LLPC operations.
enum class Result : int32_t
{
    /// The operation completed successfully
    Success                         = 0x00000000,
    // The requested operation is delayed
    Delayed                         = 0x00000001,
    // The requested feature is unsupported
    Unsupported                     = 0x00000002,
    /// The requested operation is unavailable at this time
    ErrorUnavailable                = -(0x00000001),
    /// The operation could not complete due to insufficient system memory
    ErrorOutOfMemory                = -(0x00000002),
    /// An invalid shader code was passed to the call
    ErrorInvalidShader               = -(0x00000003),
    /// An invalid value was passed to the call
    ErrorInvalidValue               = -(0x00000004),
    /// A required input pointer passed to the call was invalid (probably null)
    ErrorInvalidPointer             = -(0x00000005),
    /// The operaton encountered an unknown error
    ErrorUnknown                    = -(0x00000006),
};

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
    Count,                          ///< Count of resource mapping node types.
};

#if LLPC_BUILD_GFX10
/// Enumerates various sizing options of sub-group size for NGG primitive shader.
enum class NggSubgroupSizingType : uint32_t
{
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 26
    Auto,                           ///< Sub-group size is allocated as optimally determined
#endif
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
#endif

/// Represents graphics IP version info. See https://llvm.org/docs/AMDGPUUsage.html#processors for more
/// details.
struct GfxIpVersion
{
    uint32_t        major;              ///< Major version
    uint32_t        minor;              ///< Minor version
    uint32_t        stepping;           ///< Stepping info
};

/// Represents shader binary data.
struct BinaryData
{
    size_t          codeSize;           ///< Size of shader binary data
    const void*     pCode;              ///< Shader binary data
};

/// Represents per pipeline options.
struct PipelineOptions
{
    bool includeDisassembly;       ///< If set, the disassembly for all compiled shaders will be included in
                                   ///  the pipeline ELF.
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 30
    bool autoLayoutDesc;           ///< If set, the LLPC standalone compiler is compiling individual shader(s)
                                   ///  without pipeline info, so LLPC needs to do auto descriptor layout.
#endif
    bool scalarBlockLayout;        ///< If set, allows scalar block layout of types.
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
    bool reconfigWorkgroupLayout;  ///< If set, allows automatic workgroup reconfigure to take place on compute shaders.
#endif
    bool includeIr;                ///< If set, the IR for all compiled shaders will be included in the pipeline ELF.
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 23
    bool robustBufferAccess;       ///< If set, out of bounds accesses to buffer or private array will be handled.
                                   ///  for now this option is used by LLPC shader and affects only the private array,
                                   ///  the out of bounds accesses will be skipped with this setting.
#endif
#if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 25) && (LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 27)
    bool includeIrBinary;          ///< If set, the IR binary for all compiled shaders will be included in the pipeline
                                   ///  ELF.
#endif
};

/// Prototype of allocator for output data buffer, used in shader-specific operations.
typedef void* (VKAPI_CALL *OutputAllocFunc)(void* pInstance, void* pUserData, size_t size);

/// Represents per shader module options.
struct ShaderModuleOptions
{
    PipelineOptions     pipelineOptions;   ///< Pipeline options related with this shader module
    bool                enableOpt;         ///< Enable translate & lower phase in build shader module
};

/// Represents info to build a shader module.
struct ShaderModuleBuildInfo
{
    void*                pInstance;         ///< Vulkan instance object
    void*                pUserData;         ///< User data
    OutputAllocFunc      pfnOutputAlloc;    ///< Output buffer allocator
    BinaryData           shaderBin;         ///< Shader binary data (SPIR-V binary)
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 32
    ShaderModuleOptions  options;           ///< Per shader module options
#endif
};

/// Represents the header part of LLPC shader module data
struct ShaderModuleDataHeader
{
    uint32_t hash[4];       // Shader hash code
};

/// Represents output of building a shader module.
struct ShaderModuleBuildOut
{
    void*                pModuleData;       ///< Output shader module data (opaque)
};

/// Represents the options for pipeline dump.
struct PipelineDumpOptions
{
    const char* pDumpDir;                  ///< Pipeline dump directory
    uint32_t    filterPipelineDumpByType;  ///< Filter which types of pipeline dump are enabled
    uint64_t    filterPipelineDumpByHash;  ///< Only dump the pipeline with this compiler hash if non-zero
    bool        dumpDuplicatePipelines;    ///< If TRUE, duplicate pipelines will be dumped to a file with a
                                           ///  numeric suffix attached
};

/// Represents per shader stage options.
struct PipelineShaderOptions
{
    bool   trapPresent;  ///< Indicates a trap handler will be present when this pipeline is executed,
                         ///  and any trap conditions encountered in this shader should call the trap
                         ///  handler. This could include an arithmetic exception, an explicit trap
                         ///  request from the host, or a trap after every instruction when in debug
                         ///  mode.
    bool   debugMode;    ///< When set, this shader should cause the trap handler to be executed after
                         ///  every instruction.  Only valid if trapPresent is set.
    bool   enablePerformanceData; ///< Enables the compiler to generate extra instructions to gather
                                  ///  various performance-related data.
    bool   allowReZ;     ///< Allow the DB ReZ feature to be enabled.  This will cause an early-Z test
                         ///  to potentially kill PS waves before launch, and also issues a late-Z test
                         ///  in case the PS kills pixels.  Only valid for pixel shaders.
    /// Maximum VGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
    /// of this value and whatever the target GPU supports. To effectively disable this limit, set this to UINT_MAX.
    uint32_t  vgprLimit;

    /// Maximum SGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
    /// of this value and whatever the target GPU supports. To effectively disable this limit, set this to UINT_MAX.
    uint32_t  sgprLimit;

    /// Overrides the number of CS thread-groups which the GPU will launch per compute-unit. This throttles the
    /// shader, which can sometimes enable more graphics shader work to complete in parallel. A value of zero
    /// disables limiting the number of thread-groups to launch. This field is ignored for graphics shaders.
    uint32_t  maxThreadGroupsPerComputeUnit;

#if LLPC_BUILD_GFX10
    uint32_t      waveSize;      ///< Control the number of threads per wavefront (GFX10+)
    bool          wgpMode;       ///< Whether to choose WGP mode or CU mode (GFX10+)
    WaveBreakSize waveBreakSize; ///< Size of region to force the end of a wavefront (GFX10+).
                                 ///  Only valid for fragment shaders.
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 24
    /// Force loop unroll count. "0" means using default value; "1" means disabling loop unroll.
    uint32_t  forceLoopUnrollCount;
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
    /// Enable LLPC load scalarizer optimization.
    bool enableLoadScalarizer;
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 31
    bool allowVaryWaveSize;      ///< If set, lets the pipeline vary the wave sizes.
#elif VKI_EXT_SUBGROUP_SIZE_CONTROL
    bool allowVaryWaveSize;      ///< If set, lets the pipeline vary the wave sizes.
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
    /// Use the LLVM backend's SI scheduler instead of the default scheduler.
    bool      useSiScheduler;
#endif
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

/// Represents info of a shader attached to a to-be-built pipeline.
struct PipelineShaderInfo
{
    const void*                     pModuleData;            ///< Shader module data used for pipeline building (opaque)
    const VkSpecializationInfo*     pSpecializationInfo;    ///< Specialization constant info
    const char*                     pEntryTarget;           ///< Name of the target entry point (for multi-entry)
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 21
    ShaderStage                     entryStage;             ///< Shader stage of the target entry point
#endif
    uint32_t                        descriptorRangeValueCount; ///< Count of static descriptors
    DescriptorRangeValue*           pDescriptorRangeValues;    ///< An array of static descriptors

    uint32_t                        userDataNodeCount;      ///< Count of user data nodes

    /// User data nodes, providing the root-level mapping of descriptors in user-data entries (physical registers or
    /// GPU memory) to resources referenced in this pipeline shader.
    /// NOTE: Normally, this user data will correspond to the GPU's user data registers. However, Compiler needs some
    /// user data registers for internal use, so some user data may spill to internal GPU memory managed by Compiler.
    const ResourceMappingNode*      pUserDataNodes;
    PipelineShaderOptions           options;               ///< Per shader stage tuning/debugging options
};

/// Represents output of building a graphics pipeline.
struct GraphicsPipelineBuildOut
{
    BinaryData          pipelineBin;        ///< Output pipeline binary data
};

#if LLPC_BUILD_GFX10
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
#endif

/// Represents info to build a graphics pipeline.
struct GraphicsPipelineBuildInfo
{
    void*               pInstance;          ///< Vulkan instance object
    void*               pUserData;          ///< User data
    OutputAllocFunc     pfnOutputAlloc;     ///< Output buffer allocator
    IShaderCache*       pShaderCache;       ///< Shader cache, used to search for the compiled shader data
    PipelineShaderInfo  vs;                 ///< Vertex shader
    PipelineShaderInfo  tcs;                ///< Tessellation control shader
    PipelineShaderInfo  tes;                ///< Tessellation evaluation shader
    PipelineShaderInfo  gs;                 ///< Geometry shader
    PipelineShaderInfo  fs;                 ///< Fragment shader

    /// Create info of vertex input state
    const VkPipelineVertexInputStateCreateInfo*     pVertexInput;

    struct
    {
        VkPrimitiveTopology  topology;           ///< Primitive topology
        uint32_t             patchControlPoints; ///< Number of control points per patch (valid when the topology is
                                                 ///  "patch")
        uint32_t             deviceIndex;        ///< Device index for device group
        bool                 disableVertexReuse; ///< Disable reusing vertex shader output for indexed draws
        bool                 switchWinding ;     ///< Whether to reverse vertex ordering for tessellation
        bool                 enableMultiView;    ///< Whether to enable multi-view support
    } iaState;                                   ///< Input-assembly state

    struct
    {
        bool        depthClipEnable;            ///< Enable clipping based on Z coordinate
    } vpState;                                  ///< Viewport state

    struct
    {
        bool    rasterizerDiscardEnable;        ///< Kill all rasterized pixels. This is implicitly true if stream out
                                                ///  is enabled and no streams are rasterized
        bool    innerCoverage;                  ///< Related to conservative rasterization.  Must be false if
                                                ///  conservative rasterization is disabled.
        bool    perSampleShading;               ///< Enable per sample shading
        uint32_t  numSamples;                   ///< Number of coverage samples used when rendering with this pipeline
        uint32_t  samplePatternIdx;             ///< Index into the currently bound MSAA sample pattern table that
                                                ///  matches the sample pattern used by the rasterizer when rendering
                                                ///  with this pipeline.
        uint8_t   usrClipPlaneMask;             ///< Mask to indicate the enabled user defined clip planes
        VkPolygonMode       polygonMode;        ///< Triangle rendering mode
        VkCullModeFlags     cullMode;           ///< Fragment culling mode
        VkFrontFace         frontFace;          ///< Front-facing triangle orientation
        bool                depthBiasEnable;    ///< Whether to bias fragment depth values
    } rsState;                                  ///< Rasterizer State

    struct
    {
        bool    alphaToCoverageEnable;          ///< Enable alpha to coverage
        bool    dualSourceBlendEnable;          ///< Blend state bound at draw time will use a dual source blend mode
        struct
        {
            bool          blendEnable;          ///< Blend will be enabled for this target at draw time
            bool          blendSrcAlphaToColor; ///< Whether source alpha is blended to color channels for this target
                                                ///  at draw time
           uint8_t        channelWriteMask;     ///< Write mask to specify destination channels
           VkFormat       format;               ///< Color attachment format
        } target[MaxColorTargets];              ///< Per-MRT color target info
    } cbState;                                  ///< Color target state

#if LLPC_BUILD_GFX10
    NggState            nggState;           ///< NGG state used for tuning and debugging
#endif

    PipelineOptions     options;            ///< Per pipeline tuning/debugging options
};

/// Represents info to build a compute pipeline.
struct ComputePipelineBuildInfo
{
    void*               pInstance;          ///< Vulkan instance object
    void*               pUserData;          ///< User data
    OutputAllocFunc     pfnOutputAlloc;     ///< Output buffer allocator
    IShaderCache*       pShaderCache;       ///< Shader cache, used to search for the compiled shader data
    uint32_t            deviceIndex;        ///< Device index for device group
    PipelineShaderInfo  cs;                 ///< Compute shader
    PipelineOptions     options;            ///< Per pipeline tuning options
};

/// Represents output of building a compute pipeline.
struct ComputePipelineBuildOut
{
    BinaryData          pipelineBin;        ///< Output pipeline binary data
};

// =====================================================================================================================
/// Represents the unified of a pipeline create info.
struct PipelineBuildInfo
{
    const ComputePipelineBuildInfo*    pComputeInfo;     // Compute pipeline create info
    const GraphicsPipelineBuildInfo*   pGraphicsInfo;    // Graphic pipeline create info
};

typedef uint64_t ShaderHash;

/// Defines callback function used to lookup shader cache info in an external cache
typedef Result (*ShaderCacheGetValue)(const void* pClientData, ShaderHash hash, void* pValue, size_t* pValueLen);

/// Defines callback function used to store shader cache info in an external cache
typedef Result (*ShaderCacheStoreValue)(const void* pClientData, ShaderHash hash, const void* pValue, size_t valueLen);

/// Specifies all information necessary to create a shader cache object.
struct ShaderCacheCreateInfo
{
    const void*  pInitialData;      ///< Pointer to a data buffer whose contents should be used to seed the shader
                                    ///  cache. This may be null if no initial data is present.
    size_t       initialDataSize;   ///< Size of the initial data buffer, in bytes.

    // NOTE: The following parameters are all optional, and are only used when the IShaderCache will be used in
    // tandem with an external cache which serves as a backing store for the cached shader data.

    // [optional] Private client-opaque data which will be passed to the pClientData parameters of the Get and
    // Store callback functions.
    const void*            pClientData;
    ShaderCacheGetValue    pfnGetValueFunc;    ///< [Optional] Function to lookup shader cache data in an external cache
    ShaderCacheStoreValue  pfnStoreValueFunc;  ///< [Optional] Function to store shader cache data in an external cache
};

// =====================================================================================================================
/// Represents the interface of a cache for compiled shaders. The shader cache is designed to be optionally passed in at
/// pipeline create time. The compiled binary for the shaders is stored in the cache object to avoid compiling the same
/// shader multiple times. The shader cache also provides a method to serialize its data to be stored to disk.
class IShaderCache
{
public:
    /// Serializes the shader cache data or queries the size required for serialization.
    ///
    /// @param [in]      pBlob  System memory pointer where the serialized data should be placed. This parameter can
    ///                         be null when querying the size of the serialized data. When non-null (and the size is
    ///                         correct/sufficient) then the contents of the shader cache will be placed in this
    ///                         location. The data is an opaque blob which is not intended to be parsed by clients.
    /// @param [in,out]  pSize  Size of the memory pointed to by pBlob. If the value stored in pSize is zero then no
    ///                         data will be copied and instead the size required for serialization will be returned
    ///                         in pSize.
    ///
    /// @returns Success if data was serialized successfully, Unknown if fail to do serialize.
    virtual Result Serialize(
        void*   pBlob,
        size_t* pSize) = 0;

    /// Merges the provided source shader caches' content into this shader cache.
    ///
    /// @param [in]  srcCacheCount  Count of source shader caches to be merged.
    /// @param [in]  ppSrcCaches    Pointer to an array of pointers to shader cache objects.
    ///
    /// @returns Success if data of source shader caches was merged successfully, OutOfMemory if the internal allocator
    ///          memory cannot be allocated.
    virtual Result Merge(
        uint32_t             srcCacheCount,
        const IShaderCache** ppSrcCaches) = 0;

    /// Frees all resources associated with this object.
    virtual void Destroy() = 0;

protected:
    /// @internal Constructor. Prevent use of new operator on this interface.
    IShaderCache() {}

    /// @internal Destructor. Prevent use of delete operator on this interface.
    virtual ~IShaderCache() {}
};

// =====================================================================================================================
/// Represents the interfaces of a pipeline dumper.
class IPipelineDumper
{
public:
    /// Dumps SPIR-V shader binary to extenal file.
    ///
    /// @param [in]  pDumpDir     Directory of pipeline dump
    /// @param [in]  pSpirvBin    SPIR-V binary
    static void VKAPI_CALL DumpSpirvBinary(const char*                     pDumpDir,
                                           const BinaryData*               pSpirvBin);

    /// Begins to dump graphics/compute pipeline info.
    ///
    /// @param [in]  pDumpDir                 Directory of pipeline dump
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 21
    /// @param [in]  pipelineInfo             Info of the pipeline to be built
#else
    /// @param [in]  pComputePipelineInfo     Info of the compute pipeline to be built
    /// @param [in]  pGraphicsPipelineInfo    Info of the graphics pipeline to be built
#endif
    ///
    /// @returns The handle of pipeline dump file
    static void* VKAPI_CALL BeginPipelineDump(const PipelineDumpOptions*       pDumpOptions,
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 21
                                              PipelineBuildInfo               pipelineInfo
#else
                                              const ComputePipelineBuildInfo*  pComputePipelineInfo,
                                              const GraphicsPipelineBuildInfo* pGraphicsPipelineInfo
#endif
        );

    /// Ends to dump graphics/compute pipeline info.
    ///
    /// @param  [in]  pDumpFile         The handle of pipeline dump file
    static void VKAPI_CALL EndPipelineDump(void* pDumpFile);

    /// Disassembles pipeline binary and dumps it to pipeline info file.
    ///
    /// @param [in]  pDumpFile        The handle of pipeline dump file
    /// @param [in]  gfxIp            Graphics IP version info
    /// @param [in]  pPipelineBin     Pipeline binary (ELF)
    static void VKAPI_CALL DumpPipelineBinary(void*                    pDumpFile,
                                              GfxIpVersion             gfxIp,
                                              const BinaryData*        pPipelineBin);

    /// Dump extra info to pipeline file.
    ///
    /// @param [in]  pDumpFile        The handle of pipeline dump file
    /// @param [in]  pStr             Extra string info to dump
    static void VKAPI_CALL DumpPipelineExtraInfo(void*                  pDumpFile,
                                                 const char*            pStr);

    /// Gets shader module hash code.
    ///
    /// @param [in]  pModuleData   Pointer to the shader module data.
    ///
    /// @returns Hash code associated this shader module.
    static uint64_t VKAPI_CALL GetShaderHash(const void* pModuleData);

    /// Calculates graphics pipeline hash code.
    ///
    /// @param [in]  pPipelineInfo  Info to build this graphics pipeline
    ///
    /// @returns Hash code associated this graphics pipeline.
    static uint64_t VKAPI_CALL GetPipelineHash(const GraphicsPipelineBuildInfo* pPipelineInfo);

    /// Calculates compute pipeline hash code.
    ///
    /// @param [in]  pPipelineInfo  Info to build this compute pipeline
    ///
    /// @returns Hash code associated this compute pipeline.
    static uint64_t VKAPI_CALL GetPipelineHash(const ComputePipelineBuildInfo* pPipelineInfo);

    /// Gets graphics pipeline name.
    ///
    /// @param [in]  pPipelineInfo  Info to build this graphics pipeline
    /// @param [out] pPipeName      The full name of this graphics pipeline
    /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
    static void VKAPI_CALL GetPipelineName(const GraphicsPipelineBuildInfo* pPipelineInfo,
                                           char* pPipeName,
                                           const size_t nameBufSize);

    /// Gets compute pipeline name.
    ///
    /// @param [in]  pPipelineInfo  Info to build this compute pipeline
    /// @param [out] pPipeName      The full name of this compute pipeline
    /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
    static void VKAPI_CALL GetPipelineName(const ComputePipelineBuildInfo* pPipelineInfo,
                                           char* pPipeName,
                                           const size_t nameBufSize);

};

// =====================================================================================================================
/// Represents the interfaces of a pipeline compiler.
class ICompiler
{
public:
    /// Creates pipeline compiler from the specified info.
    ///
    /// @param [in]  optionCount    Count of compilation-option strings
    /// @param [in]  options        An array of compilation-option strings
    /// @param [out] ppCompiler     Pointer to the created pipeline compiler object
    ///
    /// @returns Result::Success if successful. Other return codes indicate failure.
    static Result VKAPI_CALL Create(GfxIpVersion      gfxIp,
                                    uint32_t          optionCount,
                                    const char*const* options,
                                    ICompiler**       ppCompiler);

    /// Checks whether a vertex attribute format is supported by fetch shader.
    ///
    /// @parame [in] format  Vertex attribute format
    ///
    /// @return TRUE if the specified format is supported by fetch shader. Otherwise, FALSE is returned.
    static bool VKAPI_CALL IsVertexFormatSupported(VkFormat format);

    /// Destroys the pipeline compiler.
    virtual void VKAPI_CALL Destroy() = 0;

    /// Build shader module from the specified info.
    ///
    /// @param [in]  pShaderInfo    Info to build this shader module
    /// @param [out] pShaderOut     Output of building this shader module
    ///
    /// @returns Result::Success if successful. Other return codes indicate failure.
    virtual Result BuildShaderModule(const ShaderModuleBuildInfo* pShaderInfo,
                                    ShaderModuleBuildOut*        pShaderOut) const = 0;

    /// Build graphics pipeline from the specified info.
    ///
    /// @param [in]  pPipelineInfo  Info to build this graphics pipeline
    /// @param [out] pPipelineOut   Output of building this graphics pipeline
    ///
    /// @returns Result::Success if successful. Other return codes indicate failure.
    virtual Result BuildGraphicsPipeline(const GraphicsPipelineBuildInfo* pPipelineInfo,
                                         GraphicsPipelineBuildOut*        pPipelineOut,
                                         void*                            pPipelineDumpFile = nullptr) = 0;

    /// Build compute pipeline from the specified info.
    ///
    /// @param [in]  pPipelineInfo  Info to build this compute pipeline
    /// @param [out] pPipelineOut   Output of building this compute pipeline
    ///
    /// @returns Result::Success if successful. Other return codes indicate failure.
    virtual Result BuildComputePipeline(const ComputePipelineBuildInfo* pPipelineInfo,
                                        ComputePipelineBuildOut*        pPipelineOut,
                                        void*                           pPipelineDumpFile = nullptr) = 0;

    /// Creates a shader cache object with the requested properties.
    ///
    /// @param [in]  pCreateInfo    Create info of the shader cache.
    /// @param [out] ppShaderCache  Constructed shader cache object.
    ///
    /// @returns Success if the shader cache was successfully created. Otherwise, ErrorOutOfMemory is returned.
    virtual Result CreateShaderCache(
        const ShaderCacheCreateInfo* pCreateInfo,
        IShaderCache**               ppShaderCache) = 0;

protected:
    ICompiler() {}
    /// Destructor
    virtual ~ICompiler() {}
};

} // Llpc
