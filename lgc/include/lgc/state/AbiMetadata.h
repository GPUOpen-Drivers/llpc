/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  AbiMetadata.h
 * @brief LLPC header file: contains declaration of keys used as PAL ABI metadata
 *
 * This file contains declarations for PAL ABI metadata. (Non-metadata PAL ABI declarations are in Abi.h.)
 * It is a copy of a subset of g_palPipelineAbiMetadata.h in PAL, together with some other PAL metadata
 * related declarations.
 ***********************************************************************************************************************
 */
#pragma once

#include <stdint.h>

namespace lgc {

namespace Util {

namespace Abi {

constexpr unsigned PipelineMetadataMajorVersion = 2; // Pipeline Metadata Major Version
constexpr unsigned PipelineMetadataMinorVersion = 3; // Pipeline Metadata Minor Version

constexpr unsigned PipelineMetadataBase = 0x10000000; // Pipeline Metadata base value to be OR'd with the
                                                      //  PipelineMetadataEntry value when saving to ELF.

// Pipeline category.
enum PipelineType : unsigned {
  VsPs = 0,
  Gs,
  Cs,
  Ngg,
  Tess,
  GsTess,
  NggTess,
};

// Hardware shader stage
enum class HardwareStage : unsigned {
  Ls = 0, ///< Hardware LS stage
  Hs,     ///< Hardware hS stage
  Es,     ///< Hardware ES stage
  Gs,     ///< Hardware GS stage
  Vs,     ///< Hardware VS stage
  Ps,     ///< Hardware PS stage
  Cs,     ///< Hardware CS stage
  Count,
  Invalid = ~0U
};

// Used to represent hardware shader stage.
enum HardwareStageFlagBits : unsigned {
  HwShaderLs = (1 << static_cast<unsigned>(HardwareStage::Ls)),
  HwShaderHs = (1 << static_cast<unsigned>(HardwareStage::Hs)),
  HwShaderEs = (1 << static_cast<unsigned>(HardwareStage::Es)),
  HwShaderGs = (1 << static_cast<unsigned>(HardwareStage::Gs)),
  HwShaderVs = (1 << static_cast<unsigned>(HardwareStage::Vs)),
  HwShaderPs = (1 << static_cast<unsigned>(HardwareStage::Ps)),
  HwShaderCs = (1 << static_cast<unsigned>(HardwareStage::Cs)),
};

namespace PalCodeObjectMetadataKey {
static constexpr char Version[] = "amdpal.version";
static constexpr char Pipelines[] = "amdpal.pipelines";
}; // namespace PalCodeObjectMetadataKey

namespace PipelineMetadataKey {
static constexpr char Name[] = ".name";
static constexpr char Type[] = ".type";
static constexpr char InternalPipelineHash[] = ".internal_pipeline_hash";
static constexpr char Shaders[] = ".shaders";
static constexpr char HardwareStages[] = ".hardware_stages";
static constexpr char Registers[] = ".registers";
static constexpr char UserDataLimit[] = ".user_data_limit";
static constexpr char SpillThreshold[] = ".spill_threshold";
static constexpr char UsesViewportArrayIndex[] = ".uses_viewport_array_index";
static constexpr char EsGsLdsSize[] = ".es_gs_lds_size";
static constexpr char StreamOutTableAddress[] = ".stream_out_table_address";
static constexpr char IndirectUserDataTableAddresses[] = ".indirect_user_data_table_addresses";
static constexpr char NggSubgroupSize[] = ".nggSubgroupSize";
static constexpr char NumInterpolants[] = ".num_interpolants";
static constexpr char CalcWaveBreakSizeAtDrawTime[] = ".calc_wave_break_size_at_draw_time";
static constexpr char Api[] = ".api";
static constexpr char ApiCreateInfo[] = ".api_create_info";
}; // namespace PipelineMetadataKey

namespace HardwareStageMetadataKey {
static constexpr char EntryPoint[] = ".entry_point";
static constexpr char ScratchMemorySize[] = ".scratch_memory_size";
static constexpr char LdsSize[] = ".lds_size";
static constexpr char PerfDataBufferSize[] = ".perf_data_buffer_size";
static constexpr char VgprCount[] = ".vgpr_count";
static constexpr char SgprCount[] = ".sgpr_count";
static constexpr char VgprLimit[] = ".vgpr_limit";
static constexpr char SgprLimit[] = ".sgpr_limit";
static constexpr char ThreadgroupDimensions[] = ".threadgroup_dimensions";
static constexpr char WavefrontSize[] = ".wavefront_size";
static constexpr char UsesUavs[] = ".uses_uavs";
static constexpr char UsesRovs[] = ".uses_rovs";
static constexpr char WritesUavs[] = ".writes_uavs";
static constexpr char WritesDepth[] = ".writes_depth";
static constexpr char UsesAppendConsume[] = ".uses_append_consume";
static constexpr char MaxPrimsPerWave[] = ".max_prims_per_wave";
}; // namespace HardwareStageMetadataKey

namespace ShaderMetadataKey {
static constexpr char ApiShaderHash[] = ".api_shader_hash";
static constexpr char HardwareMapping[] = ".hardware_mapping";
}; // namespace ShaderMetadataKey

/// User data entries can map to physical user data registers.  UserDataMapping describes the
/// content of the registers.
enum class UserDataMapping : unsigned {
  GlobalTable = 0x10000000,       // 32-bit pointer to GPU memory containing the global internal table.
  PerShaderTable = 0x10000001,    // 32-bit pointer to GPU memory containing the per-shader internal table.
  SpillTable = 0x10000002,        // 32-bit pointer to GPU memory containing the user data spill table.  See User
                                  //  Data Spilling.
  BaseVertex = 0x10000003,        // Vertex offset (32-bit unsigned integer). Only supported by the first stage in a
                                  //  graphics pipeline.
  BaseInstance = 0x10000004,      // Instance offset (32-bit unsigned integer). Only supported by the first stage in
                                  //  a graphics pipeline.
  DrawIndex = 0x10000005,         // Draw index (32-bit unsigned integer). Only supported by the first stage in a
                                  //  graphics pipeline.
  Workgroup = 0x10000006,         // Thread group count (32-bit unsigned integer). Only supported by compute
                                  //  pipelines.
  EsGsLdsSize = 0x1000000A,       // Indicates that PAL will program this user-SGPR to contain the amount of LDS
                                  //  space used for the ES/GS pseudo-ring-buffer for passing data between shader
                                  //  stages.
  ViewId = 0x1000000B,            // View id (32-bit unsigned integer) identifies a view of graphic
                                  //  pipeline instancing.
  StreamOutTable = 0x1000000C,    // 32-bit pointer to GPU memory containing the stream out target SRD table.  This
                                  //  can only appear for one shader stage per pipeline.
  VertexBufferTable = 0x1000000F, // 32-bit pointer to GPU memory containing the vertex buffer SRD table.  This can
                                  //  only appear for one shader stage per pipeline.
  NggCullingData = 0x10000011,    // 64-bit pointer to GPU memory containing the hardware register data needed by
                                  //  some NGG pipelines to perform culling.  This value contains the address of the
                                  //  first of two consecutive registers which provide the full GPU address.
  Invalid = ~0U                   // Invalid value used internally in LGC.
};

} // namespace Abi

} // namespace Util

// The names of API shader stages used in PAL metadata, in ShaderStage order.
static const char *const ApiStageNames[] = {".vertex", ".hull", ".domain", ".geometry", ".pixel", ".compute"};

// The names of hardware shader stages used in PAL metadata, in Util::Abi::HardwareStage order.
static const char *const HwStageNames[] = {".ls", ".hs", ".es", ".gs", ".vs", ".ps", ".cs"};

// The name of the metadata node containing PAL metadata. This name is part of the interface from LGC into
// the LLVM AMDGPU back-end when compiling for PAL ABI.
static const char PalMetadataName[] = "amdgpu.pal.metadata.msgpack";

// PAL metadata SPI register numbers for the start of user data.
//
// Note on LS/HS confusion:
// <=GFX8 claims LS registers are from 0x2D4C and HS registers are from 0x2D0C
// GFX9 claims LS registers are from 0x2D0C, and the LS-HS merged shader uses them
// GFX10 claims HS registers are from 0x2D0C, and the LS-HS merged shader uses them.
// So here we call the registers from 0x2D0C "HS" and have the LS-HS merged shader using them, for
// consistency. That contradicts the GFX9 docs, but has the same effect.
//
// First the ones that only apply up to GFX8
constexpr unsigned int mmSPI_SHADER_USER_DATA_LS_0 = 0x2D4C;
// Up to GFX9 only
constexpr unsigned int mmSPI_SHADER_USER_DATA_ES_0 = 0x2CCC; // For GXF9, used for ES-GS merged shader
// Then the ones that apply to all hardware.
constexpr unsigned int mmCOMPUTE_USER_DATA_0 = 0x2E40;
constexpr unsigned int mmSPI_SHADER_USER_DATA_GS_0 = 0x2C8C; // For GFX10, used for ES-GS merged shader and NGG
constexpr unsigned int mmSPI_SHADER_USER_DATA_HS_0 = 0x2D0C; // For GFX9+, Used for LS-HS merged shader
constexpr unsigned int mmSPI_SHADER_USER_DATA_PS_0 = 0x2C0C;
constexpr unsigned int mmSPI_SHADER_USER_DATA_VS_0 = 0x2C4C;

} // namespace lgc
