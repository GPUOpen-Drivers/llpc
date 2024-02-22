//===- SPIRVEnum.h - SPIR-V enums -------------------------------*- C++ -*-===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file defines SPIR-V enums.
///
//===----------------------------------------------------------------------===//

#ifndef SPIRV_LIBSPIRV_SPIRVENUM_H
#define SPIRV_LIBSPIRV_SPIRVENUM_H

#include "SPIRVOpCode.h"
#include "spirvExt.h"
#include <cstdint>
using namespace spv;

namespace SPIRV {

typedef uint32_t SPIRVWord;
typedef uint32_t SPIRVId;
#define SPIRVID_MAX ~0U
#define SPIRVID_INVALID ~0U
#define SPIRVWORD_MAX ~0U

inline bool isValidId(SPIRVId Id) {
  return Id != SPIRVID_INVALID && Id != 0;
}

inline SPIRVWord mkWord(unsigned WordCount, Op OpCode) {
  return (WordCount << 16) | OpCode;
}

const static unsigned KSpirvMemOrderSemanticMask = 0x1F;

enum SPIRVVersion : SPIRVWord { SPIRV_1_0 = 0x00010000, SPIRV_1_1 = 0x00010100 };

enum SPIRVGeneratorKind {
  SPIRVGEN_KhronosLLVMSPIRVTranslator = 6,
  SPIRVGEN_KhronosSPIRVAssembler = 7,
};

enum SPIRVInstructionSchemaKind {
  SPIRVISCH_Default,
};

enum SPIRVExtInstSetKind {
  SPIRVEIS_GLSL,
  SPIRVEIS_ShaderBallotAMD,
  SPIRVEIS_ShaderExplicitVertexParameterAMD,
  SPIRVEIS_GcnShaderAMD,
  SPIRVEIS_ShaderTrinaryMinMaxAMD,
  SPIRVEIS_NonSemanticInfo,
  SPIRVEIS_NonSemanticDebugBreak,
  SPIRVEIS_NonSemanticDebugPrintf,
  SPIRVEIS_NonSemanticShaderDebugInfo100,
  SPIRVEIS_Debug,
  SPIRVEIS_Count,
};

enum SPIRVTypeWidthKind {
  SPIRVTW_8Bit = 0x1,
  SPIRVTW_16Bit = 0x2,
  SPIRVTW_32Bit = 0x4,
  SPIRVTW_64Bit = 0x8,
};

typedef spv::Capability SPIRVCapabilityKind;
typedef spv::ExecutionModel SPIRVExecutionModelKind;
typedef spv::ExecutionMode SPIRVExecutionModeKind;
typedef spv::AddressingModel SPIRVAddressingModelKind;
typedef spv::LinkageType SPIRVLinkageTypeKind;
typedef spv::MemoryModel SPIRVMemoryModelKind;
typedef spv::StorageClass SPIRVStorageClassKind;
typedef spv::FunctionControlMask SPIRVFunctionControlMaskKind;
typedef spv::FPRoundingMode SPIRVFPRoundingModeKind;
typedef spv::BuiltIn SPIRVBuiltinVariableKind;
typedef spv::MemoryAccessMask SPIRVMemoryAccessKind;
typedef spv::GroupOperation SPIRVGroupOperationKind;
typedef spv::Dim SPIRVImageDimKind;
typedef std::vector<SPIRVCapabilityKind> SPIRVCapVec;

template <> inline void SPIRVMap<SPIRVExtInstSetKind, std::string>::init() {
  add(SPIRVEIS_GLSL, "GLSL.std.450");
  add(SPIRVEIS_ShaderBallotAMD, "SPV_AMD_shader_ballot");
  add(SPIRVEIS_ShaderExplicitVertexParameterAMD, "SPV_AMD_shader_explicit_vertex_parameter");
  add(SPIRVEIS_GcnShaderAMD, "SPV_AMD_gcn_shader");
  add(SPIRVEIS_ShaderTrinaryMinMaxAMD, "SPV_AMD_shader_trinary_minmax");
  add(SPIRVEIS_NonSemanticDebugBreak, "NonSemantic.DebugBreak");
  add(SPIRVEIS_NonSemanticDebugPrintf, "NonSemantic.DebugPrintf");
  add(SPIRVEIS_Debug, "OpenCL.DebugInfo.100");
  add(SPIRVEIS_NonSemanticShaderDebugInfo100, "NonSemantic.Shader.DebugInfo.100");
}

typedef SPIRVMap<SPIRVExtInstSetKind, std::string> SPIRVBuiltinSetNameMap;

template <typename K> SPIRVCapVec getCapability(K Key) {
  SPIRVCapVec V;
  SPIRVMap<K, SPIRVCapVec>::find(Key, &V);
  return V;
}

#define ADD_VEC_INIT(Cap, ...)                                                                                         \
  {                                                                                                                    \
    SPIRVCapabilityKind C[] = __VA_ARGS__;                                                                             \
    SPIRVCapVec V(C, C + sizeof(C) / sizeof(C[0]));                                                                    \
    add(Cap, V);                                                                                                       \
  }

template <> inline void SPIRVMap<SPIRVCapabilityKind, SPIRVCapVec>::init() {
  ADD_VEC_INIT(CapabilityShader, {CapabilityMatrix});
  ADD_VEC_INIT(CapabilityGeometry, {CapabilityShader});
  ADD_VEC_INIT(CapabilityTessellation, {CapabilityShader});
  ADD_VEC_INIT(CapabilityInt64Atomics, {CapabilityInt64});
  ADD_VEC_INIT(CapabilityAtomicStorage, {CapabilityShader});
  ADD_VEC_INIT(CapabilityTessellationPointSize, {CapabilityTessellation});
  ADD_VEC_INIT(CapabilityGeometryPointSize, {CapabilityGeometry});
  ADD_VEC_INIT(CapabilityImageGatherExtended, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStorageImageMultisample, {CapabilityShader});
  ADD_VEC_INIT(CapabilityUniformBufferArrayDynamicIndexing, {CapabilityShader});
  ADD_VEC_INIT(CapabilitySampledImageArrayDynamicIndexing, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStorageBufferArrayDynamicIndexing, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStorageImageArrayDynamicIndexing, {CapabilityShader});
  ADD_VEC_INIT(CapabilityClipDistance, {CapabilityShader});
  ADD_VEC_INIT(CapabilityCullDistance, {CapabilityShader});
  ADD_VEC_INIT(CapabilityImageCubeArray, {CapabilitySampledCubeArray});
  ADD_VEC_INIT(CapabilitySampleRateShading, {CapabilityShader});
  ADD_VEC_INIT(CapabilityImageRect, {CapabilitySampledRect});
  ADD_VEC_INIT(CapabilitySampledRect, {CapabilityShader});
  ADD_VEC_INIT(CapabilityGenericPointer, {CapabilityAddresses});
  ADD_VEC_INIT(CapabilityInputAttachment, {CapabilityShader});
  ADD_VEC_INIT(CapabilitySparseResidency, {CapabilityShader});
  ADD_VEC_INIT(CapabilityMinLod, {CapabilityShader});
  ADD_VEC_INIT(CapabilityImage1D, {CapabilitySampled1D});
  ADD_VEC_INIT(CapabilitySampledCubeArray, {CapabilityShader});
  ADD_VEC_INIT(CapabilityImageBuffer, {CapabilitySampledBuffer});
  ADD_VEC_INIT(CapabilityImageMSArray, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStorageImageExtendedFormats, {CapabilityShader});
  ADD_VEC_INIT(CapabilityImageQuery, {CapabilityShader});
  ADD_VEC_INIT(CapabilityDerivativeControl, {CapabilityShader});
  ADD_VEC_INIT(CapabilityInterpolationFunction, {CapabilityShader});
  ADD_VEC_INIT(CapabilityTransformFeedback, {CapabilityShader});
  ADD_VEC_INIT(CapabilityGeometryStreams, {CapabilityGeometry});
  ADD_VEC_INIT(CapabilityStorageImageReadWithoutFormat, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStorageImageWriteWithoutFormat, {CapabilityShader});
  ADD_VEC_INIT(CapabilityMultiViewport, {CapabilityGeometry});
  ADD_VEC_INIT(CapabilityDrawParameters, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStencilExportEXT, {CapabilityShader});
  ADD_VEC_INIT(CapabilityShaderViewportIndexLayerEXT, {CapabilityMultiViewport});
  ADD_VEC_INIT(CapabilityUniformAndStorageBuffer16BitAccess, {CapabilityStorageBuffer16BitAccess});
  ADD_VEC_INIT(CapabilityGroupNonUniformVote, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilityGroupNonUniformArithmetic, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilityGroupNonUniformBallot, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilityGroupNonUniformShuffle, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilityGroupNonUniformShuffleRelative, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilityGroupNonUniformClustered, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilityGroupNonUniformQuad, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(CapabilitySampleMaskPostDepthCoverage, {CapabilityShader});
  ADD_VEC_INIT(CapabilityStorageBuffer8BitAccess, {CapabilityShader});
  ADD_VEC_INIT(CapabilityUniformAndStorageBuffer8BitAccess, {CapabilityStorageBuffer8BitAccess});
  ADD_VEC_INIT(CapabilityStoragePushConstant8, {CapabilityShader});
  ADD_VEC_INIT(CapabilityImageGatherBiasLodAMD, {CapabilityShader});
  ADD_VEC_INIT(CapabilityFragmentMaskAMD, {CapabilityShader});
  ADD_VEC_INIT(CapabilityFloat16ImageAMD, {CapabilityShader});
  ADD_VEC_INIT(CapabilityVariablePointersStorageBuffer, {CapabilityShader});
  ADD_VEC_INIT(CapabilityVariablePointers, {CapabilityVariablePointersStorageBuffer});
  ADD_VEC_INIT(CapabilityShaderNonUniformEXT, {CapabilityShader});
  ADD_VEC_INIT(CapabilityRuntimeDescriptorArrayEXT, {CapabilityShader});
  ADD_VEC_INIT(CapabilityInputAttachmentArrayDynamicIndexingEXT, {CapabilityInputAttachment});
  ADD_VEC_INIT(CapabilityUniformTexelBufferArrayDynamicIndexingEXT, {CapabilitySampledBuffer});
  ADD_VEC_INIT(CapabilityStorageTexelBufferArrayDynamicIndexingEXT, {CapabilityImageBuffer});
  ADD_VEC_INIT(CapabilityUniformBufferArrayNonUniformIndexingEXT, {CapabilityShaderNonUniformEXT});
  ADD_VEC_INIT(CapabilitySampledImageArrayNonUniformIndexingEXT, {CapabilityShaderNonUniformEXT});
  ADD_VEC_INIT(CapabilityStorageBufferArrayNonUniformIndexingEXT, {CapabilityShaderNonUniformEXT});
  ADD_VEC_INIT(CapabilityStorageImageArrayNonUniformIndexingEXT, {CapabilityShaderNonUniformEXT});
  ADD_VEC_INIT(CapabilityInputAttachmentArrayNonUniformIndexingEXT, {CapabilityInputAttachment});
  ADD_VEC_INIT(CapabilityUniformTexelBufferArrayNonUniformIndexingEXT, {CapabilitySampledBuffer});
  ADD_VEC_INIT(CapabilityStorageTexelBufferArrayNonUniformIndexingEXT, {CapabilityImageBuffer});
  ADD_VEC_INIT(CapabilityInt64ImageEXT, {CapabilityShader});
  ADD_VEC_INIT(CapabilityDotProductInput4x8BitKHR, {CapabilityInt16});
  ADD_VEC_INIT(CapabilityMeshShadingEXT, {CapabilityShader});
  ADD_VEC_INIT(CapabilityFragmentBarycentricKHR, {CapabilityShader});
  ADD_VEC_INIT(CapabilityCooperativeMatrixKHR, {CapabilityShader});
  ADD_VEC_INIT(CapabilityComputeDerivativeGroupLinearNV, {CapabilityShader});
  ADD_VEC_INIT(CapabilityComputeDerivativeGroupQuadsNV, {CapabilityShader});
  ADD_VEC_INIT(CapabilityQuadControlKHR, {CapabilityShader});
}

template <> inline void SPIRVMap<SPIRVExecutionModelKind, SPIRVCapVec>::init() {
  ADD_VEC_INIT(ExecutionModelVertex, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModelTessellationControl, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModelTessellationEvaluation, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModelGeometry, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModelFragment, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModelGLCompute, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModelTaskEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(ExecutionModelMeshEXT, {CapabilityMeshShadingEXT});
}

template <> inline void SPIRVMap<SPIRVExecutionModeKind, SPIRVCapVec>::init() {
  ADD_VEC_INIT(ExecutionModeInvocations, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeSpacingEqual, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeSpacingFractionalEven, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeSpacingFractionalOdd, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeVertexOrderCw, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeVertexOrderCcw, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModePixelCenterInteger, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeOriginUpperLeft, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeOriginLowerLeft, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeEarlyFragmentTests, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModePointMode, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeXfb, {CapabilityTransformFeedback});
  ADD_VEC_INIT(ExecutionModeDepthReplacing, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeDepthGreater, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeDepthLess, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeDepthUnchanged, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeInputPoints, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeInputLines, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeInputLinesAdjacency, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeTriangles, {CapabilityGeometry, CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeInputTrianglesAdjacency, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeQuads, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeIsolines, {CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeOutputVertices, {CapabilityGeometry, CapabilityTessellation});
  ADD_VEC_INIT(ExecutionModeOutputPoints, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeOutputLineStrip, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModeOutputTriangleStrip, {CapabilityGeometry});
  ADD_VEC_INIT(ExecutionModePostDepthCoverage, {CapabilitySampleMaskPostDepthCoverage});
  ADD_VEC_INIT(ExecutionModeDenormPreserve, {CapabilityDenormPreserve});
  ADD_VEC_INIT(ExecutionModeDenormFlushToZero, {CapabilityDenormFlushToZero});
  ADD_VEC_INIT(ExecutionModeSignedZeroInfNanPreserve, {CapabilitySignedZeroInfNanPreserve});
  ADD_VEC_INIT(ExecutionModeRoundingModeRTE, {CapabilityRoundingModeRTE});
  ADD_VEC_INIT(ExecutionModeRoundingModeRTZ, {CapabilityRoundingModeRTZ});
  ADD_VEC_INIT(ExecutionModeOutputLinesEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(ExecutionModeOutputTrianglesEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(ExecutionModeOutputPrimitivesEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(ExecutionModeEarlyAndLateFragmentTestsAMD, {CapabilityShader});
  ADD_VEC_INIT(ExecutionModeStencilRefUnchangedFrontAMD, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(ExecutionModeStencilRefGreaterFrontAMD, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(ExecutionModeStencilRefLessFrontAMD, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(ExecutionModeStencilRefUnchangedBackAMD, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(ExecutionModeStencilRefGreaterBackAMD, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(ExecutionModeStencilRefLessBackAMD, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(ExecutionModeRequireFullQuadsKHR, {CapabilityQuadControlKHR});
  ADD_VEC_INIT(ExecutionModeQuadDerivativesKHR, {CapabilityQuadControlKHR});
}

template <> inline void SPIRVMap<SPIRVMemoryModelKind, SPIRVCapVec>::init() {
  ADD_VEC_INIT(MemoryModelSimple, {CapabilityShader});
  ADD_VEC_INIT(MemoryModelGLSL450, {CapabilityShader});
}

template <> inline void SPIRVMap<SPIRVStorageClassKind, SPIRVCapVec>::init() {
  ADD_VEC_INIT(StorageClassInput, {CapabilityShader});
  ADD_VEC_INIT(StorageClassUniform, {CapabilityShader});
  ADD_VEC_INIT(StorageClassOutput, {CapabilityShader});
  ADD_VEC_INIT(StorageClassPrivate, {CapabilityShader});
  ADD_VEC_INIT(StorageClassGeneric, {CapabilityGenericPointer});
  ADD_VEC_INIT(StorageClassPushConstant, {CapabilityShader});
  ADD_VEC_INIT(StorageClassAtomicCounter, {CapabilityAtomicStorage});
  ADD_VEC_INIT(StorageClassStorageBuffer, {CapabilityShader});
  ADD_VEC_INIT(StorageClassCallableDataKHR, {CapabilityRayTracingProvisionalKHR});
  ADD_VEC_INIT(StorageClassIncomingCallableDataKHR, {CapabilityRayTracingProvisionalKHR});
  ADD_VEC_INIT(StorageClassRayPayloadKHR, {CapabilityRayTracingProvisionalKHR});
  ADD_VEC_INIT(StorageClassHitAttributeKHR, {CapabilityRayTracingProvisionalKHR});
  ADD_VEC_INIT(StorageClassIncomingRayPayloadKHR, {CapabilityRayTracingProvisionalKHR});
  ADD_VEC_INIT(StorageClassShaderRecordBufferKHR, {CapabilityRayTracingProvisionalKHR});
  ADD_VEC_INIT(StorageClassTaskPayloadWorkgroupEXT, {CapabilityMeshShadingEXT});
}

template <> inline void SPIRVMap<SPIRVImageDimKind, SPIRVCapVec>::init() {
  ADD_VEC_INIT(Dim1D, {CapabilitySampled1D});
  ADD_VEC_INIT(DimCube, {CapabilityShader});
  ADD_VEC_INIT(DimRect, {CapabilitySampledRect});
  ADD_VEC_INIT(DimBuffer, {CapabilitySampledBuffer});
  ADD_VEC_INIT(DimSubpassData, {CapabilityInputAttachment});
}

template <> inline void SPIRVMap<ImageFormat, SPIRVCapVec>::init() {
  ADD_VEC_INIT(ImageFormatRgba32f, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba16f, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatR32f, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba8, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba8Snorm, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRg32f, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg16f, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR11fG11fB10f, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR16f, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRgba16, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRgb10A2, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg16, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg8, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR16, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR8, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRgba16Snorm, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg16Snorm, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg8Snorm, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR16Snorm, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR8Snorm, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRgba32i, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba16i, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba8i, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatR32i, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRg32i, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg16i, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg8i, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR16i, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR8i, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRgba32ui, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba16ui, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgba8ui, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatR32ui, {CapabilityShader});
  ADD_VEC_INIT(ImageFormatRgb10a2ui, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg32ui, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatRg16ui, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR16ui, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR8ui, {CapabilityStorageImageExtendedFormats});
  ADD_VEC_INIT(ImageFormatR64ui, {CapabilityInt64ImageEXT});
  ADD_VEC_INIT(ImageFormatR64i, {CapabilityInt64ImageEXT});
}

template <> inline void SPIRVMap<ImageOperandsMask, SPIRVCapVec>::init() {
  ADD_VEC_INIT(ImageOperandsBiasMask, {CapabilityShader});
  ADD_VEC_INIT(ImageOperandsOffsetMask, {CapabilityImageGatherExtended});
  ADD_VEC_INIT(ImageOperandsMinLodMask, {CapabilityMinLod});
}

template <> inline void SPIRVMap<Decoration, SPIRVCapVec>::init() {
  ADD_VEC_INIT(DecorationRelaxedPrecision, {CapabilityShader});
  ADD_VEC_INIT(DecorationSpecId, {CapabilityShader});
  ADD_VEC_INIT(DecorationBlock, {CapabilityShader});
  ADD_VEC_INIT(DecorationBufferBlock, {CapabilityShader});
  ADD_VEC_INIT(DecorationRowMajor, {CapabilityMatrix});
  ADD_VEC_INIT(DecorationColMajor, {CapabilityMatrix});
  ADD_VEC_INIT(DecorationArrayStride, {CapabilityShader});
  ADD_VEC_INIT(DecorationMatrixStride, {CapabilityMatrix});
  ADD_VEC_INIT(DecorationGLSLShared, {CapabilityShader});
  ADD_VEC_INIT(DecorationGLSLPacked, {CapabilityShader});
  ADD_VEC_INIT(DecorationNoPerspective, {CapabilityShader});
  ADD_VEC_INIT(DecorationFlat, {CapabilityShader});
  ADD_VEC_INIT(DecorationPatch, {CapabilityTessellation});
  ADD_VEC_INIT(DecorationCentroid, {CapabilityShader});
  ADD_VEC_INIT(DecorationSample, {CapabilitySampleRateShading});
  ADD_VEC_INIT(DecorationInvariant, {CapabilityShader});
  ADD_VEC_INIT(DecorationUniform, {CapabilityShader});
  ADD_VEC_INIT(DecorationUniformId, {CapabilityShader});
  ADD_VEC_INIT(DecorationStream, {CapabilityGeometryStreams});
  ADD_VEC_INIT(DecorationLocation, {CapabilityShader});
  ADD_VEC_INIT(DecorationComponent, {CapabilityShader});
  ADD_VEC_INIT(DecorationIndex, {CapabilityShader});
  ADD_VEC_INIT(DecorationBinding, {CapabilityShader});
  ADD_VEC_INIT(DecorationDescriptorSet, {CapabilityShader});
  ADD_VEC_INIT(DecorationOffset, {CapabilityShader});
  ADD_VEC_INIT(DecorationXfbBuffer, {CapabilityTransformFeedback});
  ADD_VEC_INIT(DecorationXfbStride, {CapabilityTransformFeedback});
  ADD_VEC_INIT(DecorationLinkageAttributes, {CapabilityLinkage});
  ADD_VEC_INIT(DecorationNoContraction, {CapabilityShader});
  ADD_VEC_INIT(DecorationInputAttachmentIndex, {CapabilityInputAttachment});
  ADD_VEC_INIT(DecorationNonUniformEXT, {CapabilityShaderNonUniformEXT});
  ADD_VEC_INIT(DecorationPerPrimitiveEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(DecorationPerVertexKHR, {CapabilityFragmentBarycentricKHR});
}

template <> inline void SPIRVMap<BuiltIn, SPIRVCapVec>::init() {
  ADD_VEC_INIT(BuiltInPosition, {CapabilityShader});
  ADD_VEC_INIT(BuiltInPointSize, {CapabilityShader});
  ADD_VEC_INIT(BuiltInClipDistance, {CapabilityClipDistance});
  ADD_VEC_INIT(BuiltInCullDistance, {CapabilityCullDistance});
  ADD_VEC_INIT(BuiltInVertexId, {CapabilityShader});
  ADD_VEC_INIT(BuiltInInstanceId, {CapabilityShader});
  ADD_VEC_INIT(BuiltInPrimitiveId, {CapabilityGeometry, CapabilityTessellation});
  ADD_VEC_INIT(BuiltInInvocationId, {CapabilityGeometry, CapabilityTessellation});
  ADD_VEC_INIT(BuiltInLayer, {CapabilityGeometry});
  ADD_VEC_INIT(BuiltInViewportIndex, {CapabilityMultiViewport});
  ADD_VEC_INIT(BuiltInTessLevelOuter, {CapabilityTessellation});
  ADD_VEC_INIT(BuiltInTessLevelInner, {CapabilityTessellation});
  ADD_VEC_INIT(BuiltInTessCoord, {CapabilityTessellation});
  ADD_VEC_INIT(BuiltInPatchVertices, {CapabilityTessellation});
  ADD_VEC_INIT(BuiltInFragCoord, {CapabilityShader});
  ADD_VEC_INIT(BuiltInPointCoord, {CapabilityShader});
  ADD_VEC_INIT(BuiltInFrontFacing, {CapabilityShader});
  ADD_VEC_INIT(BuiltInSampleId, {CapabilitySampleRateShading});
  ADD_VEC_INIT(BuiltInSamplePosition, {CapabilitySampleRateShading});
  ADD_VEC_INIT(BuiltInSampleMask, {CapabilitySampleRateShading});
  ADD_VEC_INIT(BuiltInFragDepth, {CapabilityShader});
  ADD_VEC_INIT(BuiltInHelperInvocation, {CapabilityShader});
  ADD_VEC_INIT(BuiltInSubgroupSize, {CapabilityGroupNonUniform, CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInNumSubgroups, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(BuiltInSubgroupId, {CapabilityGroupNonUniform});
  ADD_VEC_INIT(BuiltInSubgroupLocalInvocationId, {CapabilityGroupNonUniform, CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInVertexIndex, {CapabilityShader});
  ADD_VEC_INIT(BuiltInInstanceIndex, {CapabilityShader});
  ADD_VEC_INIT(BuiltInBaseVertex, {CapabilityDrawParameters});
  ADD_VEC_INIT(BuiltInBaseInstance, {CapabilityDrawParameters});
  ADD_VEC_INIT(BuiltInDrawIndex, {CapabilityDrawParameters});
  ADD_VEC_INIT(BuiltInFragStencilRefEXT, {CapabilityStencilExportEXT});
  ADD_VEC_INIT(BuiltInSubgroupEqMaskKHR, {CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInSubgroupGeMaskKHR, {CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInSubgroupGtMaskKHR, {CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInSubgroupLeMaskKHR, {CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInSubgroupLtMaskKHR, {CapabilitySubgroupBallotKHR});
  ADD_VEC_INIT(BuiltInDeviceIndex, {CapabilityDeviceGroup});
  ADD_VEC_INIT(BuiltInViewIndex, {CapabilityMultiView});
  ADD_VEC_INIT(BuiltInPrimitiveShadingRateKHR, {CapabilityFragmentShadingRateKHR});
  ADD_VEC_INIT(BuiltInShadingRateKHR, {CapabilityFragmentShadingRateKHR});
  ADD_VEC_INIT(BuiltInCullPrimitiveEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(BuiltInPrimitivePointIndicesEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(BuiltInPrimitiveLineIndicesEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(BuiltInPrimitiveTriangleIndicesEXT, {CapabilityMeshShadingEXT});
  ADD_VEC_INIT(BuiltInBaryCoordKHR, {CapabilityFragmentBarycentricKHR});
  ADD_VEC_INIT(BuiltInBaryCoordNoPerspKHR, {CapabilityFragmentBarycentricKHR});
}

template <> inline void SPIRVMap<MemorySemanticsMask, SPIRVCapVec>::init() {
  ADD_VEC_INIT(MemorySemanticsUniformMemoryMask, {CapabilityShader});
  ADD_VEC_INIT(MemorySemanticsAtomicCounterMemoryMask, {CapabilityAtomicStorage});
}

#undef ADD_VEC_INIT

inline unsigned getImageDimension(SPIRVImageDimKind K) {
  switch (K) {
  case Dim1D:
    return 1;
  case Dim2D:
    return 2;
  case Dim3D:
    return 3;
  case DimCube:
    return 2;
  case DimRect:
    return 2;
  case DimBuffer:
    return 1;
  default:
    return 0;
  }
}

/// Extract memory order part of SPIR-V memory semantics.
inline unsigned extractSPIRVMemOrderSemantic(unsigned Sema) {
  return Sema & KSpirvMemOrderSemanticMask;
}

} // namespace SPIRV

#endif
