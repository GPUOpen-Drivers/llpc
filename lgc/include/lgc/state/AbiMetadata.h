/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
constexpr unsigned PipelineMetadataMinorVersion = 6; // Pipeline Metadata Minor Version

// TODO: Remove and update the version to [3,0] after switching to new register metadata layout
constexpr unsigned PipelineMetadataMajorVersionNew = 3; // Pipeline Metadata Major Version
constexpr unsigned PipelineMetadataMinorVersionNew = 0; // Pipeline Metadata Minor Version

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
  Ls = 0, // Hardware LS stage
  Hs,     // Hardware hS stage
  Es,     // Hardware ES stage
  Gs,     // Hardware GS stage
  Vs,     // Hardware VS stage
  Ps,     // Hardware PS stage
  Cs,     // Hardware CS stage
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

// Point sprite override selection.
enum class PointSpriteSelect : unsigned {
  Zero, // Select 0.0f.
  One,  // Select 1.0f.
  S,    // Select S component value.
  T,    // Select T component value.
  None, // Keep interpolated result.
};

// Geometry Shader output primitive type.
enum class GsOutPrimType : unsigned {
  PointList = 0, // A list of individual vertices that make up points.
  LineStrip,     // Each additional vertex after the first two makes a new line.
  TriStrip,      // Each additional vertex after the first three makes a new triangle.
  Rect2d,        // Each rect is the bounding box of an arbitrary 2D triangle.
  RectList,      // Each rect is three 2D axis-aligned rectangle vertices.
  Last,
};

// Specifies how to populate the sample mask provided to pixel shaders.
enum class CoverageToShaderSel : unsigned {
  InputCoverage = 0,  // In over rasterization mode, replicate the overrast result to all detail samples of
                      ///  the pixel. In standard rasterization mode, leave the sample mask untouched.
  InputInnerCoverage, // In under rasterization mode, replicate the underrast result to all detail samples
                      ///  of the pixel. If under rasterization is disabled output raw mask.
  InputDepthCoverage, // The InputCoverage mask bitwise ANDed with the result of Early Depth/Stencil testing.
  Raw,                // Output the scan converter's internal mask, unchanged.
};

namespace PalCodeObjectMetadataKey {
static constexpr char Version[] = "amdpal.version";
static constexpr char Pipelines[] = "amdpal.pipelines";
static constexpr char PrintfStrings[] = "amdpal.format_strings";
}; // namespace PalCodeObjectMetadataKey

namespace PipelineMetadataKey {
static constexpr char Index[] = ".index";
static constexpr char String[] = ".string";
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
static constexpr char GraphicsRegisters[] = ".graphics_registers";
static constexpr char ComputeRegisters[] = ".compute_registers";
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
static constexpr char ChecksumValue[] = ".checksum_value";
static constexpr char FloatMode[] = ".float_mode";
static constexpr char DebugMode[] = ".debug_mode";
static constexpr char TrapPresent[] = ".trap_present";
static constexpr char UserSgprs[] = ".user_sgprs";
static constexpr char MemOrdered[] = ".mem_ordered";
static constexpr char WgpMode[] = ".wgp_mode";
static constexpr char OffchipLdsEn[] = ".offchip_lds_en";
static constexpr char UserDataRegMap[] = ".user_data_reg_map";
static constexpr char ImageOp[] = ".image_op";
}; // namespace HardwareStageMetadataKey

namespace ShaderMetadataKey {
static constexpr char ApiShaderHash[] = ".api_shader_hash";
static constexpr char HardwareMapping[] = ".hardware_mapping";
}; // namespace ShaderMetadataKey

namespace ComputeRegisterMetadataKey {
static constexpr char TgidXEn[] = ".tgid_x_en";
static constexpr char TgidYEn[] = ".tgid_y_en";
static constexpr char TgidZEn[] = ".tgid_z_en";
static constexpr char TgSizeEn[] = ".tg_size_en";
static constexpr char TidigCompCnt[] = ".tidig_comp_cnt";
}; // namespace ComputeRegisterMetadataKey

namespace GraphicsRegisterMetadataKey {
static constexpr char NggCullingDataReg[] = ".ngg_culling_data_reg";
static constexpr char LsVgprCompCnt[] = ".ls_vgpr_comp_cnt";
static constexpr char HsTgSizeEn[] = ".hs_tg_size_en";
static constexpr char EsVgprCompCnt[] = ".es_vgpr_comp_cnt";
static constexpr char GsVgprCompCnt[] = ".gs_vgpr_comp_cnt";
static constexpr char VsVgprCompCnt[] = ".vs_vgpr_comp_cnt";
static constexpr char VsSoEn[] = ".vs_so_en";
static constexpr char VsSoBase0En[] = ".vs_so_base0_en";
static constexpr char VsSoBase1En[] = ".vs_so_base1_en";
static constexpr char VsSoBase2En[] = ".vs_so_base2_en";
static constexpr char VsSoBase3En[] = ".vs_so_base3_en";
static constexpr char VsStreamoutEn[] = ".vs_streamout_en";
static constexpr char VsPcBaseEn[] = ".vs_pc_base_en";
static constexpr char PsLoadProvokingVtx[] = ".ps_load_provoking_vtx";
static constexpr char PsWaveCntEn[] = ".ps_wave_cnt_en";
static constexpr char PsExtraLdsSize[] = ".ps_extra_lds_size";
static constexpr char PaClClipCntl[] = ".pa_cl_clip_cntl";
static constexpr char PaClVteCntl[] = ".pa_cl_vte_cntl";
static constexpr char PaSuVtxCntl[] = ".pa_su_vtx_cntl";
static constexpr char PaScModeCntl1[] = ".pa_sc_mode_cntl_1";
static constexpr char PsIterSample[] = ".ps_iter_sample";
static constexpr char VgtShaderStagesEn[] = ".vgt_shader_stages_en";
static constexpr char VgtReuseOff[] = ".vgt_reuse_off";
static constexpr char VgtGsMode[] = ".vgt_gs_mode";
static constexpr char VgtTfParam[] = ".vgt_tf_param";
static constexpr char VgtLsHsConfig[] = ".vgt_ls_hs_config";
static constexpr char IaMultiVgtParam[] = ".ia_multi_vgt_param";
static constexpr char IaMultiVgtParamPiped[] = ".ia_multi_vgt_param_piped";
static constexpr char SpiInterpControl[] = ".spi_interp_control";
static constexpr char SpiPsInputCntl[] = ".spi_ps_input_cntl";
static constexpr char VgtHosMinTessLevel[] = ".vgt_hos_min_tess_level";
static constexpr char VgtHosMaxTessLevel[] = ".vgt_hos_max_tess_level";
static constexpr char SpiShaderGsMeshletDim[] = ".spi_shader_gs_meshlet_dim";
static constexpr char SpiShaderGsMeshletExpAlloc[] = ".spi_shader_gs_meshlet_exp_alloc";
static constexpr char MeshLinearDispatchFromTask[] = ".mesh_linear_dispatch_from_task";
static constexpr char ImageOp[] = ".image_op";
static constexpr char VgtGsMaxVertOut[] = ".vgt_gs_max_vert_out";
static constexpr char VgtGsInstanceCnt[] = ".vgt_gs_instance_cnt";
static constexpr char VgtEsgsRingItemsize[] = ".vgt_esgs_ring_itemsize";
static constexpr char VgtDrawPrimPayloadEn[] = ".vgt_draw_prim_payload_en";
static constexpr char VgtGsOutPrimType[] = ".vgt_gs_out_prim_type";
static constexpr char VgtGsVertItemsize[] = ".vgt_gs_vert_itemsize";
static constexpr char VgtGsvsRingOffset[] = ".vgt_gsvs_ring_offset";
static constexpr char VgtGsvsRingItemsize[] = ".vgt_gsvs_ring_itemsize";
static constexpr char VgtEsPerGs[] = ".vgt_es_per_gs";
static constexpr char VgtGsPerEs[] = ".vgt_gs_per_es";
static constexpr char VgtGsPerVs[] = ".vgt_gs_per_vs";
static constexpr char MaxVertsPerSubgroup[] = ".max_verts_per_subgroup";
static constexpr char MaxPrimsPerSubgroup[] = ".max_prims_per_subgroup";
static constexpr char SpiShaderIdxFormat[] = ".spi_shader_idx_format";
static constexpr char GeNggSubgrpCntl[] = ".ge_ngg_subgrp_cntl";
static constexpr char VgtGsOnchipCntl[] = ".vgt_gs_onchip_cntl";
static constexpr char PaClVsOutCntl[] = ".pa_cl_vs_out_cntl";
static constexpr char SpiShaderPosFormat[] = ".spi_shader_pos_format";
static constexpr char SpiVsOutConfig[] = ".spi_vs_out_config";
static constexpr char VgtPrimitiveIdEn[] = ".vgt_primitive_id_en";
static constexpr char NggDisableProvokReuse[] = ".ngg_disable_provok_reuse";
static constexpr char VgtStrmoutConfig[] = ".vgt_strmout_config";
static constexpr char VgtStrmoutBufferConfig[] = ".vgt_strmout_buffer_config";
static constexpr char VgtStrmoutVtxStride0[] = ".vgt_strmout_vtx_stride_0";
static constexpr char VgtStrmoutVtxStride1[] = ".vgt_strmout_vtx_stride_1";
static constexpr char VgtStrmoutVtxStride2[] = ".vgt_strmout_vtx_stride_2";
static constexpr char VgtStrmoutVtxStride3[] = ".vgt_strmout_vtx_stride_3";
static constexpr char CbShaderMask[] = ".cb_shader_mask";
static constexpr char DbShaderControl[] = ".db_shader_control";
static constexpr char SpiPsInControl[] = ".spi_ps_in_control";
static constexpr char AaCoverageToShaderSelect[] = ".aa_coverage_to_shader_select";
static constexpr char PaScShaderControl[] = ".pa_sc_shader_control";
static constexpr char SpiBarycCntl[] = ".spi_baryc_cntl";
static constexpr char SpiPsInputEna[] = ".spi_ps_input_ena";
static constexpr char SpiPsInputAddr[] = ".spi_ps_input_addr";
static constexpr char SpiShaderColFormat[] = ".spi_shader_col_format";
static constexpr char SpiShaderZFormat[] = ".spi_shader_z_format";
}; // namespace GraphicsRegisterMetadataKey

namespace PaClClipCntlMetadataKey {
static constexpr char UserClipPlane0Ena[] = ".user_clip_plane0_ena";
static constexpr char UserClipPlane1Ena[] = ".user_clip_plane1_ena";
static constexpr char UserClipPlane2Ena[] = ".user_clip_plane2_ena";
static constexpr char UserClipPlane3Ena[] = ".user_clip_plane3_ena";
static constexpr char UserClipPlane4Ena[] = ".user_clip_plane4_ena";
static constexpr char UserClipPlane5Ena[] = ".user_clip_plane5_ena";
static constexpr char DxLinearAttrClipEna[] = ".dx_linear_attr_clip_ena";
static constexpr char RasterizationKill[] = ".rasterization_kill";
static constexpr char VteVportProvokeDisable[] = ".vte_vport_provoke_disable";
}; // namespace PaClClipCntlMetadataKey

namespace PaSuVtxCntlMetadataKey {
static constexpr char PixCenter[] = ".pix_center";
static constexpr char RoundMode[] = ".round_mode";
static constexpr char QuantMode[] = ".quant_mode";
}; // namespace PaSuVtxCntlMetadataKey

namespace PaClVteCntlMetadataKey {
static constexpr char XScaleEna[] = ".x_scale_ena";
static constexpr char XOffsetEna[] = ".x_offset_ena";
static constexpr char YScaleEna[] = ".y_scale_ena";
static constexpr char YOffsetEna[] = ".y_offset_ena";
static constexpr char ZScaleEna[] = ".z_scale_ena";
static constexpr char ZOffsetEna[] = ".z_offset_ena";
static constexpr char VtxW0Fmt[] = ".vtx_w0_fmt";
}; // namespace PaClVteCntlMetadataKey

namespace VgtShaderStagesEnMetadataKey {
static constexpr char LsStageEn[] = ".ls_stage_en";
static constexpr char HsStageEn[] = ".hs_stage_en";
static constexpr char EsStageEn[] = ".es_stage_en";
static constexpr char GsStageEn[] = ".gs_stage_en";
static constexpr char VsStageEn[] = ".vs_stage_en";
static constexpr char DynamicHs[] = ".dynamic_hs";
static constexpr char MaxPrimgroupInWave[] = ".max_primgroup_in_wave";
static constexpr char PrimgenEn[] = ".primgen_en";
static constexpr char OrderedIdMode[] = ".ordered_id_mode";
static constexpr char NggWaveIdEn[] = ".ngg_wave_id_en";
static constexpr char GsFastLaunch[] = ".gs_fast_launch";
static constexpr char PrimgenPassthruEn[] = ".primgen_passthru_en";
static constexpr char GsW32En[] = ".gs_w32_en";
static constexpr char VsW32En[] = ".vs_w32_en";
static constexpr char HsW32En[] = ".hs_w32_en";
static constexpr char PrimgenPassthruNoMsg[] = ".primgen_passthru_no_msg";
}; // namespace VgtShaderStagesEnMetadataKey

namespace IaMultiVgtParamMetadataKey {
static constexpr char PrimgroupSize[] = ".primgroup_size";
static constexpr char SwitchOnEoi[] = ".switch_on_eoi";
static constexpr char PartialEsWaveOn[] = ".partial_es_wave_on";
}; // namespace IaMultiVgtParamMetadataKey

namespace IaMultiVgtParamPipedMetadataKey {
static constexpr char PrimgroupSize[] = ".primgroup_size";
static constexpr char SwitchOnEoi[] = ".switch_on_eoi";
static constexpr char PartialEsWaveOn[] = ".partial_es_wave_on";
}; // namespace IaMultiVgtParamPipedMetadataKey

namespace VgtGsModeMetadataKey {
static constexpr char Mode[] = ".mode";
static constexpr char Onchip[] = ".onchip";
static constexpr char EsWriteOptimize[] = ".es_write_optimize";
static constexpr char GsWriteOptimize[] = ".gs_write_optimize";
static constexpr char CutMode[] = ".cut_mode";
}; // namespace VgtGsModeMetadataKey

namespace SpiBarycCntlMetadataKey {
static constexpr char PosFloatLocation[] = ".pos_float_location";
static constexpr char FrontFaceAllBits[] = ".front_face_all_bits";
static constexpr char PosFloatUlc[] = ".pos_float_ulc";
}; // namespace SpiBarycCntlMetadataKey

namespace DbShaderControlMetadataKey {
static constexpr char ZExportEnable[] = ".z_export_enable";
static constexpr char StencilTestValExportEnable[] = ".stencil_test_val_export_enable";
static constexpr char ZOrder[] = ".z_order";
static constexpr char KillEnable[] = ".kill_enable";
static constexpr char MaskExportEnable[] = ".mask_export_enable";
static constexpr char ExecOnHierFail[] = ".exec_on_hier_fail";
static constexpr char ExecOnNoop[] = ".exec_on_noop";
static constexpr char AlphaToMaskDisable[] = ".alpha_to_mask_disable";
static constexpr char DepthBeforeShader[] = ".depth_before_shader";
static constexpr char ConservativeZExport[] = ".conservative_z_export";
static constexpr char PreShaderDepthCoverageEnable[] = ".pre_shader_depth_coverage_enable";
}; // namespace DbShaderControlMetadataKey

namespace SpiInterpControlMetadataKey {
static constexpr char PointSpriteEna[] = ".point_sprite_ena";
static constexpr char PointSpriteOverrideX[] = ".point_sprite_override_x";
static constexpr char PointSpriteOverrideY[] = ".point_sprite_override_y";
static constexpr char PointSpriteOverrideZ[] = ".point_sprite_override_z";
static constexpr char PointSpriteOverrideW[] = ".point_sprite_override_w";
}; // namespace SpiInterpControlMetadataKey

namespace SpiPsInputCntlMetadataKey {
static constexpr char Offset[] = ".offset";
static constexpr char FlatShade[] = ".flat_shade";
static constexpr char PtSpriteTex[] = ".pt_sprite_tex";
static constexpr char Fp16InterpMode[] = ".fp16_interp_mode";
static constexpr char Attr0Valid[] = ".attr0_valid";
static constexpr char Attr1Valid[] = ".attr1_valid";
static constexpr char PrimAttr[] = ".prim_attr";
}; // namespace SpiPsInputCntlMetadataKey

namespace SpiPsInControlMetadataKey {
static constexpr char NumInterps[] = ".num_interps";
static constexpr char NumPrimInterp[] = ".num_prim_interp";
static constexpr char PsW32En[] = ".ps_w32_en";
}; // namespace SpiPsInControlMetadataKey

namespace VgtGsOnchipCntlMetadataKey {
static constexpr char EsVertsPerSubgroup[] = ".es_verts_per_subgroup";
static constexpr char GsPrimsPerSubgroup[] = ".gs_prims_per_subgroup";
static constexpr char GsInstPrimsPerSubgrp[] = ".gs_inst_prims_per_subgrp";
}; // namespace VgtGsOnchipCntlMetadataKey

namespace VgtGsInstanceCntMetadataKey {
static constexpr char Enable[] = ".enable";
static constexpr char Count[] = ".count";
static constexpr char EnMaxVertOutPerGsInstance[] = ".en_max_vert_out_per_gs_instance";
}; // namespace VgtGsInstanceCntMetadataKey

namespace VgtGsOutPrimTypeMetadataKey {
static constexpr char OutprimType[] = ".outprim_type";
static constexpr char OutprimType_1[] = ".outprim_type_1";
static constexpr char OutprimType_2[] = ".outprim_type_2";
static constexpr char OutprimType_3[] = ".outprim_type_3";
static constexpr char UniqueTypePerStream[] = ".unique_type_per_stream";
}; // namespace VgtGsOutPrimTypeMetadataKey

namespace SpiVsOutConfigMetadataKey {
static constexpr char NoPcExport[] = ".no_pc_export";
static constexpr char VsExportCount[] = ".vs_export_count";
static constexpr char PrimExportCount[] = ".prim_export_count";
}; // namespace SpiVsOutConfigMetadataKey

namespace PaClVsOutCntlMetadataKey {
static constexpr char ClipDistEna_0[] = ".clip_dist_ena_0";
static constexpr char ClipDistEna_1[] = ".clip_dist_ena_1";
static constexpr char ClipDistEna_2[] = ".clip_dist_ena_2";
static constexpr char ClipDistEna_3[] = ".clip_dist_ena_3";
static constexpr char ClipDistEna_4[] = ".clip_dist_ena_4";
static constexpr char ClipDistEna_5[] = ".clip_dist_ena_5";
static constexpr char ClipDistEna_6[] = ".clip_dist_ena_6";
static constexpr char ClipDistEna_7[] = ".clip_dist_ena_7";
static constexpr char CullDistEna_0[] = ".cull_dist_ena_0";
static constexpr char CullDistEna_1[] = ".cull_dist_ena_1";
static constexpr char CullDistEna_2[] = ".cull_dist_ena_2";
static constexpr char CullDistEna_3[] = ".cull_dist_ena_3";
static constexpr char CullDistEna_4[] = ".cull_dist_ena_4";
static constexpr char CullDistEna_5[] = ".cull_dist_ena_5";
static constexpr char CullDistEna_6[] = ".cull_dist_ena_6";
static constexpr char CullDistEna_7[] = ".cull_dist_ena_7";
static constexpr char UseVtxPointSize[] = ".use_vtx_point_size";
static constexpr char UseVtxEdgeFlag[] = ".use_vtx_edge_flag";
static constexpr char UseVtxRenderTargetIndx[] = ".use_vtx_render_target_indx";
static constexpr char UseVtxViewportIndx[] = ".use_vtx_viewport_indx";
static constexpr char UseVtxKillFlag[] = ".use_vtx_kill_flag";
static constexpr char VsOutMiscVecEna[] = ".vs_out_misc_vec_ena";
static constexpr char VsOutCcDist0VecEna[] = ".vs_out_cc_dist0_vec_ena";
static constexpr char VsOutCcDist1VecEna[] = ".vs_out_cc_dist1_vec_ena";
static constexpr char VsOutMiscSideBusEna[] = ".vs_out_misc_side_bus_ena";
static constexpr char UseVtxLineWidth[] = ".use_vtx_line_width";
static constexpr char UseVtxVrsRate[] = ".use_vtx_vrs_rate";
static constexpr char BypassVtxRateCombiner[] = ".bypass_vtx_rate_combiner";
static constexpr char BypassPrimRateCombiner[] = ".bypass_prim_rate_combiner";
static constexpr char UseVtxGsCutFlag[] = ".use_vtx_gs_cut_flag";
#if PAL_BUILD_GFX11
static constexpr char UseVtxFsrSelect[] = ".use_vtx_fsr_select";
#endif
}; // namespace PaClVsOutCntlMetadataKey

namespace GeNggSubgrpCntlMetadataKey {
static constexpr char PrimAmpFactor[] = ".prim_amp_factor";
static constexpr char ThreadsPerSubgroup[] = ".threads_per_subgroup";
}; // namespace GeNggSubgrpCntlMetadataKey

namespace SpiShaderColFormatMetadataKey {
static constexpr char Col_0ExportFormat[] = ".col_0_export_format";
static constexpr char Col_1ExportFormat[] = ".col_1_export_format";
static constexpr char Col_2ExportFormat[] = ".col_2_export_format";
static constexpr char Col_3ExportFormat[] = ".col_3_export_format";
static constexpr char Col_4ExportFormat[] = ".col_4_export_format";
static constexpr char Col_5ExportFormat[] = ".col_5_export_format";
static constexpr char Col_6ExportFormat[] = ".col_6_export_format";
static constexpr char Col_7ExportFormat[] = ".col_7_export_format";
}; // namespace SpiShaderColFormatMetadataKey

namespace PaScShaderControlMetadataKey {
static constexpr char LoadCollisionWaveid[] = ".load_collision_waveid";
static constexpr char LoadIntrawaveCollision[] = ".load_intrawave_collision";
static constexpr char WaveBreakRegionSize[] = ".wave_break_region_size";
}; // namespace PaScShaderControlMetadataKey

namespace VgtLsHsConfigMetadataKey {
static constexpr char NumPatches[] = ".num_patches";
static constexpr char HsNumInputCp[] = ".hs_num_input_cp";
static constexpr char HsNumOutputCp[] = ".hs_num_output_cp";
}; // namespace VgtLsHsConfigMetadataKey

namespace VgtTfParamMetadataKey {
static constexpr char Type[] = ".type";
static constexpr char Partitioning[] = ".partitioning";
static constexpr char Topology[] = ".topology";
static constexpr char DisableDonuts[] = ".disable_donuts";
static constexpr char NumDsWavesPerSimd[] = ".num_ds_waves_per_simd";
static constexpr char DistributionMode[] = ".distribution_mode";
}; // namespace VgtTfParamMetadataKey

namespace VgtStrmoutConfigMetadataKey {
static constexpr char Streamout_0En[] = ".streamout_0_en";
static constexpr char Streamout_1En[] = ".streamout_1_en";
static constexpr char Streamout_2En[] = ".streamout_2_en";
static constexpr char Streamout_3En[] = ".streamout_3_en";
static constexpr char RastStream[] = ".rast_stream";
static constexpr char PrimsNeededCntEn[] = ".prims_needed_cnt_en";
static constexpr char RastStreamMask[] = ".rast_stream_mask";
static constexpr char UseRastStreamMask[] = ".use_rast_stream_mask";
}; // namespace VgtStrmoutConfigMetadataKey

namespace VgtStrmoutBufferConfigMetadataKey {
static constexpr char Stream_0BufferEn[] = ".stream_0_buffer_en";
static constexpr char Stream_1BufferEn[] = ".stream_1_buffer_en";
static constexpr char Stream_2BufferEn[] = ".stream_2_buffer_en";
static constexpr char Stream_3BufferEn[] = ".stream_3_buffer_en";
}; // namespace VgtStrmoutBufferConfigMetadataKey

namespace SpiShaderGsMeshletDimMetadataKey {
static constexpr char NumThreadX[] = ".num_thread_x";
static constexpr char NumThreadY[] = ".num_thread_y";
static constexpr char NumThreadZ[] = ".num_thread_z";
static constexpr char ThreadgroupSize[] = ".threadgroup_size";
}; // namespace SpiShaderGsMeshletDimMetadataKey

namespace SpiShaderGsMeshletExpAllocMetadataKey {
static constexpr char MaxExpVerts[] = ".max_exp_verts";
static constexpr char MaxExpPrims[] = ".max_exp_prims";
}; // namespace SpiShaderGsMeshletExpAllocMetadataKey

namespace CbShaderMaskMetadataKey {
static constexpr char Output0Enable[] = ".output0_enable";
static constexpr char Output1Enable[] = ".output1_enable";
static constexpr char Output2Enable[] = ".output2_enable";
static constexpr char Output3Enable[] = ".output3_enable";
static constexpr char Output4Enable[] = ".output4_enable";
static constexpr char Output5Enable[] = ".output5_enable";
static constexpr char Output6Enable[] = ".output6_enable";
static constexpr char Output7Enable[] = ".output7_enable";
}; // namespace CbShaderMaskMetadataKey

namespace SpiPsInputAddrMetadataKey {
static constexpr char PerspSampleEna[] = ".persp_sample_ena";
static constexpr char PerspCenterEna[] = ".persp_center_ena";
static constexpr char PerspCentroidEna[] = ".persp_centroid_ena";
static constexpr char PerspPullModelEna[] = ".persp_pull_model_ena";
static constexpr char LinearSampleEna[] = ".linear_sample_ena";
static constexpr char LinearCenterEna[] = ".linear_center_ena";
static constexpr char LinearCentroidEna[] = ".linear_centroid_ena";
static constexpr char LineStippleTexEna[] = ".line_stipple_tex_ena";
static constexpr char PosXFloatEna[] = ".pos_x_float_ena";
static constexpr char PosYFloatEna[] = ".pos_y_float_ena";
static constexpr char PosZFloatEna[] = ".pos_z_float_ena";
static constexpr char PosWFloatEna[] = ".pos_w_float_ena";
static constexpr char FrontFaceEna[] = ".front_face_ena";
static constexpr char AncillaryEna[] = ".ancillary_ena";
static constexpr char SampleCoverageEna[] = ".sample_coverage_ena";
static constexpr char PosFixedPtEna[] = ".pos_fixed_pt_ena";
}; // namespace SpiPsInputAddrMetadataKey

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
  ColorExportAddr = 0x10000020,      // Color export address

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
constexpr unsigned mmVGT_GS_OUT_PRIM_TYPE_GFX11 = 0xC266;

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

union SPI_SHADER_Z_FORMAT {
  struct {
    unsigned int Z_EXPORT_FORMAT : 4;
    unsigned int : 28;
  } bits, bitfields;

  unsigned int u32All;
  signed int i32All;
  float f32All;
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
