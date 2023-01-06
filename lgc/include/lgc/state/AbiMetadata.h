/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

static constexpr char AmdGpuVendorName[] = "AMD";  // Vendor name string for .note record.
static constexpr char AmdGpuArchName[] = "AMDGPU"; // Architecture name string for .note record.

// Pipeline category.
enum PipelineType : unsigned {
  VsPs = 0,
  Gs,
  Cs,
  Ngg,
  Tess,
  GsTess,
  NggTess,
  Mesh,
  TaskMesh,
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
static constexpr char XglCacheInfo[] = ".xgl_cache_info";
static constexpr char CacheHash128Bits[] = ".128_bit_cache_hash";
static constexpr char LlpcVersion[] = ".llpc_version";
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
static constexpr char StreamOutVertexStrides[] = ".streamout_vertex_strides";
static constexpr char Api[] = ".api";
static constexpr char ApiCreateInfo[] = ".api_create_info";
static constexpr char PsSampleMask[] = ".ps_sample_mask";
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

} // namespace Abi

} // namespace Util

// User data mapping for special user data values.
enum class UserDataMapping : unsigned {
  GlobalTable = 0x10000000,          // 32-bit pointer to GPU memory containing the global internal table.
  PerShaderTable = 0x10000001,       // 32-bit pointer to GPU memory containing the per-shader internal table.
  SpillTable = 0x10000002,           // 32-bit pointer to GPU memory containing the user data spill table.  See User
                                     //  Data Spilling.
  BaseVertex = 0x10000003,           // Vertex offset (32-bit unsigned integer). Only supported by the first stage in a
                                     //  graphics pipeline.
  BaseInstance = 0x10000004,         // Instance offset (32-bit unsigned integer). Only supported by the first stage in
                                     //  a graphics pipeline.
  DrawIndex = 0x10000005,            // Draw index (32-bit unsigned integer). Only supported by the first stage in a
                                     //  graphics pipeline.
  Workgroup = 0x10000006,            // Thread group count (32-bit unsigned integer). Only supported by compute
                                     //  pipelines.
  EsGsLdsSize = 0x1000000A,          // Indicates that PAL will program this user-SGPR to contain the amount of LDS
                                     //  space used for the ES/GS pseudo-ring-buffer for passing data between shader
                                     //  stages.
  ViewId = 0x1000000B,               // View id (32-bit unsigned integer) identifies a view of graphic
                                     //  pipeline instancing.
  StreamOutTable = 0x1000000C,       // 32-bit pointer to GPU memory containing the stream out target SRD table.  This
                                     //  can only appear for one shader stage per pipeline.
  VertexBufferTable = 0x1000000F,    // 32-bit pointer to GPU memory containing the vertex buffer SRD table.  This can
                                     //  only appear for one shader stage per pipeline.
  NggCullingData = 0x10000011,       // 64-bit pointer to GPU memory containing the hardware register data needed by
                                     //  some NGG pipelines to perform culling.  This value contains the address of the
                                     //  first of two consecutive registers which provide the full GPU address.
  MeshTaskDispatchDims = 0x10000012, // Offset to three consecutive registers which indicate the number of
                                     //  threadgroups dispatched in the X, Y, and Z dimensions.
  MeshTaskRingIndex = 0x10000013,    // Index offset (32-bit unsigned integer). Indicates the index into the
                                     //  Mesh/Task shader rings for the shader to consume.
  MeshPipeStatsBuf = 0x10000014,     // 32-bit GPU virtual address of a buffer storing the shader-emulated mesh
                                     //  pipeline stats query.
  StreamOutControlBuf = 0x10000016,  // 32-bit GPU virtual address to the streamout control buffer for GPUs that
                                     // use SW-emulated streamout.

  // Values used in a user data PAL metadata register to be resolved at link time.
  // This is part of the "unlinked" ABI, so should arguably be in AbiUnlinked.h.
  DescriptorSet0 = 0x80000000,   // 32-bit pointer to the descriptor table for descriptor set 0: add N to this value
                                 //  for descriptor set N
  DescriptorSetMax = 0x800000FF, // Max descriptor set
  PushConst0 = 0x80000100,       // Push constant dword 0: add N to this value for push constant dword N
  PushConstMax = 0x800001FF,     // Max push constant dword

  // Value used internally in LGC.
  Invalid = ~0U // Invalid value used internally in LGC.
};

// An enumeration of shader export formats.
typedef enum SPI_SHADER_EX_FORMAT {
  SPI_SHADER_ZERO = 0x00000000,
  SPI_SHADER_32_R = 0x00000001,
  SPI_SHADER_32_GR = 0x00000002,
  SPI_SHADER_32_AR = 0x00000003,
  SPI_SHADER_FP16_ABGR = 0x00000004,
  SPI_SHADER_UNORM16_ABGR = 0x00000005,
  SPI_SHADER_SNORM16_ABGR = 0x00000006,
  SPI_SHADER_UINT16_ABGR = 0x00000007,
  SPI_SHADER_SINT16_ABGR = 0x00000008,
  SPI_SHADER_32_ABGR = 0x00000009,
} SPI_SHADER_EX_FORMAT;

// The names of API shader stages used in PAL metadata, in ShaderStage order.
static const char *const ApiStageNames[] = {".task",     ".vertex", ".hull",  ".domain",
                                            ".geometry", ".mesh",   ".pixel", ".compute"};

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

// The RSRC1 registers.
constexpr unsigned mmSPI_SHADER_PGM_RSRC1_LS = 0x2D4A;
constexpr unsigned mmSPI_SHADER_PGM_RSRC1_HS = 0x2D0A;
constexpr unsigned mmSPI_SHADER_PGM_RSRC1_ES = 0x2CCA;
constexpr unsigned mmSPI_SHADER_PGM_RSRC1_GS = 0x2C8A;
constexpr unsigned mmSPI_SHADER_PGM_RSRC1_VS = 0x2C4A;
constexpr unsigned mmSPI_SHADER_PGM_RSRC1_PS = 0x2C0A;
constexpr unsigned mmCOMPUTE_PGM_RSRC1 = 0x2E12;

// RSRC2 register. We only specify one, as each graphics shader stage has its RSRC2 register at the same
// offset (-1) from its USER_DATA_*_0 register.
constexpr unsigned mmSPI_SHADER_PGM_RSRC2_VS = 0x2C4B;

// Other SPI register numbers in PAL metadata
constexpr unsigned int mmPA_CL_CLIP_CNTL = 0xA204;
constexpr unsigned mmVGT_SHADER_STAGES_EN = 0xA2D5;
constexpr unsigned mmSPI_SHADER_COL_FORMAT = 0xA1C5;
constexpr unsigned mmDB_SHADER_CONTROL = 0xA203;
constexpr unsigned mmSPI_SHADER_Z_FORMAT = 0xA1C4;
constexpr unsigned mmCB_SHADER_MASK = 0xA08F;

// PS register numbers in PAL metadata
constexpr unsigned mmSPI_PS_INPUT_CNTL_0 = 0xA191; // NOLINT
constexpr unsigned mmSPI_PS_INPUT_ENA = 0xA1B3;
constexpr unsigned mmSPI_PS_INPUT_ADDR = 0xA1B4;
constexpr unsigned mmSPI_PS_IN_CONTROL = 0xA1B6;
constexpr unsigned mmPA_SC_SHADER_CONTROL = 0xA310;
constexpr unsigned mmPA_SC_AA_CONFIG = 0xA2F8;

// GS register numbers in PAL metadata
constexpr unsigned mmVGT_GS_OUT_PRIM_TYPE = 0xA29B;

// Register bitfield layout.

// General RSRC1 register, enough to get the VGPR and SGPR counts.
union SPI_SHADER_PGM_RSRC1 {
  struct {
    unsigned int VGPRS : 6;
    unsigned int SGPRS : 4;
    unsigned int : 2;
    unsigned int FLOAT_MODE : 8;
    unsigned int : 12;
  } bits, bitfields;
  unsigned int u32All;
};

// General RSRC2 register, enough to get the user SGPR count.
union SPI_SHADER_PGM_RSRC2 {
  struct {
    unsigned int : 1;
    unsigned int USER_SGPR : 5;
    unsigned int : 26;
  } bits;
  unsigned int u32All;
};

// PA_CL_CLIP_CNTL register
union PA_CL_CLIP_CNTL {
  struct {
    unsigned int UCP_ENA_0 : 1;
    unsigned int UCP_ENA_1 : 1;
    unsigned int UCP_ENA_2 : 1;
    unsigned int UCP_ENA_3 : 1;
    unsigned int UCP_ENA_4 : 1;
    unsigned int UCP_ENA_5 : 1;
    unsigned int : 7;
    unsigned int PS_UCP_Y_SCALE_NEG : 1;
    unsigned int PS_UCP_MODE : 2;
    unsigned int CLIP_DISABLE : 1;
    unsigned int UCP_CULL_ONLY_ENA : 1;
    unsigned int BOUNDARY_EDGE_FLAG_ENA : 1;
    unsigned int DX_CLIP_SPACE_DEF : 1;
    unsigned int DIS_CLIP_ERR_DETECT : 1;
    unsigned int VTX_KILL_OR : 1;
    unsigned int DX_RASTERIZATION_KILL : 1;
    unsigned int : 1;
    unsigned int DX_LINEAR_ATTR_CLIP_ENA : 1;
    unsigned int VTE_VPORT_PROVOKE_DISABLE : 1;
    unsigned int ZCLIP_NEAR_DISABLE : 1;
    unsigned int ZCLIP_FAR_DISABLE : 1;
    unsigned int ZCLIP_PROG_NEAR_ENA : 1; // GFX9+
    unsigned int : 3;
  } bits, bitfields;
  unsigned int u32All;
};

// VGT_SHADER_STAGES_EN register (just the GFX10 wave32 enable bits)
union VGT_SHADER_STAGES_EN {
  struct {
    unsigned int : 21;
    unsigned int HS_W32_EN : 1;
    unsigned int GS_W32_EN : 1;
    unsigned int VS_W32_EN : 1;
    unsigned int : 8;
  } gfx10;
  unsigned int u32All;
};

// The DB_SHADER_CONTROL register.
union DB_SHADER_CONTROL {
  struct {
    unsigned int : 6;
    unsigned int KILL_ENABLE : 1;
    unsigned int : 1;
    unsigned int MASK_EXPORT_ENABLE : 1;
    unsigned int : 2;
    unsigned int ALPHA_TO_MASK_DISABLE : 1;
    unsigned int : 20;
  } bitfields, bits;
  unsigned int u32All;
};

union PA_SC_SHADER_CONTROL {
  struct {
    unsigned : 5;
    unsigned WAVE_BREAK_REGION_SIZE : 2;
    unsigned : 25;
  } gfx10;

  unsigned u32All;
};

enum CovToShaderSel {
  INPUT_COVERAGE = 0x00000000,
  INPUT_INNER_COVERAGE = 0x00000001,
  INPUT_DEPTH_COVERAGE = 0x00000002,
  RAW = 0x00000003,
};

union PA_SC_AA_CONFIG {
  struct {
    unsigned : 26;
    unsigned COVERAGE_TO_SHADER_SELECT : 2;
    unsigned : 4;
  } bits, bitfields;

  unsigned u32All;
};

union SPI_PS_INPUT_CNTL_0 {
  struct {
    unsigned OFFSET : 6;              // NOLINT
    unsigned : 2;                     // NOLINT
    unsigned DEFAULT_VAL : 2;         // NOLINT
    unsigned FLAT_SHADE : 1;          // NOLINT
    unsigned : 6;                     // NOLINT
    unsigned PT_SPRITE_TEX : 1;       // NOLINT
    unsigned DUP : 1;                 // NOLINT
    unsigned FP16_INTERP_MODE : 1;    // NOLINT
    unsigned USE_DEFAULT_ATTR1 : 1;   // NOLINT
    unsigned DEFAULT_VAL_ATTR1 : 2;   // NOLINT
    unsigned PT_SPRITE_TEX_ATTR1 : 1; // NOLINT
    unsigned ATTR0_VALID : 1;         // NOLINT
    unsigned ATTR1_VALID : 1;         // NOLINT
    unsigned : 6;                     // NOLINT
  } bits, bitfields;
  unsigned u32All;
};

} // namespace lgc
