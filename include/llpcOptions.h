/*
 ***********************************************************************************************************************
 *
 *  Trade secret of Advanced Micro Devices, Inc.
 *  Copyright (c) 2019, Advanced Micro Devices, Inc., (unpublished)
 *
 *  All rights reserved. This notice is intended as a precaution against inadvertent publication and does not imply
 *  publication or any waiver of confidentiality. The year included in the foregoing notice is the year of creation of
 *  the work.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcOptions.h
 * @brief LLPC header file: contains LLPC options
 ***********************************************************************************************************************
 */

// ---------------------------------------------------------------------------------------------------------------------
// Per pipeline options that make up PipelineOptions.

// If set, the disassembly for all compiled shaders will be included in the pipeline ELF.
PIPELINE_OPT(bool, includeDisassembly)

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 30
// If set, the LLPC standalone compiler is compiling individual shader(s) without pipeline info, so LLPC needs
// to do auto descriptor layout.
PIPELINE_OPT(bool, autoLayoutDesc)
#endif

// If set, allows scalar block layout of types.
PIPELINE_OPT(bool, scalarBlockLayout)

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
// If set, allows automatic workgroup reconfigure to take place on compute shaders.
PIPELINE_OPT(bool, reconfigWorkgroupLayout)
#endif

// If set, the IR for all compiled shaders will be included in the pipeline ELF.
PIPELINE_OPT(bool, includeIr)

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 23
// If set, out of bounds accesses to buffer or private array will be handled.
//  for now this option is used by LLPC shader and affects only the private array,
//  the out of bounds accesses will be skipped with this setting.
PIPELINE_OPT(bool, robustBufferAccess)
#endif

#if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 25) && (LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 27)
// If set, the IR binary for all compiled shaders will be included in the pipeline ELF.
PIPELINE_OPT(bool, includeIrBinary)
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Per shader stage options that make up PipelineShaderOptions

// Indicates a trap handler will be present when this pipeline is executed,
// and any trap conditions encountered in this shader should call the trap
// handler. This could include an arithmetic exception, an explicit trap
// request from the host, or a trap after every instruction when in debug
// mode.
PIPELINESHADER_OPT(bool, trapPresent)

// When set, this shader should cause the trap handler to be executed after
// every instruction.  Only valid if trapPresent is set.
PIPELINESHADER_OPT(bool, debugMode)

// Enables the compiler to generate extra instructions to gather
// various performance-related data.
PIPELINESHADER_OPT(bool, enablePerformanceData)

// Allow the DB ReZ feature to be enabled.  This will cause an early-Z test
// to potentially kill PS waves before launch, and also issues a late-Z test
// in case the PS kills pixels.  Only valid for pixel shaders.
PIPELINESHADER_OPT(bool, allowReZ)

// Maximum VGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
// of this value and whatever the target GPU supports. To effectively disable this limit, set this to UINT_MAX.
PIPELINESHADER_OPT(uint32_t, vgprLimit)

// Maximum SGPR limit for this shader. The actual limit used by back-end for shader compilation is the smaller
// of this value and whatever the target GPU supports. To effectively disable this limit, set this to UINT_MAX.
PIPELINESHADER_OPT(uint32_t, sgprLimit)

// Overrides the number of CS thread-groups which the GPU will launch per compute-unit. This throttles the
// shader, which can sometimes enable more graphics shader work to complete in parallel. A value of zero
// disables limiting the number of thread-groups to launch. This field is ignored for graphics shaders.
PIPELINESHADER_OPT(uint32_t, maxThreadGroupsPerComputeUnit)

#if LLPC_BUILD_GFX10
// Control the number of threads per wavefront (GFX10+)
PIPELINESHADER_OPT(uint32_t, waveSize)

// Whether to choose WGP mode or CU mode (GFX10+)
PIPELINESHADER_OPT(bool, wgpMode)

// Size of region to force the end of a wavefront (GFX10+).
//  Only valid for fragment shaders.
PIPELINESHADER_OPT(WaveBreakSize, waveBreakSize)
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 24
// Force loop unroll count. "0" means using default value; "1" means disabling loop unroll.
PIPELINESHADER_OPT(uint32_t, forceLoopUnrollCount)
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
// Enable LLPC load scalarizer optimization.
PIPELINESHADER_OPT(bool, enableLoadScalarizer)
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 31
// If set, lets the pipeline vary the wave sizes.
PIPELINESHADER_OPT(bool, allowVaryWaveSize)
#elif VKI_EXT_SUBGROUP_SIZE_CONTROL
// If set, lets the pipeline vary the wave sizes.
PIPELINESHADER_OPT(bool, allowVaryWaveSize)
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
// Use the LLVM backend's SI scheduler instead of the default scheduler.
PIPELINESHADER_OPT(bool, useSiScheduler)
#endif

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 35
// Disable the the LLVM backend's LICM pass.
PIPELINESHADER_OPT(bool, disableLicm)
#endif

#if LLPC_BUILD_GFX10
// ---------------------------------------------------------------------------------------------------------------------
// NGG tuning options that make up NggState

// Enable NGG mode, use an implicit primitive shader
NGGSTATE_OPT(bool, enableNgg)

// Enable NGG use on geometry shader
NGGSTATE_OPT(bool, enableGsUse)

// Force NGG to run in non pass-through mode
NGGSTATE_OPT(bool, forceNonPassthrough)

// Always use primitive shader table to fetch culling-control registers
NGGSTATE_OPT(bool, alwaysUsePrimShaderTable)

// Compaction mode after culling operations
NGGSTATE_OPT(NggCompactMode, compactMode)

// Enable the hardware to launch subgroups of work at a faster rate
NGGSTATE_OPT(bool, enableFastLaunch)

// Enable optimization to cull duplicate vertices
NGGSTATE_OPT(bool, enableVertexReuse)

// Enable culling of primitives that don't meet facing criteria
NGGSTATE_OPT(bool, enableBackfaceCulling)

// Enable discarding of primitives outside of view frustum
NGGSTATE_OPT(bool, enableFrustumCulling)

// Enable simpler frustum culler that is less accurate
NGGSTATE_OPT(bool, enableBoxFilterCulling)

// Enable frustum culling based on a sphere
NGGSTATE_OPT(bool, enableSphereCulling)

// Enable trivial sub-sample primitive culling
NGGSTATE_OPT(bool, enableSmallPrimFilter)

// Enable culling when "cull distance" exports are present
NGGSTATE_OPT(bool, enableCullDistanceCulling)

// Value from 1 to UINT32_MAX that will cause the backface culling
//  algorithm to ignore area calculations that are less than
//  (10 ^ -(backfaceExponent)) / abs(w0 * w1 * w2)
//  Only valid if the NGG backface culler is enabled.
//  A value of 0 will disable the threshold.
NGGSTATE_OPT(uint32_t, backfaceExponent)

// NGG sub-group sizing type
NGGSTATE_OPT(NggSubgroupSizingType, subgroupSizing)

// Preferred number of GS primitives to pack into a primitive shader sub-group
NGGSTATE_OPT(uint32_t, primsPerSubgroup)

// Preferred number of vertices consumed by a primitive shader sub-group
NGGSTATE_OPT(uint32_t, vertsPerSubgroup)

#endif
