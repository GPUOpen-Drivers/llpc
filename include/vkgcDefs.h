/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  vkgcDefs.h
 * @brief VKGC header file: contains vulkan graphics compiler basic definitions (including interfaces and data types).
 ***********************************************************************************************************************
 */
#pragma once

#include "vkgcBase.h"
#include "vulkan.h"
#include <cassert>
#include <optional>
#include <tuple>

// Confliction of Xlib and LLVM headers
#if !_WIN32
#undef True
#undef False
#undef DestroyAll
#undef Status
#undef Bool
#endif

/// LLPC major interface version.
#define LLPC_INTERFACE_MAJOR_VERSION 66

/// LLPC minor interface version.
#define LLPC_INTERFACE_MINOR_VERSION 1

#ifndef LLPC_CLIENT_INTERFACE_MAJOR_VERSION
#error LLPC client version is not defined
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 60
#error LLPC client version is too old
#endif

/// LLPC_NODISCARD - Warns when function return value is discarded.
//
// We cannot use the 'nodiscard' attribute until we upgrade to C++17 or newer mode.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::warn_unused_result)
#define LLPC_NODISCARD [[clang::warn_unused_result]]
#elif defined(__GNUC__) && __has_cpp_attribute(nodiscard)
#define LLPC_NODISCARD [[nodiscard]]
#else
#define LLPC_NODISCARD
#endif
#else
#define LLPC_NODISCARD
#endif

//
// -------------------------------------------------------------------------------------------------------------------
//  @page VersionHistory
//  %Version History
//  | %Version | Change Description                                                                                    |
//  | -------- | ----------------------------------------------------------------------------------------------------- |
//  |     66.1 | Add forwardPropagateNoContract and backwardPropagateNoContract to PipelineShaderOptions               |
//  |     66.0 | Rename noContract in PipelineShaderOptions to noContractOpDot                                         |
//  |     65.1 | Add disableSampleMask to PipelineOptions                                                              |
//  |     65.0 | Remove updateDescInElf                                                                                |
//  |     64.2 | Add dynamicSampleInfo to GraphicsPipelineBuildInfo::rsState                                           |
//  |     64.1 | Add disableTruncCoordForGather to PipelineOptions.                                                    |
//  |     64.0 | Add enableColorExportShader to GraphicsPipelineBuildInfo.                                             |
//  |     63.3 | Add TesssellationLevel to iaState                                                                     |
//  |     63.2 | Add vertex64BitsAttribSingleLoc to PipelineOptions                                                    |
//  |     63.1 | Add forceDisableStreamOut and forceEnablePrimStats to ApiXfbOutData                                   |
//  |     63.0 | Add Atomic Counter, its default descriptor and map its concertType to Buffer.                         |
//  |     62.1 | Add ApiXfbOutData GraphicsPipelineBuildInfo                                                           |
//  |     62.0 | Default to the compiler getting the GPURT library directly, and move shader library info into RtState |
//  |     61.16| Add replaceSetWithResourceType to PipelineOptions                                                     |
//  |     61.15| Add disableReadFirstLaneWorkaround to PipelineShaderOptions                                           |
//  |     61.14| Add rasterStream to rsState                                                                           |
//  |     61.13| Add dualSourceBlendDynamic to cbState                                                                 |
//  |     61.12| Add mode to RayTracingPipelineBuildInfo                                                               |
//  |     61.11| Add UniformConstantMap and related structures                                                         |
//  |     61.10| Add useShadingRate and useSampleInfoto ShaderModuleUsage                                              |
//  |     61.8 | Add enableImplicitInvariantExports to PipelineOptions                                                 |
//  |     61.7 | Add disableFMA to PipelineShaderOptions                                                               |
//  |     61.6 | Add workaroundInitializeOutputsToZero to PipelineShaderOptions                                        |
//  |     61.5 | Add RtIpVersion (including its checkers) to represent RT IP                                           |
//  |     61.4 | Add workaroundStorageImageFormats to PipelineShaderOptions                                            |
//  |     61.2 | Add pClientMetadata and clientMetadataSize to all PipelineBuildInfos                                  |
//  |     61.1 | Add IPipelineDumper::GetGraphicsShaderBinaryHash                                                      |
//  |     61.0 | Add DescriptorMutable type and ResourceMappingNode::strideInDwords to support mutable descriptors     |
//  |     60.0 | Simplify the enum NggCompactMode to a boolean flag                                                    |
//  |     59.0 | Remove the option enableVertexReuse from NggState                                                     |
//  |     57.2 | Move all internal resource binding id to enum InternalBinding.                                        |
//  |     57.1 | Add forceNonUniformResourceIndexStageMask to PipelineOptions                                          |
//  |     57.0 | Merge aggressiveInvariantLoads and disableInvariantLoads to an enumerated option                      |
//  |     56.2 | Add aggressiveInvariantLoads and disableInvariantLoads to PipelineShaderOptions                       |
//  |     56.1 | Add struct UberFetchShaderAttribInfo                                                                  |
//  |     56.0 | Move maxRayLength to RtState                                                                          |
//  |     55.2 | Add pipeline layout API hash to all PipelineBuildInfos                                                |
//  |     55.1 | Add nsaThreshold to PipelineShaderOptions                                                             |
//  |     55.0 | Remove isInternalRtShader from module options                                                         |
//  |     54.9 | Add internalRtShaders to PipelineOptions to allow for dumping this data                               |
//  |     54.6 | Add reverseThreadGroup to PipelineOptions                                                             |
//  |     54.5 | Add forceLateZ to PipelineShaderOptions                                                               |
//  |     54.4 | Add isReplay to RayTracingPipelineBuildInfo for ray tracing capture replay feature                    |
//  |     54.3 | Add usePointSize to ShaderModuleUsage                                                                 |
//  |     54.2 | Add subgroupSize to PipelineShaderOptions                                                             |
//  |     54.1 | Add overrideForceThreadIdSwizzling overrideShaderThreadGroupSizeX, overrideShaderThreadGroupSizeY     |
//  |          | and overrideShaderThreadGroupSizeZ  to PipelineShaderOptions                                          |
//  |     54.0 | Add overrideThreadGroupSizeX, overrideThreadGroupSizeY and overrideThreadGroupSizeZ to PipelineOptions|
//  |     53.7 | Add threadGroupSwizzleMode to PipelineOptions                                                         |
//  |     53.6 | Add scalarizeWaterfallLoads to PipelineShaderOptions                                                  |
//  |     53.5 | Add forceCsThreadIdSwizzling for thread id swizzle in 8*4                                             |
//  |     53.4 | Add ldsSpillLimitDwords shader option                                                                 |
//  |     53.3 | Add disableFastMathFlags shader option, plus support for this and fastMathFlags in pipeline files     |
//  |     53.2 | Add resourceLayoutScheme to PipelineOptions                                                           |
//  |     53.1 | Add PartPipelineStage enum for part-pipeline mode                                                     |
//  |     53.0 | Add optimizationLevel to PipelineOptions                                                              |
//  |     52.3 | Add fastMathFlags to PipelineShaderOptions                                                            |
//  |     52.2 | Add provokingVertexMode to rsState                                                                    |
//  |     52.1 | Add pageMigrationEnabled to PipelineOptions                                                           |
//  |     52.0 | Add the member word4 and word5 to SamplerYCbCrConversionMetaData                                      |
//  |     51.2 | Added new pipeline shader info to support mesh shader                                                 |
//  |     51.0 | Added new shader stage enumerants to support mesh shader                                              |
//  |     50.2 | Add the member dsState to GraphicsPipelineBuildInfo                                                   |
//  |     50.1 | Disclose ResourceMappingNodeType::InlineBuffer                                                        |
//  |     50.0 | Removed the member 'enableOpt' of ShaderModuleOptions                                                 |
//  |     49.1 | Added enableEarlyCompile to GraphicsPipelineBuildInfo                                                 |
//  |     49.0 | Added DescriptorConstBuffer, DescriptorConstBufferCompact, DescriptorImage, DescriptorConstTexelBuffer|
//  |          | to ResourceMappingNodeType                                                                            |
//  |     48.1 | Added enableUberFetchShader to GraphicsPipelineBuildInfo                                              |
//  |     48.0 | Removed the member 'polygonMode' of rsState                                                           |
//  |     47.0 | Always get culling controls from primitive shader table                                               |
//  |     46.3 | Added enableInterpModePatch to PipelineOptions                                                        |
//  |     46.2 | Added optimizeTessFactor to PipelineOptions                                                           |
//  |     46.1 | Added dynamicVertexStride to GraphicsPipelineBuildInfo                                                |
//  |     46.0 | Removed the member 'depthBiasEnable' of rsState                                                       |
//  |     45.5 | Added new enum type ThreadGroupSwizzleMode for thread group swizzling for compute shaders             |
//  |     45.4 | Added disableLicmThreshold, unrollHintThreshold, and dontUnrollHintThreshold to PipelineShaderOptions |
//  |     45.3 | Add pipelinedump function to enable BeginPipelineDump and GetPipelineName                             |
//  |     45.2 | Add GFX IP plus checker to GfxIpVersion                                                               |
//  |     45.1 | Add pipelineCacheAccess, stageCacheAccess(es) to GraphicsPipelineBuildOut/ComputePipelineBuildOut     |
//  |     45.0 | Remove the member 'enableFastLaunch' of NGG state                                                     |
//  |     44.0 | Rename the member 'forceNonPassthrough' of NGG state to 'forceCullingMode'                            |
//  |     43.1 | Add disableImageResourceCheck in PipelineOptions                                                      |
//  |     43.0 | Removed the enumerant WaveBreakSize::DrawTime                                                         |
//  |     42.0 | Removed tileOptimal flag from SamplerYcbcrConversion metadata struct                                  |
//  |     41.0 | Moved resource mapping from ShaderPipeline-level to Pipeline-level                                    |
//  |     40.4 | Added fp32DenormalMode in PipelineShaderOptions to allow overriding SPIR-V denormal settings          |
//  |     40.3 | Added ICache interface                                                                                |
//  |     40.2 | Added extendedRobustness in PipelineOptions to support VK_EXT_robustness2                             |
//  |     40.1 | Added disableLoopUnroll to PipelineShaderOptions                                                      |
//  |     40.0 | Added DescriptorReserved12, which moves DescriptorYCbCrSampler down to 13                             |
//  |     39.0 | Non-LLPC-specific XGL code should #include vkcgDefs.h instead of llpc.h                               |
//  |     38.3 | Added shadowDescriptorTableUsage and shadowDescriptorTablePtrHigh to PipelineOptions                  |
//  |     38.2 | Added scalarThreshold to PipelineShaderOptions                                                        |
//  |     38.1 | Added unrollThreshold to PipelineShaderOptions                                                        |
//  |     38.0 | Removed CreateShaderCache in ICompiler and pShaderCache in pipeline build info                        |
//  |     37.0 | Removed the -enable-dynamic-loop-unroll option                                                        |
//  |     36.0 | Add 128 bit hash as clientHash in PipelineShaderOptions                                               |
//  |     35.0 | Added disableLicm to PipelineShaderOptions                                                            |
//  |     33.0 | Add enableLoadScalarizer option into PipelineShaderOptions.                                           |
//  |     32.0 | Add ShaderModuleOptions in ShaderModuleBuildInfo                                                      |
//  |     31.0 | Add PipelineShaderOptions::allowVaryWaveSize                                                          |
//  |     30.0 | Removed PipelineOptions::autoLayoutDesc                                                               |
//  |     28.0 | Added reconfigWorkgroupLayout to PipelineOptions and useSiScheduler to PipelineShaderOptions          |
//  |     27.0 | Remove the includeIrBinary option from PipelineOptions as only IR disassembly is now dumped           |
//  |     25.0 | Add includeIrBinary option into PipelineOptions for including IR binaries into ELF files.             |
//  |     24.0 | Add forceLoopUnrollCount option into PipelineShaderOptions.                                           |
//  |     23.0 | Add flag robustBufferAccess in PipelineOptions to check out of bounds of private array.               |
//  |     22.0 | Internal revision.                                                                                    |
//  |     21.0 | Add stage in Pipeline shader info and struct PipelineBuildInfo to simplify pipeline dump interface.   |
//
//  IMPORTANT NOTE: All structures defined in this file that are passed as input into LLPC must be zero-initialized
//  with code such as the following before filling in the structure's fields:
//
//    SomeLlpcStructure someLlpcStructure = {};
//
//  It is sufficient to perform this initialization on a containing structure.
//
//  LLPC is free to add new fields to such structures without increasing the client interface major version, as long
//  as setting the newly added fields to a 0 (or false) value is safe, i.e. it preserves the old behavior.
//

namespace Vkgc {

static const unsigned Version = LLPC_INTERFACE_MAJOR_VERSION;
static const unsigned InternalDescriptorSetId = static_cast<unsigned>(-1);
static const unsigned MaxVertexAttribs = 64;
static const unsigned MaxColorTargets = 8;
static const unsigned MaxFetchShaderInternalBufferSize = 16 * MaxVertexAttribs;

namespace GlCompatibilityLimits {
static const unsigned MaxUniformLocations = 8000;
static const unsigned MaxClipPlanes = 8;
static const unsigned MaxGenericVertexAttribs = 32;
static const unsigned MaxTextureCoords = 8;
} // namespace GlCompatibilityLimits

// Forward declarations
class IShaderCache;
class ICache;
class EntryHandle;

// Hide operator bool to safe-guard against accidents.
struct optional_bool : private std::optional<bool> {
  optional_bool() = default;
  optional_bool(const optional_bool &rhs) = default;
  optional_bool &operator=(const optional_bool &rhs) = default;
  optional_bool &operator=(bool rhs) {
    std::optional<bool>::operator=(rhs);
    return *this;
  }
  using std::optional<bool>::has_value;
  using std::optional<bool>::value;
  using std::optional<bool>::value_or;
};

/// Enumerates result codes of LLPC operations.
enum class Result : int {
  /// The operation completed successfully
  Success = 0x00000000,
  // The requested operation is delayed
  Delayed = 0x00000001,
  // The requested feature is unsupported
  Unsupported = 0x00000002,
  // A required resource (e.g. cache entry) is not ready yet.
  NotReady = 0x00000003,
  // A required resource (e.g. cache entry) was not found.
  NotFound = 0x00000004,
  /// The requested operation is unavailable at this time
  ErrorUnavailable = -(0x00000001),
  /// The operation could not complete due to insufficient system memory
  ErrorOutOfMemory = -(0x00000002),
  /// An invalid shader code was passed to the call
  ErrorInvalidShader = -(0x00000003),
  /// An invalid value was passed to the call
  ErrorInvalidValue = -(0x00000004),
  /// A required input pointer passed to the call was invalid (probably null)
  ErrorInvalidPointer = -(0x00000005),
  /// The operation encountered an unknown error
  ErrorUnknown = -(0x00000006),
};

/// Represents the base data type
enum class BasicType : unsigned {
  Unknown = 0, ///< Unknown
  Float,       ///< Float
  Double,      ///< Double
  Int,         ///< Signed integer
  Uint,        ///< Unsigned integer
  Int64,       ///< 64-bit signed integer
  Uint64,      ///< 64-bit unsigned integer
  Float16,     ///< 16-bit floating-point
  Int16,       ///< 16-bit signed integer
  Uint16,      ///< 16-bit unsigned integer
  Int8,        ///< 8-bit signed integer
  Uint8,       ///< 8-bit unsigned integer
};

/// Enumerates LLPC shader stages.
enum ShaderStage : unsigned {
  ShaderStageTask = 0,                                  ///< Task shader
  ShaderStageVertex,                                    ///< Vertex shader
  ShaderStageTessControl,                               ///< Tessellation control shader
  ShaderStageTessEval,                                  ///< Tessellation evaluation shader
  ShaderStageGeometry,                                  ///< Geometry shader
  ShaderStageMesh,                                      ///< Mesh shader
  ShaderStageFragment,                                  ///< Fragment shader
  ShaderStageCompute,                                   ///< Compute shader
  ShaderStageRayTracingRayGen,                          ///< Ray generation shader (for ray tracing)
  ShaderStageRayTracingIntersect,                       ///< Intersection shader (for ray tracing)
  ShaderStageRayTracingAnyHit,                          ///< Any-Hit shader (for ray tracing)
  ShaderStageRayTracingClosestHit,                      ///< Closest Hit shader (for ray tracing)
  ShaderStageRayTracingMiss,                            ///< Miss shader (for ray tracing)
  ShaderStageRayTracingCallable,                        ///< Callable shader (for ray tracing)
  ShaderStageCount,                                     ///< Count of shader stages
  ShaderStageInvalid = ~0u,                             ///< Invalid shader stage
  ShaderStageNativeStageCount = ShaderStageCompute + 1, ///< Native supported shader stage count
  ShaderStageGfxCount = ShaderStageFragment + 1,        ///< Count of shader stages for graphics pipeline

  ShaderStageCopyShader = ShaderStageCount, ///< Copy shader (internal-use)
  ShaderStageCountInternal,                 ///< Count of shader stages (internal-use)
};

/// Enumerating multiple shader stages when used in a mask.
enum ShaderStageBit : unsigned {
  ShaderStageTaskBit = (1 << ShaderStageTask),                                 ///< Task shader bit
  ShaderStageVertexBit = (1 << ShaderStageVertex),                             ///< Vertex shader bit
  ShaderStageTessControlBit = (1 << ShaderStageTessControl),                   ///< Tessellation control shader bit
  ShaderStageTessEvalBit = (1 << ShaderStageTessEval),                         ///< Tessellation evaluation shader bit
  ShaderStageGeometryBit = (1 << ShaderStageGeometry),                         ///< Geometry shader bit
  ShaderStageMeshBit = (1 << ShaderStageMesh),                                 ///< Mesh shader bit
  ShaderStageFragmentBit = (1 << ShaderStageFragment),                         ///< Fragment shader bit
  ShaderStageComputeBit = (1 << ShaderStageCompute),                           ///< Compute shader bit
  ShaderStageRayTracingRayGenBit = (1 << ShaderStageRayTracingRayGen),         ///< Ray generation shader bit
  ShaderStageRayTracingIntersectBit = (1 << ShaderStageRayTracingIntersect),   ///< Intersection shader bit
  ShaderStageRayTracingAnyHitBit = (1 << ShaderStageRayTracingAnyHit),         ///< Any-Hit shader bit
  ShaderStageRayTracingClosestHitBit = (1 << ShaderStageRayTracingClosestHit), ///< Closest Hit shader bit
  ShaderStageRayTracingMissBit = (1 << ShaderStageRayTracingMiss),             ///< Miss shader bit
  ShaderStageRayTracingCallableBit = (1 << ShaderStageRayTracingCallable),     ///< Callable shader bit
  ShaderStageAllMeshBit = ShaderStageTaskBit | ShaderStageMeshBit,             ///< All mesh bits
  ShaderStageAllGraphicsBit = ShaderStageVertexBit | ShaderStageTessControlBit | ShaderStageTessEvalBit |
                              ShaderStageGeometryBit | ShaderStageFragmentBit |
                              ShaderStageAllMeshBit, ///< All graphics bits
  ShaderStageAllRayTracingBit = ShaderStageRayTracingRayGenBit | ShaderStageRayTracingIntersectBit |
                                ShaderStageRayTracingAnyHitBit | ShaderStageRayTracingClosestHitBit |
                                ShaderStageRayTracingMissBit |
                                ShaderStageRayTracingCallableBit, ///< All ray tracing bits
};

/// Enumerates LLPC types of unlinked shader elf.
enum UnlinkedShaderStage : unsigned {
  UnlinkedStageVertexProcess,
  UnlinkedStageFragment,
  UnlinkedStageCompute,
  UnlinkedStageRayTracing,
  UnlinkedStageCount
};

static_assert((1 << (ShaderStageCount - 1)) == ShaderStageRayTracingCallableBit,
              "Vkgc::ShaderStage has been updated. Please update Vkgc::ShaderStageBit as well.");

/// Enumerates the binding ID of internal resource.
enum InternalBinding : unsigned {
  FetchShaderBinding = 0,                   ///< Binding ID of vertex buffer table
  ConstantBuffer0Binding = 1,               ///< Binding ID of default uniform block
  PushConstantBinding = 2,                  ///< Binding ID of push constant buffer
  ShaderRecordBufferBinding = 3,            ///< Binding ID of ray-tracing shader record buffer
  TaskPayloadBinding = 4,                   ///< Binding ID of payload buffer in task shader
  FetchShaderInternalBufferBinding = 5,     ///< Binding ID of uber-fetch shader internal buffer
  PrintfBufferBindingId = 6,                ///< Binding ID of internal buffer for debug printf
  ReverseThreadGroupControlBinding = 7,     ///< Binding ID of internal buffer for reverseThreadGroup
  RtCaptureReplayInternalBufferBinding = 8, ///< Binding ID of ray-tracing capture replay internal buffer
  SpecConstInternalBufferBindingId = 9,     ///< Binding ID of internal buffer for specialized constant.
  SpecConstInternalBufferBindingIdEnd = SpecConstInternalBufferBindingId + ShaderStageCount,
  CurrentAttributeBufferBinding = 24, ///< Binding ID of current attribute
};

/// Internal vertex attribute location start from 0.
enum GlCompatibilityAttributeLocation : unsigned {
  FirstAttributeLocation = 0,        ///< Internal vertex attribute start from 0.
  Generic0 = FirstAttributeLocation, ///< Internal vertex attribute of vertex.attrib[] in Arb shader.
  Pos = Generic0 + GlCompatibilityLimits::MaxGenericVertexAttribs,
  ///< Internal vertex attribute of vertex.position in Arb shader
  Weight,         ///< Internal vertex attribute of vertex.weight in Arb shader.
  Normal,         ///< Internal vertex attribute of gl_Normal.
  Color,          ///< Internal vertex attribute of gl_Color.
  SecondaryColor, ///< Internal vertex attribute of gl_SecondaryColor.
  FogCoord,       ///< Internal vertex attribute of gl_FogCoord.
  ColorIndex,     ///< Internal vertex attribute to pass on index of gl_MultiTexCoord.
  EdgeFlag,       ///< Internal vertex attribute use to pass on the edeg flag.
  Texcoord0,      ///< Internal vertex attribute of gl_MultiTexCoord0.
  BaseinstanceOffset = Texcoord0 + GlCompatibilityLimits::MaxTextureCoords,
  ///< Internal vertex attribute: BaseInstanceOffset,
  ///  used by MultiDrawElementsIndirectMerger.
  IndirectVertexId,      ///< Internal vertex attribute to load gl_VertexId from
                         ///  external buffer, used by MultiDrawElementsIndirectMerger.
  IndirectBaseInstance,  ///< Internal vertex attribute to load gl_BaseInstance from
                         ///  external buffer, used by MultiDrawElementsIndirectMerger.
  AttributeLocationCount ///< The count of Internal vertex attribute.
};

/// Internal inout location start from 128.
enum GlCompatibilityInOutLocation : unsigned {
  FirstInOutLocation = 128,        ///< InOut location start from 128.
  FrontColor = FirstInOutLocation, ///< InOut location of gl_FrontColor or gl_Color.
  BackColor,                       ///< InOut location of gl_BackColor.
  FrontSecondaryColor,             ///< InOut location of gl_FrontSecondaryColor or gl_SecondaryColor.
  BackSecondaryColor,              ///< InOut location of gl_BackSecondaryColor.
  FogFragCoord,                    ///< InOut location of gl_FogFragCoord.
  ClipVertex,                      ///< InOut location of gl_ClipVertex.
  TexCoord,                        ///< InOut location of gl_TexCoord.
  PatchTexCoord = TexCoord + GlCompatibilityLimits::MaxTextureCoords,
  ///< Internal pipeline patch texture coordinate,
  ///  It is used by glBitMap and glDrawPixels.
  SpecialFragOut = 0 ///< InOut location 0, use to specify the special output of fragment
                     ///  shader, eg. gl_FragColor and gl_FragData.
};

/// Internal uniform location start from 8000.
enum GlCompatibilityUniformLocation : unsigned {
  FirstUniformLocation = GlCompatibilityLimits::MaxUniformLocations,
  ///< Internal uniform location start from MaxUniformLocations.
  FrameBufferSize = FirstUniformLocation + 0, ///< Internal uniform location use to pass on the size of frame buffer.
  AlphaTestRef,                               ///< Internal uniform location use to pass on the alpha test ref.
  ClipPlane,                                  ///< Internal uniform location of gl_ClipPlane.
  UniformLocationCount = ClipPlane + GlCompatibilityLimits::MaxClipPlanes - GlCompatibilityLimits::MaxUniformLocations
};

/// Enumerates the function of a particular node in a shader's resource mapping graph.
enum class ResourceMappingNodeType : unsigned {
  Unknown,                   ///< Invalid type
  DescriptorResource,        ///< Generic descriptor: resource, including texture resource,
  DescriptorSampler,         ///< Generic descriptor: sampler
  DescriptorCombinedTexture, ///< Generic descriptor: combined texture, combining resource descriptor with
                             ///  sampler descriptor of the same texture, starting with resource descriptor
  DescriptorTexelBuffer,     ///< Generic descriptor: texel buffer
  DescriptorFmask,           ///< Generic descriptor: F-mask
  DescriptorBuffer,          ///< Generic descriptor: buffer, including shader storage buffer
  DescriptorTableVaPtr,      ///< Descriptor table VA pointer
  IndirectUserDataVaPtr,     ///< Indirect user data VA pointer
  PushConst,                 ///< Push constant
  DescriptorBufferCompact,   ///< Compact buffer descriptor, only contains the buffer address
  StreamOutTableVaPtr,       ///< Stream-out buffer table VA pointer
  DescriptorReserved12,
  DescriptorYCbCrSampler,       ///< Generic descriptor: YCbCr sampler
  DescriptorConstBuffer,        ///< Generic descriptor: constBuffer,including uniform buffer
  DescriptorConstBufferCompact, ///< Generic descriptor: constBuffer,including dynamic storage buffer
  DescriptorImage,              ///< Generic descriptor: storageImage, including image, input attachment
  DescriptorConstTexelBuffer,   ///< Generic descriptor: constTexelBuffer, including uniform texel buffer
  InlineBuffer,                 ///< Push constant with binding
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
  DescriptorMutable, ///< Mutable descriptor type
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 63
  DescriptorAtomicCounter, ///< Generic descriptor: atomic counter
#endif
  Count, ///< Count of resource mapping node types.
};

/// Enumerates part-pipeline stages of compilation.
enum PartPipelineStage : unsigned {
  PartPipelineStageFragment,         ///< Fragment stage
  PartPipelineStagePreRasterization, ///< Pre-rasterization stage
  PartPipelineStageCount             ///< Count of part-pipeline stages
};

/// Represents one node in a graph defining how the user data bound in a command buffer at draw/dispatch time maps to
/// resources referenced by a shader (t#, u#, etc.).
struct ResourceMappingNode {
  ResourceMappingNodeType type; ///< Type of this node

  unsigned sizeInDwords;   ///< Size of this node in dword
  unsigned offsetInDwords; ///< Offset of this node (from the beginning of the resource mapping table) in dword

  union {
    /// Info for generic descriptor nodes (DescriptorResource, DescriptorSampler, DescriptorCombinedTexture,
    /// DescriptorTexelBuffer, DescriptorBuffer and DescriptorBufferCompact)
    struct {
      unsigned set;     ///< Descriptor set
      unsigned binding; ///< Descriptor binding
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
      unsigned strideInDwords; ///< Stride of elements in a descriptor array (used for mutable descriptors)
                               ///  a stride of zero will use the type of the node to determine the stride
#endif
    } srdRange;
    /// Info for hierarchical nodes (DescriptorTableVaPtr)
    struct {
      unsigned nodeCount;               ///< Number of entries in the "pNext" array
      const ResourceMappingNode *pNext; ///< Array of node structures describing the next hierarchical
                                        ///  level of mapping
    } tablePtr;
    /// Info for hierarchical nodes (IndirectUserDataVaPtr)
    struct {
      unsigned sizeInDwords; ///< Size of the pointed table in dwords
    } userDataPtr;
  };
};

struct ResourceMappingRootNode {
  ResourceMappingNode node; ///< Common node contents (between root and sub nodes)
  unsigned visibility;      ///< Mask composed of ShaderStageBit values
};

/// Represents the info of static descriptor.
struct StaticDescriptorValue {
  ResourceMappingNodeType type; ///< Type of this resource mapping node (currently, only sampler is supported)
  unsigned set;                 ///< ID of descriptor set
  unsigned binding;             ///< ID of descriptor binding
  unsigned arraySize;           ///< Element count for arrayed binding
  const unsigned *pValue;       ///< Static SRDs
  unsigned visibility;          ///< Mask composed of ShaderStageBit values
};

/// Represents the resource mapping data provided during pipeline creation
struct ResourceMappingData {
  /// User data nodes, providing the root-level mapping of descriptors in user-data entries (physical registers or
  /// GPU memory) to resources referenced in this pipeline shader.
  /// NOTE: Normally, this user data will correspond to the GPU's user data registers. However, Compiler needs some
  /// user data registers for internal use, so some user data may spill to internal GPU memory managed by Compiler.
  const ResourceMappingRootNode *pUserDataNodes; ///< An array of user data nodes
  unsigned userDataNodeCount;                    ///< Count of user data nodes

  const StaticDescriptorValue *pStaticDescriptorValues; ///< An array of static descriptors
  unsigned staticDescriptorValueCount;                  ///< Count of static descriptors
};

/// Represents shader binary data.
struct BinaryData {
  size_t codeSize;   ///< Size of shader binary data
  const void *pCode; ///< Shader binary data
};

/// Values for shadowDescriptorTableUsage pipeline option.
enum class ShadowDescriptorTableUsage : unsigned {
  Auto = 0, ///< Use 0 for auto setting so null initialized structures default to auto.
  Enable = 1,
  Disable = 2,
};

/// Represents the features of VK_EXT_robustness2
struct ExtendedRobustness {
  bool robustBufferAccess; ///< Whether buffer accesses are tightly bounds-checked against the range of the descriptor.
                           ///  Give defined behavior (e.g. read 0) for out-of-bounds buffer access and descriptor range
                           ///  rounding up.
  bool robustImageAccess;  ///< Whether image accesses are tightly bounds-checked against the dimensions of the image
                           ///  view. Give defined behavior for out-of-bounds image access.
  bool nullDescriptor;     ///< Whether the descriptor can be written with VK_NULL_HANDLE. If set, it is considered
                           ///  valid to access and acts as if the descriptor is bounded to nothing.
};

/// Represents the tiling modes for compute shader thread group swizzling
enum class ThreadGroupSwizzleMode : unsigned {
  Default = 0, ///< Use the default layout. There is no swizzling conducted.
  _4x4 = 1,    ///< The tile size is 4x4 in x and y dimension.
  _8x8 = 2,    ///< The tile size is 8x8 in x and y dimension.
  _16x16 = 3,  ///< The tile size is 16x16 in x and y dimension.
  Count
};

/// Represents mapping layout of the resources used in shaders
enum class ResourceLayoutScheme : unsigned {
  Compact = 0, ///< Compact scheme make full use of all the user data registers.
  Indirect     ///< Fixed layout, push constant will be the sub node of DescriptorTableVaPtr
               ///  In indirect scheme, The order of resources is like this:
               ///  1. one user data entry for vertex buffer
               ///  2. one user data entry for the push constant buffer
               ///  3. descriptor set index for each set
};

/// Represents per pipeline options.
struct PipelineOptions {
  bool includeDisassembly;         ///< If set, the disassembly for all compiled shaders will be included in
                                   ///  the pipeline ELF.
  bool scalarBlockLayout;          ///< If set, allows scalar block layout of types.
  bool reconfigWorkgroupLayout;    ///< If set, allows automatic workgroup reconfigure to take place on compute shaders.
  bool forceCsThreadIdSwizzling;   ///< Force rearranges threadId within group into blocks of 8*8 or 8*4.
  bool includeIr;                  ///< If set, the IR for all compiled shaders will be included in the pipeline ELF.
  bool robustBufferAccess;         ///< If set, out of bounds accesses to buffer or private array will be handled.
                                   ///  for now this option is used by LLPC shader and affects only the private array,
                                   ///  the out of bounds accesses will be skipped with this setting.
  bool enableRelocatableShaderElf; ///< If set, the pipeline will be compiled by compiling each shader separately, and
                                   ///  then linking them, when possible.  When not possible this option is ignored.
  bool disableImageResourceCheck;  ///< If set, the pipeline shader will not contain code to check and fix invalid image
                                   ///  descriptors.
  bool enableScratchAccessBoundsChecks; ///< If set, out of bounds guards will be inserted in the LLVM IR for OpLoads
                                        ///< and OpStores in private and function memory storage.
  bool enableImplicitInvariantExports;  ///< If set, enable implicit marking of position exports as invariant.
  ShadowDescriptorTableUsage shadowDescriptorTableUsage; ///< Controls shadow descriptor table.
  unsigned shadowDescriptorTablePtrHigh;                 ///< Sets high part of VA ptr for shadow descriptor table.
  ExtendedRobustness extendedRobustness;                 ///< ExtendedRobustness is intended to correspond to the
                                                         ///  features of VK_EXT_robustness2.
  bool enableRayQuery;                                   ///< If set, ray query is enabled
  bool optimizeTessFactor;    ///< If set, we can determine either send HT_TessFactor message or write to TF buffer
                              ///< depending the values of tessellation factors.
  bool enableInterpModePatch; ///< If set, per-sample interpolation for nonperspective and smooth input is enabled
  bool pageMigrationEnabled;  ///< If set, page migration is enabled
  uint32_t optimizationLevel; ///< The higher the number the more optimizations will be performed.  Valid values are
                              ///< between 0 and 3.
  unsigned overrideThreadGroupSizeX;              ///< Override value for ThreadGroupSizeX
  unsigned overrideThreadGroupSizeY;              ///< Override value for ThreadGroupSizeY
  unsigned overrideThreadGroupSizeZ;              ///< Override value for ThreadGroupSizeZ
  ResourceLayoutScheme resourceLayoutScheme;      ///< Resource layout scheme
  ThreadGroupSwizzleMode threadGroupSwizzleMode;  ///< Controls thread group swizzle mode for compute shader.
  bool reverseThreadGroup;                        ///< If set, enable thread group reversing
  bool internalRtShaders;                         ///< Whether this pipeline has internal raytracing shaders
  unsigned forceNonUniformResourceIndexStageMask; ///< Mask of the stage to force using non-uniform resource index.
  bool reserved16;
  bool replaceSetWithResourceType;        ///< For OGL only, replace 'set' with resource type during spirv translate
  bool disableSampleMask;                 ///< For OGL only, disabled if framebuffer doesn't attach multisample texture
  bool buildResourcesDataForShaderModule; ///< For OGL only, build resources usage data while building shader module
  bool disableTruncCoordForGather;        ///< If set, trunc_coord of sampler srd is disabled for gather4
  bool enableCombinedTexture;             ///< For OGL only, use the 'set' for DescriptorCombinedTexture
                                          ///< for sampled images and samplers
  bool vertex64BitsAttribSingleLoc;       ///< For OGL only, dvec3/dvec4 vertex attrib only consumes 1 location.
};

/// Prototype of allocator for output data buffer, used in shader-specific operations.
typedef void *(VKAPI_CALL *OutputAllocFunc)(void *pInstance, void *pUserData, size_t size);

/// Enumerates types of shader binary.
enum class BinaryType : unsigned {
  Unknown = 0, ///< Invalid type
  Spirv,       ///< SPIR-V binary
  LlvmBc,      ///< LLVM bitcode
  MultiLlvmBc, ///< Multiple LLVM bitcode
  Elf,         ///< ELF
};

/// Represents resource node data
struct ResourceNodeData {
  ResourceMappingNodeType type;     ///< Type of this resource mapping node
  unsigned set;                     ///< ID of descriptor set
  unsigned binding;                 ///< ID of descriptor binding
  unsigned arraySize;               ///< Element count for arrayed binding
  unsigned location;                ///< ID of resource location
  unsigned isTexelBuffer;           ///< TRUE if it is ImageBuffer or TextureBuffer
  unsigned isDefaultUniformSampler; ///< TRUE if it's sampler image in default uniform struct
  BasicType basicType;              ///< Type of the variable or element
};

/// Represents the information of one shader entry in ShaderModuleExtraData
struct ShaderModuleEntryData {
  ShaderStage stage;                     ///< Shader stage
  const char *pEntryName;                ///< Shader entry name
  void *pShaderEntry;                    ///< Private shader module entry info
  unsigned resNodeDataCount;             ///< Resource node data count
  const ResourceNodeData *pResNodeDatas; ///< Resource node data array
  unsigned pushConstSize;                ///< Push constant size in byte
};

/// Represents the shader resources
struct ResourcesNodes {
  ResourceNodeData *pInputInfo;
  uint32_t inputInfoCount;
  ResourceNodeData *pOutputInfo;
  uint32_t outputInfoCount;
  ResourceNodeData *pUniformBufferInfo;
  uint32_t uniformBufferInfoCount;
  ResourceNodeData *pShaderStorageInfo;
  uint32_t shaderStorageInfoCount;
  ResourceNodeData *pTexturesInfo;
  uint32_t textureInfoCount;
  ResourceNodeData *pImagesInfo;
  uint32_t imageInfoCount;
  ResourceNodeData *pAtomicCounterInfo;
  uint32_t atomicCounterInfoCount;
  ResourceNodeData *pDefaultUniformInfo;
  uint32_t defaultUniformInfoCount;
};

/// Represents usage info of a shader module
struct ShaderModuleUsage {
  bool enableVarPtrStorageBuf; ///< Whether to enable "VariablePointerStorageBuffer" capability
  bool enableVarPtr;           ///< Whether to enable "VariablePointer" capability
  bool useSubgroupSize;        ///< Whether gl_SubgroupSize is used
  bool useSpecConstant;        ///< Whether specialization constant is used
  bool keepUnusedFunctions;    ///< Whether to keep unused function
  bool enableRayQuery;         ///< Whether the "RayQueryKHR" capability is used
  bool rayQueryLibrary;        ///< Whether the shaderModule is rayQueryLibrary
  bool isInternalRtShader;     ///< Whether the shaderModule is a GPURT internal shader (e.g. BVH build)
  bool hasTraceRay;            ///< Whether the shaderModule has OpTraceRayKHR
  bool hasExecuteCallable;     ///< Whether the shaderModule has OpExecuteCallableKHR
  bool useIsNan;               ///< Whether IsNan is used
  bool useInvariant;           ///< Whether invariant variable is used
  bool usePointSize;           ///< Whether gl_PointSize is used in output
  bool useShadingRate;         ///< Whether shading rate is used
  bool useSampleInfo;          ///< Whether gl_SamplePosition or InterpolateAtSample are used
  bool useClipVertex;          ///< Whether gl_useClipVertex is used
  ResourcesNodes *pResources;  ///< Resource node for buffers and opaque types
};

/// Represents common part of shader module data
struct ShaderModuleData {
  unsigned hash[4];        ///< Shader hash code
  BinaryType binType;      ///< Shader binary type
  BinaryData binCode;      ///< Shader binary data
  unsigned cacheHash[4];   ///< Hash code for calculate pipeline cache key
  ShaderModuleUsage usage; ///< Usage info of a shader module
};

/// Represents the options for pipeline dump.
struct PipelineDumpOptions {
  const char *pDumpDir;              ///< Pipeline dump directory
  unsigned filterPipelineDumpByType; ///< Filter which types of pipeline dump are enabled
  uint64_t filterPipelineDumpByHash; ///< Only dump the pipeline with this compiler hash if non-zero
  bool dumpDuplicatePipelines;       ///< If TRUE, duplicate pipelines will be dumped to a file with a
                                     ///  numeric suffix attached
};

/// Enumerate denormal override modes.
enum class DenormalMode : unsigned {
  Auto = 0x0,        ///< No denormal override (default behaviour)
  FlushToZero = 0x1, ///< Denormals flushed to zero
  Preserve = 0x2,    ///< Denormals preserved
};

/// If next available quad falls outside tile aligned region of size defined by this enumeration the SC will force end
/// of vector in the SC to shader wavefront.
enum class WaveBreakSize : unsigned {
  None = 0x0,   ///< No wave break by region
  _8x8 = 0x1,   ///< Outside a 8x8 pixel region
  _16x16 = 0x2, ///< Outside a 16x16 pixel region
  _32x32 = 0x3, ///< Outside a 32x32 pixel region
};

/// Enumerates various sizing options of subgroup size for NGG primitive shader.
enum class NggSubgroupSizingType : unsigned {
  Auto,             ///< Subgroup size is allocated as optimally determined
  MaximumSize,      ///< Subgroup size is allocated to the maximum allowable size by the hardware
  HalfSize,         ///< Subgroup size is allocated as to allow half of the maximum allowable size
                    ///  by the hardware
  OptimizeForVerts, ///< Subgroup size is optimized for vertex thread utilization
  OptimizeForPrims, ///< Subgroup size is optimized for primitive thread utilization
  Explicit,         ///< Subgroup size is allocated based on explicitly-specified vertsPerSubgroup and
                    ///  primsPerSubgroup
};

/// Represents NGG tuning options
struct NggState {
  bool enableNgg;                 ///< Enable NGG mode, use an implicit primitive shader
  bool enableGsUse;               ///< Enable NGG use on geometry shader
  bool forceCullingMode;          ///< Force NGG to run in culling mode
  bool compactVertex;             ///< Enable NGG vertex compaction after culling
  bool enableBackfaceCulling;     ///< Enable culling of primitives that don't meet facing criteria
  bool enableFrustumCulling;      ///< Enable discarding of primitives outside of view frustum
  bool enableBoxFilterCulling;    ///< Enable simpler frustum culler that is less accurate
  bool enableSphereCulling;       ///< Enable frustum culling based on a sphere
  bool enableSmallPrimFilter;     ///< Enable trivial sub-sample primitive culling
  bool enableCullDistanceCulling; ///< Enable culling when "cull distance" exports are present

  /// Following fields are used for NGG tuning
  unsigned backfaceExponent; ///< Value from 1 to UINT32_MAX that will cause the backface culling
                             ///  algorithm to ignore area calculations that are less than
                             ///  (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
                             ///  Only valid if the NGG backface culler is enabled.
                             ///  A value of 0 will disable the threshold.

  NggSubgroupSizingType subgroupSizing; ///< NGG subgroup sizing type

  unsigned primsPerSubgroup; ///< Preferred number of GS primitives to pack into a primitive shader
                             ///  subgroup

  unsigned vertsPerSubgroup; ///< Preferred number of vertices consumed by a primitive shader subgroup
};

/// ShaderHash represents a 128-bit client-specified hash key which uniquely identifies a shader program.
struct ShaderHash {
  uint64_t lower; ///< Lower 64 bits of hash key.
  uint64_t upper; ///< Upper 64 bits of hash key.
};

/// Compacts a 128-bit hash into a 32-bit one by XOR'ing each 32-bit chunk together.
///
/// Takes input parameter ShaderHash, which is a struct consisting of 2 quad words to be compacted.
//
/// Returns 32-bit hash value based on the input 128-bit hash.
inline unsigned compact32(ShaderHash hash) {
  return (static_cast<unsigned>(hash.lower) ^ static_cast<unsigned>(hash.lower >> 32) ^
          static_cast<unsigned>(hash.upper) ^ static_cast<unsigned>(hash.upper >> 32));
}

/// Represent a pipeline option which can be automatic as well as explicitly set.
enum InvariantLoads : unsigned { Auto = 0, EnableOptimization = 1, DisableOptimization = 2, ClearInvariants = 3 };

/// Represents per shader stage options.
struct PipelineShaderOptions {
  ShaderHash clientHash;      ///< Client-supplied unique shader hash. A value of zero indicates that LLPC should
                              ///  calculate its own hash. This hash is used for dumping, shader replacement, SPP, etc.
                              ///  If the client provides this hash, they are responsible for ensuring it is as stable
                              ///  as possible.
  bool trapPresent;           ///< Indicates a trap handler will be present when this pipeline is executed,
                              ///  and any trap conditions encountered in this shader should call the trap
                              ///  handler. This could include an arithmetic exception, an explicit trap
                              ///  request from the host, or a trap after every instruction when in debug
                              ///  mode.
  bool debugMode;             ///< When set, this shader should cause the trap handler to be executed after
                              ///  every instruction.  Only valid if trapPresent is set.
  bool enablePerformanceData; ///< Enables the compiler to generate extra instructions to gather
                              ///  various performance-related data.
  bool allowReZ;              ///< Allow the DB ReZ feature to be enabled.  This will cause an early-Z test
                              ///  to potentially kill PS waves before launch, and also issues a late-Z test
                              ///  in case the PS kills pixels.  Only valid for pixel shaders.
  /// Maximum VGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
  /// of this value and whatever the target GPU supports. To effectively disable this limit, set this to UINT_MAX.
  unsigned vgprLimit;

  /// Maximum SGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
  /// of this value and whatever the target GPU supports. To effectively disable this limit, set this to UINT_MAX.
  unsigned sgprLimit;

  /// Overrides the number of CS thread-groups which the GPU will launch per compute-unit. This throttles the
  /// shader, which can sometimes enable more graphics shader work to complete in parallel. A value of zero
  /// disables limiting the number of thread-groups to launch. This field is ignored for graphics shaders.
  unsigned maxThreadGroupsPerComputeUnit;

  unsigned subgroupSize;       ///< The number of invocations in each subgroup, it is a power-of-two. 0 means
                               ///  the size is unspecified, the current reasonable value should be 32 or 64.
  unsigned waveSize;           ///< Control the number of threads per wavefront (GFX10+)
  bool wgpMode;                ///< Whether to choose WGP mode or CU mode (GFX10+)
  WaveBreakSize waveBreakSize; ///< Size of region to force the end of a wavefront (GFX10+).
                               ///  Only valid for fragment shaders.

  /// Force loop unroll count. "0" means using default value; "1" means disabling loop unroll.
  unsigned forceLoopUnrollCount;

  /// Enable LLPC load scalarizer optimization.
  bool enableLoadScalarizer;
  /// If set, lets the pipeline vary the wave sizes.
  bool allowVaryWaveSize;
  /// Use the LLVM backend's SI scheduler instead of the default scheduler.
  bool useSiScheduler;

  /// Disable various LLVM IR code sinking passes.
  bool disableCodeSinking;

  /// Schedule for latency even if it reduces occupancy.
  bool favorLatencyHiding;

  /// Disable the LLVM backend's LICM pass (equivalent to disableLicmThreshold=1).
  bool disableLicm;

  /// Default unroll threshold for LLVM.
  unsigned unrollThreshold;

  /// The threshold for load scalarizer.
  unsigned scalarThreshold;

  /// Forcibly disable loop unrolling - overrides any explicit unroll directives
  bool disableLoopUnroll;

  /// Whether enable adjustment of the fragment shader depth import for the variable shading rate
  bool adjustDepthImportVrs;

  /// Override FP32 denormal handling.
  DenormalMode fp32DenormalMode;

  /// Threshold number of blocks in a loop for LICM pass to be disabled.
  unsigned disableLicmThreshold;

  /// Threshold to use for loops with "Unroll" hint (0 = use llvm.llop.unroll.full).
  unsigned unrollHintThreshold;

  /// Threshold to use for loops with "DontUnroll" hint (0 = use llvm.llop.unroll.disable).
  unsigned dontUnrollHintThreshold;

  /// Whether fastmath contract could be disabled on Dot operations.
  bool noContractOpDot;

  /// The enabled fast math flags (0 = depends on input language).
  unsigned fastMathFlags;

  /// Disable fast math flags mask (0 = nothing disabled).
  unsigned disableFastMathFlags;

  /// Maximum amount of LDS space to be used for spilling.
  unsigned ldsSpillLimitDwords;

  /// Attempt to scalarize waterfall descriptor loads.
  bool scalarizeWaterfallLoads;

  /// Force rearranges threadId within group into blocks of 8*8 or 8*4
  bool overrideForceThreadIdSwizzling;

  /// Override value for ThreadGroupSizeX
  unsigned overrideShaderThreadGroupSizeX;

  /// Override value for ThreadGroupSizeY
  unsigned overrideShaderThreadGroupSizeY;

  /// Override value for ThreadGroupSizeZ
  unsigned overrideShaderThreadGroupSizeZ;

  /// When there is a valid "feedback loop" in renderpass, lateZ needs to be enabled
  /// In Vulkan a "feedback loop" is described as a subpass where there is at least
  /// one input attachment that is also a color or depth/stencil attachment
  /// Feedback loops are allowed and their behavior is well defined under certain conditions.
  /// When there is a feedback loop it is possible for the shaders to read
  /// the contents of the color and depth/stencil attachments
  /// from the shader during draw. Because of that possibility you have to use late-z
  bool forceLateZ;

  /// Minimum number of addresses to use NSA encoding on GFX10+ (0 = backend decides).
  unsigned nsaThreshold;

  /// Aggressively mark shader loads as invariant (where it is safe to do so).
  InvariantLoads aggressiveInvariantLoads;

  /// Disable an optimization that relies on trusting shaders to specify the correct image format to reduce the number
  /// of written channels.
  bool workaroundStorageImageFormats;

  /// Initialize outputs to zero if it is true
  bool workaroundInitializeOutputsToZero;

  /// Application workaround: Treat GLSL.ext fma instruction as OpFMul + OpFAdd
  bool disableFMA;

  /// Disable to emulate DX's readfirst lane workaround in BIL
  bool disableReadFirstLaneWorkaround;

  /// Application workaround: back propagate NoContraction decoration to all inputs to a NoContraction operation.
  bool backwardPropagateNoContract;

  /// Application workaround: forward propagate NoContraction decoration to any related FAdd operation.
  bool forwardPropagateNoContract;
};

/// Represents YCbCr sampler meta data in resource descriptor
struct SamplerYCbCrConversionMetaData {
  union {
    struct {                     ///< e.g R12X4G12X4_UNORM_2PACK16
      unsigned channelBitsR : 5; ///< channelBitsR = 12
      unsigned channelBitsG : 5; ///< channelBitsG = 12
      unsigned channelBitsB : 5; ///< channelBitsB =  0
      unsigned : 17;
    } bitDepth;
    struct {
      unsigned : 15;         ///< VkComponentSwizzle, e.g
      unsigned swizzleR : 3; ///< swizzleR = VK_COMPONENT_SWIZZLE_R(3)
      unsigned swizzleG : 3; ///< swizzleG = VK_COMPONENT_SWIZZLE_G(4)
      unsigned swizzleB : 3; ///< swizzleB = VK_COMPONENT_SWIZZLE_B(5)
      unsigned swizzleA : 3; ///< swizzleA = VK_COMPONENT_SWIZZLE_A(6)
      unsigned : 5;
    } componentMapping;
    struct {
      unsigned : 27;
      unsigned yCbCrModel : 3;               ///< RGB_IDENTITY(0), ycbcr_identity(1),
                                             ///  _709(2),_601(3),_2020(4)
      unsigned yCbCrRange : 1;               ///< ITU_FULL(0), ITU_NARROW(0)
      unsigned forceExplicitReconstruct : 1; ///< Disable(0), Enable(1)
    };
    unsigned u32All;
  } word0;

  union {
    struct {
      unsigned planes : 2;        ///< Number of planes, normally from 1 to 3
      unsigned lumaFilter : 1;    ///< FILTER_NEAREST(0) or FILTER_LINEAR(1)
      unsigned chromaFilter : 1;  ///< FILTER_NEAREST(0) or FILTER_LINEAR(1)
      unsigned xChromaOffset : 1; ///< COSITED_EVEN(0) or MIDPOINT(1)
      unsigned yChromaOffset : 1; ///< COSITED_EVEN(0) or MIDPOINT(1)
      unsigned xSubSampled : 1;   ///< true(1) or false(0)
      unsigned : 1;               ///< Unused
      unsigned ySubSampled : 1;   ///< true(1) or false(0)
      unsigned dstSelXYZW : 12;   ///< dst selection Swizzle
      unsigned undefined : 11;
    };
    unsigned u32All;
  } word1;

  union {
    /// For YUV formats, bitCount may not equal to bitDepth, where bitCount >= bitDepth
    struct {
      unsigned xBitCount : 6; ///< Bit count for x channel
      unsigned yBitCount : 6; ///< Bit count for y channel
      unsigned zBitCount : 6; ///< Bit count for z channel
      unsigned wBitCount : 6; ///< Bit count for w channel
      unsigned undefined : 8;
    } bitCounts;
    unsigned u32All;
  } word2;

  union {
    struct {
      unsigned sqImgRsrcWord1 : 32; ///< Reconstructed sqImgRsrcWord1
    };
    unsigned u32All;
  } word3;

  union {
    struct {
      unsigned lumaWidth : 16;  ///< Actual width of luma plane
      unsigned lumaHeight : 16; ///< Actual height of luma plane
    };
    unsigned u32All;
  } word4;

  union {
    struct {
      unsigned lumaDepth : 16; ///< Actual array slices of luma plane
      unsigned : 16;
    };
    unsigned u32All;
  } word5;
};

/// Represents assistant info for each vertex attribute in uber fetch shader
struct UberFetchShaderAttribInfo {
  uint32_t binding : 8;       ///< Attribute binding in vertex buffer table
  uint32_t perInstance : 1;   ///< Whether vertex input rate is per-instance
  uint32_t isCurrent : 1;     ///< Whether it is a current attribute
  uint32_t isPacked : 1;      ///< Whether it is a packed format
  uint32_t isFixed : 1;       ///< Whether it is a fixed format
  uint32_t componentSize : 4; ///< Byte size per component
  uint32_t componentMask : 4; ///< Component mask of this attribute.
  uint32_t isBgra : 1;        ///< Whether is BGRA format
  uint32_t reserved : 11;     ///< reserved bits in DWORD 0
  uint32_t offset;            ///< Attribute offset
  uint32_t instanceDivisor;   ///< Reciprocal of instance divisor
  uint32_t bufferFormat;      ///< Buffer format info. it is a copy of buffer SRD DWORD3.
};

/// Represents the bit field info of struct BilUberFetchShaderAttribInfo
constexpr uint32_t UberFetchShaderAttribMaskBinding = 0x00000FFu;
constexpr uint32_t UberFetchShaderAttribMaskPerInstance = 0x0000100u;
constexpr uint32_t UberFetchShaderAttribMaskIsCurrent = 0x0000200u;
constexpr uint32_t UberFetchShaderAttribMaskIsPacked = 0x0000400u;
constexpr uint32_t UberFetchShaderAttribMaskIsFixed = 0x0000800u;
constexpr uint32_t UberFetchShaderAttribMaskComponentSize = 0x000F000u;
constexpr uint32_t UberFetchShaderAttribShiftComponentSize = 12u;
constexpr uint32_t UberFetchShaderAttribMaskComponent0 = 0x0010000u;
constexpr uint32_t UberFetchShaderAttribMaskComponent1 = 0x0020000u;
constexpr uint32_t UberFetchShaderAttribMaskComponent2 = 0x0040000u;
constexpr uint32_t UberFetchShaderAttribMaskComponent3 = 0x0080000u;
constexpr uint32_t UberFetchShaderAttribMaskIsBgra = 0x0100000u;

/// Represents info of a shader attached to a to-be-built pipeline.
struct PipelineShaderInfo {
  const void *pModuleData;                         ///< Shader module data used for pipeline building (opaque)
  const VkSpecializationInfo *pSpecializationInfo; ///< Specialization constant info
  const char *pEntryTarget;                        ///< Name of the target entry point (for multi-entry)
  ShaderStage entryStage;                          ///< Shader stage of the target entry point
  PipelineShaderOptions options;                   ///< Per shader stage tuning/debugging options
};

/// Represents color target info
struct ColorTarget {
  bool blendEnable;          ///< Blend will be enabled for this target at draw time
  bool blendSrcAlphaToColor; ///< Whether source alpha is blended to color channels for this target
                             ///  at draw time
  uint8_t channelWriteMask;  ///< Write mask to specify destination channels
  VkFormat format;           ///< Color attachment format
};

// Maximum size of descriptor in dwords
static const unsigned MaxDescriptorSize = 8;

/// Represents BVH shader resource descriptor
struct BvhShaderResourceDescriptor {
  unsigned descriptorData[MaxDescriptorSize]; ///< BVH descriptor data
  unsigned dataSizeInDwords;                  ///< BVH buffer descriptor size in dword
};

// Corresponds to gl_RayFlags* in GLSL_EXT_ray_tracing.txt
enum RayTracingRayFlag : uint32_t {
  RayTracingRayFlagNone = 0x00,                       // gl_RayFlagsNoneEXT
  RayTracingRayFlagForceOpaque = 0x01,                // gl_RayFlagsOpaqueEXT
  RayTracingRayFlagForceNonOpaque = 0x02,             // gl_RayFlagsNoOpaqueEXT
  RayTracingRayFlagAcceptFirstHitAndEndSearch = 0x04, // gl_RayFlagsTerminateOnFirstHitEXT
  RayTracingRayFlagSkipClosestHitShader = 0x08,       // gl_RayFlagsSkipClosestHitShaderEXT
  RayTracingRayFlagCullBackFacingTriangles = 0x10,    // gl_RayFlagsCullBackFacingTrianglesEXT
  RayTracingRayFlagCullFrontFacingTriangles = 0x20,   // gl_RayFlagsCullFrontFacingTrianglesEXT
  RayTracingRayFlagCullOpaque = 0x40,                 // gl_RayFlagsCullOpaqueEXT
  RayTracingRayFlagCullNonOpaque = 0x80,              // gl_RayFlagsCullNoOpaqueEXT
  RayTracingRayFlagSkipTriangles = 0x100,             // gl_RayFlagsSkipTrianglesEXT
  RayTracingRayFlagSkipAabb = 0x200,                  // gl_RayFlagsSkipAABBEXT
};

// =====================================================================================================================
// raytracing system value usage flags
union RayTracingSystemValueUsage {
  struct {
    union {
      struct {
        uint16_t flags : 1;             // Shader calls gl_IncomingRayFlagsEXT
        uint16_t worldRayOrigin : 1;    // Shader calls gl_WorldRayOriginEXT
        uint16_t tMin : 1;              // Shader calls gl_RayTminEXT
        uint16_t worldRayDirection : 1; // Shader calls gl_WorldRayDirectionEXT
        uint16_t tCurrent : 1;          // Shader calls gl_HitTEXT
        uint16_t launchId : 1;          // Shader calls gl_LaunchIDEXT
        uint16_t launchSize : 1;        // Shader calls gl_LaunchSizeEXT
        uint16_t reserved : 9;          // Reserved
      };
      uint16_t u16All;
    } ray;

    union {
      struct {
        uint16_t hitKind : 1;             // Shader calls gl_HitKindEXT
        uint16_t instanceIndex : 1;       // Shader calls gl_InstanceCustomIndexEXT
        uint16_t instanceID : 1;          // Shader calls gl_InstanceID
        uint16_t primitiveIndex : 1;      // Shader calls gl_PrimitiveID
        uint16_t geometryIndex : 1;       // Shader calls gl_GeometryIndexEXT
        uint16_t objectToWorld : 1;       // Shader calls gl_ObjectToWorldEXT
        uint16_t objectRayOrigin : 1;     // Shader calls gl_ObjectRayOriginEXT
        uint16_t objectRayDirection : 1;  // Shader calls gl_ObjectRayDirectionEXT
        uint16_t worldToObject : 1;       // Shader calls gl_WorldToObjectEXT
        uint16_t hitTrianglePosition : 1; // Shader calls gl_HitTriangleVertexPositionsEXT
        uint16_t reserved : 6;            // Reserved
      };
      uint16_t u16All;
    } primitive;
  };
  uint32_t u32All;
};

/// Represents ray-tracing shader export configuration
struct RayTracingShaderExportConfig {
  unsigned indirectCallingConvention; ///< Indirect calling convention
  struct {
    unsigned raygen;         ///< Ray generation shader saved register
    unsigned miss;           ///< Miss shader saved register
    unsigned closestHit;     ///< Closest hit shader saved register
    unsigned anyHit;         ///< Any hit shader saved register
    unsigned intersection;   ///< Intersection shader saved register
    unsigned callable;       ///< Callable shader saved register
    unsigned traceRays;      ///< Trace ray shader saved register
  } indirectCalleeSavedRegs; ///< Indirect callee saved register

  bool enableUniformNoReturn;         // Enable flag indirect call as uniform-noreturn
  bool enableTraceRayArgsInLds;       // Enable TraceRay() call arguments in LDS
  bool enableReducedLinkageOpt;       // Enable reduced linkage across indirect call sites
  bool readsDispatchRaysIndex;        // Shader reads dispatchRaysIndex
  bool enableDynamicLaunch;           // Enable dynamic launch
  bool emitRaytracingShaderDataToken; // Emitting Raytracing ShaderData SQTT Token
};

/// Enumerates the method of mapping from ray tracing launch ID to native thread ID
enum DispatchDimSwizzleMode : unsigned {
  Native,             ///< Native mapping (width -> x, height -> y, depth -> z)
  FlattenWidthHeight, ///< Flatten width and height to x, and depth to y
};

enum class LlpcRaytracingMode : unsigned {
  None = 0,      // Not goto any raytracing compiling path
  Legacy,        // LLpc Legacy compiling path
  Gpurt2,        // Raytracing lowering at the end of spirvLower.
  Continuations, // Enable continuation in the new raytracing path
};

/// RayTracing state
struct RtState {
  unsigned nodeStrideShift;               ///< Ray tracing BVH node stride
  BvhShaderResourceDescriptor bvhResDesc; ///< Ray tracing BVH shader resource descriptor
  unsigned staticPipelineFlags;           ///< GPURT static pipeline flags constant
  unsigned triCompressMode;               ///< GPURT triangle compression mode constant
  unsigned boxSortHeuristicMode;          ///< GPURT box sort heuristic mode constant
  unsigned pipelineFlags;                 ///< Ray tracing pipeline flags
  unsigned counterMode;                   ///< Counter mode
  unsigned counterMask;                   ///< Traversal counter mask
  unsigned threadGroupSizeX;              ///< Thread group size for ray tracing shader
  unsigned threadGroupSizeY;
  unsigned threadGroupSizeZ;
  unsigned rayQueryCsSwizzle;                    ///< Swizzle for compute shader using ray query
  unsigned ldsStackSize;                         ///< LDS stack size
  unsigned dispatchRaysThreadGroupSize;          ///< Dispatch thread group size
  unsigned ldsSizePerThreadGroup;                ///< LDS size per thread group
  unsigned outerTileSize;                        ///< Size of outer tile about ray tracing shader thread group swizzling
  DispatchDimSwizzleMode dispatchDimSwizzleMode; ///< The method of mapping from ray tracing launch ID to native thread
                                                 ///  ID. Thread group size would be 1D and threadGroupSizeX would be
                                                 ///  the size of thread group in x-dim in FlattenWidthHeight mode.
                                                 ///  Shader may need to inverse the mapping if launch ID is used.
  RayTracingShaderExportConfig exportConfig;     ///< Ray tracing shader export config
  bool enableRayQueryCsSwizzle;                  ///< Determine if performs thread group swizzling
                                                 ///  for computer shaders use ray query feature
  bool enableDispatchRaysInnerSwizzle;           ///< Perform swizzling logic on the thread indices inside ray tracing
                                                 ///  thread groups
  bool enableDispatchRaysOuterSwizzle;           ///< Perform swizzling logic on the thread group indices used for ray
                                                 ///  tracing shaders
  bool forceInvalidAccelStruct;                  ///< Force ray tracing invalid acceleration structure
  bool enableRayTracingCounters;                 ///< Enable using ray tracing counters
  bool enableRayTracingHwTraversalStack;         ///< Enable using hardware accelerated traversal stack
  bool enableOptimalLdsStackSizeForIndirect;     ///< Enable optimal LDS stack size for indirect shaders
  bool enableOptimalLdsStackSizeForUnified;      ///< Enable optimal LDS stack size for unified shaders
  float maxRayLength;                            ///< Raytracing rayDesc.tMax override
  unsigned gpurtFeatureFlags;                    ///< GPURT features flags to use for the pipeline compile
  BinaryData gpurtShaderLibrary;                 ///< GPURT shader library
  GpurtFuncTable gpurtFuncTable;                 ///< GPURT function table
  RtIpVersion rtIpVersion;                       ///< RT IP version

  /// If true, force the compiler to use gpurtShaderLibrary. Otherwise, it is up to the compiler whether it uses
  /// gpurtShaderLibrary or obtains a library directly from GPURT.
  ///
  /// If LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62, the compiler always behaves as if this was true.
  bool gpurtOverride;

  /// If true, force the compile to use rtIpVersion. If false, the compiler may derive the default RTIP version from
  /// the GFXIP version.
  ///
  /// If LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62, the compiler always behaves as if this was true.
  bool rtIpOverride;
};

struct UniformConstantMapEntry {
  uint32_t location; ///< Starting location of the uniform constant variable
  uint32_t offset;   ///< Offset of the uniform constant variable in the final buffer
};

struct UniformConstantMap {
  uint32_t visibility;                ///< Mask composed of ShaderStageBit values
  unsigned numUniformConstants;       ///< Number of uniform constant entries in the map
  UniformConstantMapEntry *pUniforms; ///< Mapping of <location, offset> for uniform constant
};

/// Represents transform feedback info for the caputred output
struct XfbOutInfo {
  bool isBuiltIn;     ///< Determine if it is a built-in output
  unsigned location;  ///< If isBuiltIn is true, it is the buildIn Id
  unsigned component; ///< The component offset within a location
  unsigned xfbBuffer; ///< The transform feedback buffer captures the output
  unsigned xfbOffset; ///< The byte offset in the transform feedback buffer
  unsigned xfbStride; ///< The bytes consumed by a caputred vertex in the transform feedback buffer
  unsigned streamId;  ///< The stream index
};

/// Represents the transform feedback data filled by API interface
struct ApiXfbOutData {
  XfbOutInfo *pXfbOutInfos;   ///< An array of XfbOutInfo iterms
  unsigned numXfbOutInfo;     ///< Count of XfbOutInfo iterms
  bool forceDisableStreamOut; ///< Force to disable stream out XFB outputs
  bool forceEnablePrimStats;  ///< Force to enable counting generated primitives
};

/// Represents the tessellation level passed from driver API
struct TessellationLevel {
  float inner[2]; ///< Inner tessellation level
  float outer[4]; ///< Outer tessellation level
};

/// Represents info to build a graphics pipeline.
struct GraphicsPipelineBuildInfo {
  void *pInstance;                ///< Vulkan instance object
  void *pUserData;                ///< User data
  OutputAllocFunc pfnOutputAlloc; ///< Output buffer allocator
  ICache *cache;                  ///< ICache, used to search for the compiled shader data
  PipelineShaderInfo task;        ///< Task shader
  PipelineShaderInfo vs;          ///< Vertex shader
  PipelineShaderInfo tcs;         ///< Tessellation control shader
  PipelineShaderInfo tes;         ///< Tessellation evaluation shader
  PipelineShaderInfo gs;          ///< Geometry shader
  PipelineShaderInfo mesh;        ///< Mesh shader
  PipelineShaderInfo fs;          ///< Fragment shader

  ResourceMappingData resourceMapping; ///< Resource mapping graph and static descriptor values
  uint64_t pipelineLayoutApiHash;      ///< Pipeline Layout Api Hash

  /// Create info of vertex input state
  const VkPipelineVertexInputStateCreateInfo *pVertexInput;

  // Depth/stencil state
  VkPipelineDepthStencilStateCreateInfo dsState;

  struct {
    VkPrimitiveTopology topology;  ///< Primitive topology
    unsigned patchControlPoints;   ///< Number of control points per patch (valid when the topology is
                                   ///  "patch")
    unsigned deviceIndex;          ///< Device index for device group
    bool disableVertexReuse;       ///< Disable reusing vertex shader output for indexed draws
    bool switchWinding;            ///< Whether to reverse vertex ordering for tessellation
    bool enableMultiView;          ///< Whether to enable multi-view support
    bool useVertexBufferDescArray; ///< Whether vertex buffer descriptors are in a descriptor array binding instead of
                                   ///< the VertexBufferTable
    TessellationLevel *tessLevel;  ///< Tessellation level passed from driver
  } iaState;                       ///< Input-assembly state

  struct {
    bool depthClipEnable; ///< Enable clipping based on Z coordinate
  } vpState;              ///< Viewport state

  struct {
    bool rasterizerDiscardEnable; ///< Kill all rasterized pixels. This is implicitly true if stream out
                                  ///  is enabled and no streams are rasterized
    bool innerCoverage;           ///< Related to conservative rasterization.  Must be false if
                                  ///  conservative rasterization is disabled.
    bool perSampleShading;        ///< Enable per sample shading
    uint8_t usrClipPlaneMask;     ///< Mask to indicate the enabled user defined clip planes
    unsigned numSamples;          ///< Number of coverage samples used when rendering with this pipeline
    unsigned pixelShaderSamples;  ///< Controls the pixel shader execution rate. Must be less than or equal to
                                  ///  coverageSamples. Valid values are 1, 2, 4, and 8.
    unsigned samplePatternIdx;    ///< Index into the currently bound MSAA sample pattern table that
                                  ///  matches the sample pattern used by the rasterizer when rendering
                                  ///  with this pipeline.
    unsigned dynamicSampleInfo;   ///< Dynamic sampling is enabled.
    unsigned rasterStream;        ///< Which vertex stream to rasterize
    VkProvokingVertexModeEXT provokingVertexMode; ///< Specifies which vertex of a primitive is the _provoking
                                                  ///  vertex_, this impacts which vertex's "flat" VS outputs
                                                  ///  are passed to the PS.
  } rsState;                                      ///< Rasterizer State
  struct {
    bool alphaToCoverageEnable;  ///< Enable alpha to coverage
    bool dualSourceBlendEnable;  ///< Blend state bound at draw time will use a dual source blend mode
    bool dualSourceBlendDynamic; ///< Dual source blend mode is dynamically set.

    ColorTarget target[MaxColorTargets]; ///< Per-MRT color target info
  } cbState;                             ///< Color target state

  NggState nggState;            ///< NGG state used for tuning and debugging
  PipelineOptions options;      ///< Per pipeline tuning/debugging options
  bool unlinked;                ///< True to build an "unlinked" half-pipeline ELF
  bool dynamicVertexStride;     ///< Dynamic Vertex input Stride is enabled.
  bool enableUberFetchShader;   ///< Use uber fetch shader
  bool enableColorExportShader; ///< Explicitly build color export shader, UnlinkedStageFragment elf will
                                ///  return extra meta data.
  bool enableEarlyCompile;      ///< Whether enable early compile
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  BinaryData shaderLibrary; ///< SPIR-V library binary data
#endif
  RtState rtState;                    ///< Ray tracing state
  const void *pClientMetadata;        ///< Pointer to (optional) client-defined data to be stored inside the ELF
  size_t clientMetadataSize;          ///< Size (in bytes) of the client-defined data
  unsigned numUniformConstantMaps;    ///< Number of uniform constant maps
  UniformConstantMap **ppUniformMaps; ///< Pointers to array of pointers for the uniform constant map.
  ApiXfbOutData apiXfbOutData;        ///< Transform feedback data specified by API interface.
};

/// Represents info to build a compute pipeline.
struct ComputePipelineBuildInfo {
  void *pInstance;                     ///< Vulkan instance object
  void *pUserData;                     ///< User data
  OutputAllocFunc pfnOutputAlloc;      ///< Output buffer allocator
  ICache *cache;                       ///< ICache, used to search for the compiled shader data
  unsigned deviceIndex;                ///< Device index for device group
  PipelineShaderInfo cs;               ///< Compute shader
  ResourceMappingData resourceMapping; ///< Resource mapping graph and static descriptor values
  uint64_t pipelineLayoutApiHash;      ///< Pipeline Layout Api Hash
  PipelineOptions options;             ///< Per pipeline tuning options
  bool unlinked;                       ///< True to build an "unlinked" half-pipeline ELF
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  BinaryData shaderLibrary; ///< SPIR-V library binary data
#endif
  RtState rtState;                 ///< Ray tracing state
  const void *pClientMetadata;     ///< Pointer to (optional) client-defined data to be stored inside the ELF
  size_t clientMetadataSize;       ///< Size (in bytes) of the client-defined data
  UniformConstantMap *pUniformMap; ///< Pointer to the uniform constants map
};

/// Represents output of building a ray tracing pipeline.
struct RayTracingPipelineBuildInfo {
  void *pInstance;                                           ///< Vulkan instance object
  void *pUserData;                                           ///< User data
  OutputAllocFunc pfnOutputAlloc;                            ///< Output buffer allocator
  ICache *cache;                                             ///< ICache, used to search for the compiled shader data
  unsigned deviceIndex;                                      ///< Device index for device group
  unsigned deviceCount;                                      ///< Device count for device group
  unsigned shaderCount;                                      ///< Count of shader info
  PipelineShaderInfo *pShaders;                              ///< An array of shader info
  ResourceMappingData resourceMapping;                       ///< Resource mapping graph and static descriptor values
  uint64_t pipelineLayoutApiHash;                            ///< Pipeline Layout Api Hash
  unsigned shaderGroupCount;                                 ///< Count of shader group
  const VkRayTracingShaderGroupCreateInfoKHR *pShaderGroups; ///< An array of shader group
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  BinaryData shaderTraceRay; ///< Trace-ray SPIR-V binary data
#endif
  PipelineOptions options;        ///< Per pipeline tuning options
  unsigned maxRecursionDepth;     ///< Ray tracing max recursion depth
  unsigned indirectStageMask;     ///< Ray tracing indirect stage mask
  LlpcRaytracingMode mode;        ///< Ray tracing compiling mode
  RtState rtState;                ///< Ray tracing state
  unsigned payloadSizeMaxInLib;   ///< Pipeline library maxPayloadSize
  unsigned attributeSizeMaxInLib; ///< Pipeline library maxAttributeSize
  bool hasPipelineLibrary;        ///< Whether include pipeline library
  unsigned pipelineLibStageMask;  ///< Pipeline library stage mask
  bool isReplay;                  ///< Pipeline is created for replaying
  const void *pClientMetadata;    ///< Pointer to (optional) client-defined data to be
                                  ///  stored inside the ELF
  size_t clientMetadataSize;      ///< Size (in bytes) of the client-defined data
};

/// Ray tracing max shader name length
static constexpr unsigned RayTracingMaxShaderNameLength = 16;

/// Raytracing invalid shader ID
static constexpr uint64_t RayTracingInvalidShaderId = 0;

/// Represents the property for a single ray-tracing shader
struct RayTracingShaderProperty {
  uint64_t shaderId;                        ///< Ray tracing compiled shader ID
  char name[RayTracingMaxShaderNameLength]; ///< Ray tracing compiled shader name
  bool hasTraceRay;                         ///< Whether TraceRay() is used
  bool onlyGpuVaLo;                         ///< Whether shader identifier LSB metadata is applied
  uint64_t shaderIdExtraBits;               ///< Raytracing shader identifier extra bits
};

/// Represents ray-tracing shader identifier.
struct RayTracingShaderIdentifier {
  uint64_t shaderId;       ///< Generic shader ID for RayGen, ClosestHit, Miss, and Callable
  uint64_t anyHitId;       ///< AnyHit ID for hit groups
  uint64_t intersectionId; ///< Intersection shader ID for hit groups
  uint64_t padding;        ///< Padding to meet 32-byte api requirement and 8-byte alignment for descriptor table offset
};

/// Values to be bitwise OR'd into the result of the mapping procedure applied to shader IDs.
///
/// The compiler may use this to encode additional metadata into bits that are otherwise unused, e.g. LSBs that are
/// available based on alignment, or in the high 32 bits.
struct RayTracingShaderIdentifierExtraBits {
  uint64_t shader;
  uint64_t anyHit;
  uint64_t intersection;
};

/// Represents ray-tracing capture replay GPU VA remapping table entry
struct RayTracingCaptureReplayVaMappingEntry {
  uint64_t capturedGpuVa; ///< GPU VA that is generated when capturing
  uint64_t replayGpuVa;   ///< GPU VA that is in used when replaying
};

/// Represents the handles of shader group for ray-tracing pipeline
struct RayTracingShaderGroupHandle {
  unsigned shaderHandleCount;                ///< Count of shader group handle array
  RayTracingShaderIdentifier *shaderHandles; ///< Shader group handle array
};

/// Represents a set of ray-tracing shaders referenced by a ray-tracing pipeline
struct RayTracingShaderPropertySet {
  unsigned shaderCount;                  ///< Shader count
  unsigned traceRayIndex;                ///< Index of TraceRay() shader in properties array
  RayTracingShaderProperty *shaderProps; ///< The bunch of shaders in a ray-tracing pipeline
};

// =====================================================================================================================
/// Represents the unified of a pipeline create info.
struct PipelineBuildInfo {
  const ComputePipelineBuildInfo *pComputeInfo;       // Compute pipeline create info
  const GraphicsPipelineBuildInfo *pGraphicsInfo;     // Graphic pipeline create info
  const RayTracingPipelineBuildInfo *pRayTracingInfo; // Ray tracing pipeline create info
};

// =====================================================================================================================
/// Represents the interfaces of a pipeline dumper.
class IPipelineDumper {
public:
  /// Dumps SPIR-V shader binary to external file.
  ///
  /// @param [in]  pDumpDir     Directory of pipeline dump
  /// @param [in]  pSpirvBin    SPIR-V binary
  static void VKAPI_CALL DumpSpirvBinary(const char *pDumpDir, const BinaryData *pSpirvBin);

  /// Begins to dump graphics/compute pipeline info.
  ///
  /// @param [in]  pDumpDir                 Directory of pipeline dump
  /// @param [in]  pipelineInfo             Info of the pipeline to be built
  ///
  /// @returns : The handle of pipeline dump file
  static void *VKAPI_CALL BeginPipelineDump(const PipelineDumpOptions *pDumpOptions, PipelineBuildInfo pipelineInfo);

  /// Begins to dump graphics/compute pipeline info.
  ///
  /// @param [in]  pDumpDir                 Directory of pipeline dump
  /// @param [in]  pipelineInfo             Info of the pipeline to be built
  /// @param hash64                         Hash code
  ///
  /// @returns : The handle of pipeline dump file
  static void *VKAPI_CALL BeginPipelineDump(const PipelineDumpOptions *pDumpOptions, PipelineBuildInfo pipelineInfo,
                                            uint64_t hash64);

  /// Ends to dump graphics/compute pipeline info.
  ///
  /// @param  [in]  pDumpFile         The handle of pipeline dump file
  static void VKAPI_CALL EndPipelineDump(void *pDumpFile);

  /// Disassembles pipeline binary and dumps it to pipeline info file.
  ///
  /// @param [in]  pDumpFile        The handle of pipeline dump file
  /// @param [in]  gfxIp            Graphics IP version info
  /// @param [in]  pPipelineBin     Pipeline binary (ELF)
  static void VKAPI_CALL DumpPipelineBinary(void *pDumpFile, GfxIpVersion gfxIp, const BinaryData *pPipelineBin);

  /// Dump extra info to pipeline file.
  ///
  /// @param [in]  pDumpFile        The handle of pipeline dump file
  /// @param [in]  pStr             Extra string info to dump
  static void VKAPI_CALL DumpPipelineExtraInfo(void *pDumpFile, const char *pStr);

  /// Gets shader module hash code.
  ///
  /// @param [in]  pModuleData   Pointer to the shader module data.
  ///
  /// @returns : Hash code associated this shader module.
  static uint64_t VKAPI_CALL GetShaderHash(const void *pModuleData);

  /// Calculates graphics shader binary hash code.
  ///
  /// @param [in]  pPipelineInfo  Info to build this partial graphics pipeline
  /// @param [in]  stage          The shader stage for which the code is calculated
  ///
  /// @returns : Hash code associated to this shader binary compilation
  static uint64_t VKAPI_CALL GetGraphicsShaderBinaryHash(const GraphicsPipelineBuildInfo *pPipelineInfo,
                                                         ShaderStage stage);

  /// Calculates graphics pipeline hash code.
  ///
  /// @param [in]  pPipelineInfo  Info to build this graphics pipeline
  ///
  /// @returns : Hash code associated this graphics pipeline.
  static uint64_t VKAPI_CALL GetPipelineHash(const GraphicsPipelineBuildInfo *pPipelineInfo);

  /// Calculates compute pipeline hash code.
  ///
  /// @param [in]  pPipelineInfo  Info to build this compute pipeline
  ///
  /// @returns : Hash code associated this compute pipeline.
  static uint64_t VKAPI_CALL GetPipelineHash(const ComputePipelineBuildInfo *pPipelineInfo);

  /// Gets graphics pipeline name.
  ///
  /// @param [in]  pPipelineInfo  Info to build this graphics pipeline
  /// @param [out] pPipeName : The full name of this graphics pipeline
  /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
  static void VKAPI_CALL GetPipelineName(const GraphicsPipelineBuildInfo *pPipelineInfo, char *pPipeName,
                                         const size_t nameBufSize);

  /// Gets compute pipeline name.
  ///
  /// @param [in]  pPipelineInfo  Info to build this compute pipeline
  /// @param [out] pPipeName : The full name of this compute pipeline
  /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
  static void VKAPI_CALL GetPipelineName(const ComputePipelineBuildInfo *pPipelineInfo, char *pPipeName,
                                         const size_t nameBufSize);

  /// Gets graphics pipeline name.
  ///
  /// @param [in]  pPipelineInfo   Info to build this graphics pipeline
  /// @param [out] pPipeName       The full name of this graphics pipeline
  /// @param [in]  nameBufSize     Size of the buffer to store pipeline name
  /// @param hashCode64            Precalculated Hash code of pipeline
  static void VKAPI_CALL GetPipelineName(const GraphicsPipelineBuildInfo *pPipelineInfo, char *pPipeName,
                                         const size_t nameBufSize, uint64_t hashCode64);

  /// Gets compute pipeline name.
  ///
  /// @param [in]  pPipelineInfo  Info to build this compute pipeline
  /// @param [out] pPipeName      The full name of this compute pipeline
  /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
  /// @param hashCode64           Precalculated Hash code of pipeline
  static void VKAPI_CALL GetPipelineName(const ComputePipelineBuildInfo *pPipelineInfo, char *pPipeName,
                                         const size_t nameBufSize, uint64_t hashCode64);

  /// Calculates ray tracing pipeline hash code.
  ///
  /// @param [in]  pPipelineInfo  Info to build this ray tracing pipeline
  ///
  /// @returns Hash code associated this ray tracing pipeline.
  static uint64_t VKAPI_CALL GetPipelineHash(const RayTracingPipelineBuildInfo *pPipelineInfo);

  /// Gets ray tracing pipeline name.
  ///
  /// @param [in]  pPipelineInfo  Info to build this ray tracing pipeline
  /// @param [out] pPipeName      The full name of this ray tracing pipeline
  /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
  static void VKAPI_CALL GetPipelineName(const RayTracingPipelineBuildInfo *pPipelineInfo, char *pPipeName,
                                         const size_t nameBufSize);

  /// Gets ray tracing pipeline name.
  ///
  /// @param [in]  pPipelineInfo  Info to build this ray tracing pipeline
  /// @param [out] pPipeName      The full name of this ray tracing pipeline
  /// @param [in]  nameBufSize    Size of the buffer to store pipeline name
  /// @param hashCode64           Precalculated Hash code of pipeline
  static void VKAPI_CALL GetPipelineName(const RayTracingPipelineBuildInfo *pPipelineInfo, char *pPipeName,
                                         const size_t nameBufSize, uint64_t hashCode64);

  /// Dumps ray tracing pipeline metadata.
  ///
  /// @param [in]  dumpFile      The handle of pipeline dump file
  /// @param [in]  pipelineMeta   Ray tracing pipeline metadata binary
  static void VKAPI_CALL DumpRayTracingPipelineMetadata(void *dumpFile, BinaryData *pipelineMeta);
};

// =====================================================================================================================
/// Represents the interfaces of the utility.
class IUtil {
public:
  /// Gets the entry-point name from the SPIR-V binary.
  ///
  /// @param [in] spvBin   SPIR-V binary
  static const char *VKAPI_CALL GetEntryPointNameFromSpirvBinary(const BinaryData *spvBin);
};

/// 128-bit hash compatible structure
struct HashId {
  union {
    uint64_t qwords[2]; ///< Output hash in qwords.
    uint32_t dwords[4]; ///< Output hash in dwords.
    uint8_t bytes[16];  ///< Output hash in bytes.
  };
};

typedef void *RawEntryHandle;

// =====================================================================================================================
// Shader Cache interfaces, that client needs to inherit and implement it.
class ICache {
public:
  virtual ~ICache() = default;

  /// \brief Obtain a cache entry for the \p hash.
  ///
  /// The caller receives reference-counted ownership of the returned handle, if any, and must
  /// eventually call \ref PutEntry to release it.
  ///
  /// Valid handles are always non-null.
  ///
  /// @param hash : The hash key for the cache entry
  /// @param allocateOnMiss : If true, a new cache entry will be allocated when none is found
  /// @param pHandle : Will be filled with a handle to the cache entry on Success, NotReady and allocateOnMiss if
  /// NotFound
  /// @returns : Success code, possible values:
  ///   * Success: an existing, ready entry was found
  ///   * NotReady: an existing entry was found, but it is not ready yet because another thread
  ///               is working on filling it
  ///   * NotFound: no existing entry was found. If \p allocateOnMiss is set, a new entry was
  ///               allocated and the caller must populate it via SetValue
  ///   * ErrorXxx: some internal error has occurred, no handle is returned
  LLPC_NODISCARD virtual Result GetEntry(HashId hash, bool allocateOnMiss, EntryHandle *pHandle) = 0;

  /// \brief Release ownership of a handle to a cache entry.
  ///
  /// If the handle owner is responsible for populating the cache entry, it is an error to call this
  /// method without first calling SetValue.
  /// Put can be called multiple times if the Entry is empty.
  ///
  /// @param rawHandle : The handle to cache entry to be released
  virtual void ReleaseEntry(RawEntryHandle rawHandle) = 0;

  /// \brief Wait for a cache entry to become ready (populated by another thread).
  ///
  /// This block the current thread until the entry becomes ready.
  ///
  /// @param rawHandle : The handle to the cache entry to be wait.
  /// @returns : Success code, possible values:
  ///   * Success: the entry is now ready
  ///   * ErrorXxx: some internal error has occurred, or populating the cache was not successful
  ///               (e.g. due to a compiler error). The operation was semantically a no-op:
  ///               the entry is still not ready, and the caller must still release it via \ref PutEntry
  LLPC_NODISCARD virtual Result WaitForEntry(RawEntryHandle rawHandle) = 0;

  /// \brief Retrieve the value contents of a cache entry.
  ///
  /// @param rawHandle : The handle to the cache entry
  /// @param pData : If non-null, up to *pDataLen bytes of contents of the cache entry will be copied
  ///              into the memory pointed to by pData
  /// @param pDataLen : If \p pData is non-null, the caller must set *pDataLen to the space available
  ///                 in the memory that it points to. The method will store the total size of the
  ///                 cache entry in *pDataLen.
  /// @returns : Success code, possible values:
  ///   * Success: operation completed successfully
  ///   * NotReady: the entry is not ready yet (waiting to be populated by another thread)
  ///   * ErrorXxx: some internal error has occurred. The operation was semantically a no-op.
  LLPC_NODISCARD virtual Result GetValue(RawEntryHandle rawHandle, void *pData, size_t *pDataLen) = 0;

  /// \brief Zero-copy retrieval of the value contents of a cache entry.
  ///
  /// @param handle : The handle to the cache entry
  /// @param ppData : Will be set to a pointer to the cache value contents. The pointer remains
  ///               valid until the handle is released via \ref PutEntry.
  /// @param pDataLen : Will be set to the total size of the cache entry in *pDataLen.
  /// @returns : Success code, possible values:
  ///   * Success: operation completed successfully
  ///   * Unsupported: this implementation does not support zero-copy, the caller must use
  ///                  \ref GetValue instead
  ///   * NotReady: the entry is not ready yet (waiting to be populated by another thread)
  ///   * ErrorXxx: some internal error has occurred. The operation was semantically a no-op.
  LLPC_NODISCARD virtual Result GetValueZeroCopy(RawEntryHandle rawHandle, const void **ppData, size_t *pDataLen) = 0;

  /// \brief Populate the value contents of a cache entry.
  ///
  /// This method must be called exactly once when a cache entry is newly allocated by \ref GetEntry
  /// with allocateOnMiss set and a return value of NotFound.
  ///
  /// The handle must still be released using \ref PutEntry after calling this method.
  ///
  /// @param handle : The handle to be populated
  /// @param success : Whether computing the value contents was successful
  /// @param pData : Pointer to the value contents
  /// @param dataLen : Size of the value contents in bytes
  /// @returns : Success code, possible values:
  ///   * Success: operation completed successfully
  ///   * ErrorXxx: some internal error has occurred. The caller must not call SetValue again,
  ///               but it must still release the handle via \ref PutEntry.
  LLPC_NODISCARD virtual Result SetValue(RawEntryHandle rawHandle, bool success, const void *pData, size_t dataLen) = 0;

  /// \brief Populate the value contents of a cache entry and release the handle.
  ///
  /// Semantics are identical to SetValue, except that the handle is guaranteed to be released.
  /// Doing this atomically can sometimes allow a more efficient implementation; the default
  /// implementation is trivial.
  LLPC_NODISCARD virtual Result ReleaseWithValue(RawEntryHandle rawHandle, bool success, const void *pData,
                                                 size_t dataLen) {
    Result result = Result::ErrorUnknown;
    if (!rawHandle)
      return result;
    result = SetValue(rawHandle, success, pData, dataLen);
    ReleaseEntry(rawHandle);
    return result;
  }
};

// =====================================================================================================================
// A slight alternative using more C++ RAII safety:
// - make all methods of ICache other than GetEntry protected
// - use this class for the EntryHandle, providing more type safety and using ~EntryHandle
//   to ensure that PutEntry gets called etc.
class EntryHandle {
public:
  /// \brief Construct from a raw handle.
  EntryHandle(ICache *pCache, RawEntryHandle rawHandle, bool mustPopulate) {
    m_pCache = pCache;
    m_rawHandle = rawHandle;
    m_mustPopulate = mustPopulate;
  }

  EntryHandle() = default;
  ~EntryHandle() { Put(); }
  EntryHandle(const EntryHandle &) = delete;
  EntryHandle &operator=(const EntryHandle &) = delete;

  EntryHandle(EntryHandle &&rhs)
      : m_pCache(rhs.m_pCache), m_rawHandle(rhs.m_rawHandle), m_mustPopulate(rhs.m_mustPopulate) {
    rhs.m_pCache = nullptr;
    rhs.m_rawHandle = nullptr;
    rhs.m_mustPopulate = false;
  }

  EntryHandle &operator=(EntryHandle &&rhs) {
    if (this != &rhs) {
      m_pCache = rhs.m_pCache;
      m_rawHandle = rhs.m_rawHandle;
      m_mustPopulate = rhs.m_mustPopulate;

      rhs.m_pCache = nullptr;
      rhs.m_rawHandle = nullptr;
      rhs.m_mustPopulate = false;
    }

    return *this;
  }

  static void ReleaseHandle(EntryHandle &&rhs) {
    EntryHandle entryHandle;
    entryHandle.m_pCache = rhs.m_pCache;
    entryHandle.m_rawHandle = rhs.m_rawHandle;
    entryHandle.m_mustPopulate = rhs.m_mustPopulate;

    rhs.m_pCache = nullptr;
    rhs.m_rawHandle = nullptr;
    rhs.m_mustPopulate = false;

    entryHandle.Put();
  }
  bool IsEmpty() { return m_pCache == nullptr; }

  // semantics of these methods are largely analogous to the above in ICache,
  // their implementation simply forwards to the m_pCache.
  LLPC_NODISCARD Result WaitForEntry() const {
    assert(m_pCache);
    return m_pCache->WaitForEntry(m_rawHandle);
  }

  LLPC_NODISCARD Result GetValue(void *pData, size_t *pDataLen) const {
    assert(m_pCache);
    return m_pCache->GetValue(m_rawHandle, pData, pDataLen);
  }

  LLPC_NODISCARD Result GetValueZeroCopy(const void **ppData, size_t *pDataLen) const {
    assert(m_pCache);
    return m_pCache->GetValueZeroCopy(m_rawHandle, ppData, pDataLen);
  }

  LLPC_NODISCARD Result SetValue(bool success, const void *pData, size_t dataLen) {
    assert(m_pCache);
    assert(m_mustPopulate);
    m_mustPopulate = false;
    return m_pCache->SetValue(m_rawHandle, success, pData, dataLen);
  }

private:
  void Put() {
    if (!m_pCache)
      return;
    if (m_mustPopulate) {
      Result result = m_pCache->SetValue(m_rawHandle, false, nullptr, 0);
      assert(result == Result::Success);
      (void)result;
    }
    m_pCache->ReleaseEntry(m_rawHandle);
    m_pCache = nullptr;
    m_rawHandle = nullptr;
    m_mustPopulate = false;
  }

  ICache *m_pCache = nullptr;
  void *m_rawHandle = nullptr;
  bool m_mustPopulate = false;
};

} // namespace Vkgc
