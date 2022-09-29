//===- SPIRVIsValidEnum.h - SPIR-V isValid enums ----------------*- C++ -*-===//
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
/// This file defines SPIR-V isValid enums.
///
//===----------------------------------------------------------------------===//
// WARNING:
//
// This file has been generated using `tools/spirv-tool/gen_spirv.bash` and
// should not be modified manually. If the file needs to be updated, edit the
// script and any other source file instead, before re-generating this file.
//===----------------------------------------------------------------------===//

#ifndef SPIRV_LIBSPIRV_SPIRVISVALIDENUM_H
#define SPIRV_LIBSPIRV_SPIRVISVALIDENUM_H

#include "SPIRVEnum.h"
#include "spirv.hpp"

using namespace spv;

namespace SPIRV {

inline bool isValid(spv::SourceLanguage V) {
  switch (V) {
  case SourceLanguageUnknown:
  case SourceLanguageESSL:
  case SourceLanguageGLSL:
  case SourceLanguageHLSL:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::ExecutionModel V) {
  uint32_t ExecModel = V;
  switch (ExecModel) {
  case ExecutionModelVertex:
  case ExecutionModelTessellationControl:
  case ExecutionModelTessellationEvaluation:
  case ExecutionModelGeometry:
  case ExecutionModelFragment:
  case ExecutionModelGLCompute:
#if VKI_RAY_TRACING
  case ExecutionModelRayGenerationKHR:
  case ExecutionModelIntersectionKHR:
  case ExecutionModelAnyHitKHR:
  case ExecutionModelClosestHitKHR:
  case ExecutionModelMissKHR:
  case ExecutionModelCallableKHR:
#endif
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::AddressingModel V) {
  switch (V) {
  case AddressingModelLogical:
  case AddressingModelPhysical32:
  case AddressingModelPhysical64:
#if SPV_VERSION >= 0x10500
  case AddressingModelPhysicalStorageBuffer64:
#else
  case AddressingModelPhysicalStorageBuffer64EXT:
#endif
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::MemoryModel V) {
  switch (V) {
  case MemoryModelSimple:
  case MemoryModelGLSL450:
#if SPV_VERSION >= 0x10500
  case MemoryModelVulkan:
#endif
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::ExecutionMode V) {
  uint32_t ExecMode = V;
  switch (ExecMode) {
  case ExecutionModeInvocations:
  case ExecutionModeSpacingEqual:
  case ExecutionModeSpacingFractionalEven:
  case ExecutionModeSpacingFractionalOdd:
  case ExecutionModeVertexOrderCw:
  case ExecutionModeVertexOrderCcw:
  case ExecutionModePixelCenterInteger:
  case ExecutionModeOriginUpperLeft:
  case ExecutionModeOriginLowerLeft:
  case ExecutionModeEarlyFragmentTests:
  case ExecutionModePointMode:
  case ExecutionModeXfb:
  case ExecutionModeDepthReplacing:
  case ExecutionModeDepthGreater:
  case ExecutionModeDepthLess:
  case ExecutionModeDepthUnchanged:
  case ExecutionModeLocalSize:
  case ExecutionModeInputPoints:
  case ExecutionModeInputLines:
  case ExecutionModeInputLinesAdjacency:
  case ExecutionModeTriangles:
  case ExecutionModeInputTrianglesAdjacency:
  case ExecutionModeQuads:
  case ExecutionModeIsolines:
  case ExecutionModeOutputVertices:
  case ExecutionModeOutputPoints:
  case ExecutionModeOutputLineStrip:
  case ExecutionModeOutputTriangleStrip:
  case ExecutionModeSubgroupSize:
  case ExecutionModeSubgroupsPerWorkgroup:
  case ExecutionModePostDepthCoverage:
  case ExecutionModeDenormPreserve:
  case ExecutionModeDenormFlushToZero:
  case ExecutionModeSignedZeroInfNanPreserve:
  case ExecutionModeRoundingModeRTE:
  case ExecutionModeRoundingModeRTZ:
  case ExecutionModeSubgroupUniformControlFlowKHR:
  case ExecutionModeStencilRefReplacingEXT:
  case ExecutionModeEarlyAndLateFragmentTestsAMD:
  case ExecutionModeStencilRefUnchangedFrontAMD:
  case ExecutionModeStencilRefGreaterFrontAMD:
  case ExecutionModeStencilRefLessFrontAMD:
  case ExecutionModeStencilRefUnchangedBackAMD:
  case ExecutionModeStencilRefGreaterBackAMD:
  case ExecutionModeStencilRefLessBackAMD:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::StorageClass V) {
  uint32_t StorageClass = V;
  switch (StorageClass) {
  case StorageClassUniformConstant:
  case StorageClassInput:
  case StorageClassUniform:
  case StorageClassOutput:
  case StorageClassWorkgroup:
  case StorageClassCrossWorkgroup:
  case StorageClassPrivate:
  case StorageClassFunction:
  case StorageClassGeneric:
  case StorageClassPushConstant:
  case StorageClassAtomicCounter:
  case StorageClassImage:
  case StorageClassStorageBuffer:
#if SPV_VERSION >= 0x10500
  case StorageClassPhysicalStorageBuffer:
#else
  case StorageClassPhysicalStorageBufferEXT:
#endif
#if VKI_RAY_TRACING
  case StorageClassCallableDataKHR:
  case StorageClassIncomingCallableDataKHR:
  case StorageClassRayPayloadKHR:
  case StorageClassHitAttributeKHR:
  case StorageClassIncomingRayPayloadKHR:
  case StorageClassShaderRecordBufferKHR:
#endif
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::Dim V) {
  switch (V) {
  case Dim1D:
  case Dim2D:
  case Dim3D:
  case DimCube:
  case DimRect:
  case DimBuffer:
  case DimSubpassData:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::ImageFormat V) {
  uint32_t Format = V;
  switch (Format) {
  case ImageFormatUnknown:
  case ImageFormatRgba32f:
  case ImageFormatRgba16f:
  case ImageFormatR32f:
  case ImageFormatRgba8:
  case ImageFormatRgba8Snorm:
  case ImageFormatRg32f:
  case ImageFormatRg16f:
  case ImageFormatR11fG11fB10f:
  case ImageFormatR16f:
  case ImageFormatRgba16:
  case ImageFormatRgb10A2:
  case ImageFormatRg16:
  case ImageFormatRg8:
  case ImageFormatR16:
  case ImageFormatR8:
  case ImageFormatRgba16Snorm:
  case ImageFormatRg16Snorm:
  case ImageFormatRg8Snorm:
  case ImageFormatR16Snorm:
  case ImageFormatR8Snorm:
  case ImageFormatRgba32i:
  case ImageFormatRgba16i:
  case ImageFormatRgba8i:
  case ImageFormatR32i:
  case ImageFormatRg32i:
  case ImageFormatRg16i:
  case ImageFormatRg8i:
  case ImageFormatR16i:
  case ImageFormatR8i:
  case ImageFormatRgba32ui:
  case ImageFormatRgba16ui:
  case ImageFormatRgba8ui:
  case ImageFormatR32ui:
  case ImageFormatRgb10a2ui:
  case ImageFormatRg32ui:
  case ImageFormatRg16ui:
  case ImageFormatRg8ui:
  case ImageFormatR16ui:
  case ImageFormatR8ui:
  case ImageFormatR64ui:
  case ImageFormatR64i:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::FPRoundingMode V) {
  switch (V) {
  case FPRoundingModeRTE:
  case FPRoundingModeRTZ:
  case FPRoundingModeRTP:
  case FPRoundingModeRTN:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::LinkageType V) {
  uint32_t Ty = V;
  switch (Ty) {
  case LinkageTypeExport:
  case LinkageTypeImport:
  case LinkageTypeInternal:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::Decoration V) {
  uint32_t Decor = V;
  switch (Decor) {
  case DecorationRelaxedPrecision:
  case DecorationSpecId:
  case DecorationBlock:
  case DecorationBufferBlock:
  case DecorationRowMajor:
  case DecorationColMajor:
  case DecorationArrayStride:
  case DecorationMatrixStride:
  case DecorationGLSLShared:
  case DecorationGLSLPacked:
  case DecorationBuiltIn:
  case DecorationNoPerspective:
  case DecorationFlat:
  case DecorationPatch:
  case DecorationCentroid:
  case DecorationSample:
  case DecorationInvariant:
  case DecorationRestrict:
  case DecorationAliased:
  case DecorationVolatile:
  case DecorationCoherent:
  case DecorationNonWritable:
  case DecorationNonReadable:
  case DecorationUniform:
#if SPV_VERSION >= 0x10400
  case DecorationUniformId:
#endif
  case DecorationStream:
  case DecorationLocation:
  case DecorationComponent:
  case DecorationIndex:
  case DecorationBinding:
  case DecorationDescriptorSet:
  case DecorationOffset:
  case DecorationXfbBuffer:
  case DecorationXfbStride:
  case DecorationFPRoundingMode:
  case DecorationLinkageAttributes:
  case DecorationNoContraction:
  case DecorationInputAttachmentIndex:
  case DecorationMaxByteOffset:
#if SPV_VERSION >= 0x10400
  case DecorationNoSignedWrap:
  case DecorationNoUnsignedWrap:
#endif
  case DecorationExplicitInterpAMD:
  case DecorationPerVertexKHR:
#if SPV_VERSION >= 0x10500
  case DecorationNonUniform:
  case DecorationRestrictPointer:
  case DecorationAliasedPointer:
#else
  case DecorationNonUniformEXT:
#endif
  case DecorationHlslCounterBufferGOOGLE:
  case DecorationHlslSemanticGOOGLE:
  case DecorationUserTypeGOOGLE:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::BuiltIn V) {
  uint32_t Id = V;
  switch (Id) {
  case BuiltInPosition:
  case BuiltInPointSize:
  case BuiltInClipDistance:
  case BuiltInCullDistance:
  case BuiltInVertexId:
  case BuiltInInstanceId:
  case BuiltInPrimitiveId:
  case BuiltInInvocationId:
  case BuiltInLayer:
  case BuiltInViewportIndex:
  case BuiltInTessLevelOuter:
  case BuiltInTessLevelInner:
  case BuiltInTessCoord:
  case BuiltInPatchVertices:
  case BuiltInFragCoord:
  case BuiltInPointCoord:
  case BuiltInFrontFacing:
  case BuiltInSampleId:
  case BuiltInSamplePosition:
  case BuiltInSampleMask:
  case BuiltInFragDepth:
  case BuiltInHelperInvocation:
  case BuiltInNumWorkgroups:
  case BuiltInWorkgroupSize:
  case BuiltInWorkgroupId:
  case BuiltInLocalInvocationId:
  case BuiltInGlobalInvocationId:
  case BuiltInLocalInvocationIndex:
  case BuiltInSubgroupSize:
  case BuiltInNumSubgroups:
  case BuiltInSubgroupId:
  case BuiltInSubgroupLocalInvocationId:
  case BuiltInVertexIndex:
  case BuiltInInstanceIndex:
  case BuiltInBaseVertex:
  case BuiltInBaseInstance:
  case BuiltInDrawIndex:
  case BuiltInFragStencilRefEXT:
  case BuiltInSubgroupEqMaskKHR:
  case BuiltInSubgroupGeMaskKHR:
  case BuiltInSubgroupGtMaskKHR:
  case BuiltInSubgroupLeMaskKHR:
  case BuiltInSubgroupLtMaskKHR:
  case BuiltInPrimitiveShadingRateKHR:
  case BuiltInDeviceIndex:
  case BuiltInViewIndex:
  case BuiltInShadingRateKHR:
  case BuiltInBaryCoordNoPerspAMD:
  case BuiltInBaryCoordNoPerspCentroidAMD:
  case BuiltInBaryCoordNoPerspSampleAMD:
  case BuiltInBaryCoordSmoothAMD:
  case BuiltInBaryCoordSmoothCentroidAMD:
  case BuiltInBaryCoordSmoothSampleAMD:
  case BuiltInBaryCoordPullModelAMD:
  case BuiltInBaryCoordKHR:
  case BuiltInBaryCoordNoPerspKHR:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::Scope V) {
  switch (V) {
  case ScopeCrossDevice:
  case ScopeDevice:
  case ScopeWorkgroup:
  case ScopeSubgroup:
  case ScopeInvocation:
#if SPV_VERSION >= 0x10500
  case ScopeQueueFamily:
#endif
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::GroupOperation V) {
  switch (V) {
  case GroupOperationReduce:
  case GroupOperationInclusiveScan:
  case GroupOperationExclusiveScan:
  case GroupOperationClusteredReduce:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::Capability V) {
  uint32_t Cap = V;
  switch (Cap) {
  case CapabilityMatrix:
  case CapabilityShader:
  case CapabilityGeometry:
  case CapabilityTessellation:
  case CapabilityAddresses:
  case CapabilityLinkage:
  case CapabilityFloat16:
  case CapabilityFloat64:
  case CapabilityInt64:
  case CapabilityInt64Atomics:
  case CapabilityGroups:
  case CapabilityAtomicStorage:
  case CapabilityInt16:
  case CapabilityTessellationPointSize:
  case CapabilityGeometryPointSize:
  case CapabilityImageGatherExtended:
  case CapabilityStorageImageMultisample:
  case CapabilityUniformBufferArrayDynamicIndexing:
  case CapabilitySampledImageArrayDynamicIndexing:
  case CapabilityStorageBufferArrayDynamicIndexing:
  case CapabilityStorageImageArrayDynamicIndexing:
  case CapabilityClipDistance:
  case CapabilityCullDistance:
  case CapabilityImageCubeArray:
  case CapabilitySampleRateShading:
  case CapabilityImageRect:
  case CapabilitySampledRect:
  case CapabilityGenericPointer:
  case CapabilityInt8:
  case CapabilityInputAttachment:
  case CapabilitySparseResidency:
  case CapabilityMinLod:
  case CapabilitySampled1D:
  case CapabilityImage1D:
  case CapabilitySampledCubeArray:
  case CapabilitySampledBuffer:
  case CapabilityImageBuffer:
  case CapabilityImageMSArray:
  case CapabilityStorageImageExtendedFormats:
  case CapabilityImageQuery:
  case CapabilityDerivativeControl:
  case CapabilityInterpolationFunction:
  case CapabilityTransformFeedback:
  case CapabilityGeometryStreams:
  case CapabilityStorageImageReadWithoutFormat:
  case CapabilityStorageImageWriteWithoutFormat:
  case CapabilityMultiViewport:
  case CapabilitySubgroupDispatch:
  case CapabilityNamedBarrier:
  case CapabilityGroupNonUniform:
  case CapabilityGroupNonUniformVote:
  case CapabilityGroupNonUniformArithmetic:
  case CapabilityGroupNonUniformBallot:
  case CapabilityGroupNonUniformShuffle:
  case CapabilityGroupNonUniformShuffleRelative:
  case CapabilityGroupNonUniformClustered:
  case CapabilityGroupNonUniformQuad:
#if SPV_VERSION >= 0x10500
  case CapabilityShaderLayer:
  case CapabilityShaderViewportIndex:
#endif
  case CapabilityStencilExportEXT:
  case CapabilityShaderViewportIndexLayerEXT:
  case CapabilitySubgroupBallotKHR:
  case CapabilitySubgroupVoteKHR:
  case CapabilityStorageBuffer16BitAccess:
  case CapabilityUniformAndStorageBuffer16BitAccess:
  case CapabilityStoragePushConstant16:
  case CapabilityStorageInputOutput16:
  case CapabilityDeviceGroup:
  case CapabilityMultiView:
  case CapabilitySampleMaskPostDepthCoverage:
  case CapabilityStorageBuffer8BitAccess:
  case CapabilityUniformAndStorageBuffer8BitAccess:
  case CapabilityStoragePushConstant8:
  case CapabilityDenormPreserve:
  case CapabilityDenormFlushToZero:
  case CapabilitySignedZeroInfNanPreserve:
  case CapabilityRoundingModeRTE:
  case CapabilityRoundingModeRTZ:
  case CapabilityImageGatherBiasLodAMD:
  case CapabilityFragmentMaskAMD:
#if VKI_RAY_TRACING
  case CapabilityRayQueryProvisionalKHR:
  case CapabilityRayTraversalPrimitiveCullingKHR:
#endif
  case CapabilityFloat16ImageAMD:
  case CapabilityShaderClockKHR:
  case CapabilityVariablePointersStorageBuffer:
  case CapabilityVariablePointers:
  case CapabilityFragmentShadingRateKHR:
  case CapabilityInt64ImageEXT:
#if SPV_VERSION >= 0x10500
  case CapabilityShaderNonUniform:
  case CapabilityRuntimeDescriptorArray:
  case CapabilityInputAttachmentArrayDynamicIndexing:
  case CapabilityUniformTexelBufferArrayDynamicIndexing:
  case CapabilityStorageTexelBufferArrayDynamicIndexing:
  case CapabilityUniformBufferArrayNonUniformIndexing:
  case CapabilitySampledImageArrayNonUniformIndexing:
  case CapabilityStorageBufferArrayNonUniformIndexing:
  case CapabilityStorageImageArrayNonUniformIndexing:
  case CapabilityInputAttachmentArrayNonUniformIndexing:
  case CapabilityUniformTexelBufferArrayNonUniformIndexing:
  case CapabilityStorageTexelBufferArrayNonUniformIndexing:
  case CapabilityVulkanMemoryModel:
  case CapabilityVulkanMemoryModelDeviceScope:
  case CapabilityPhysicalStorageBufferAddresses:
#else
  case CapabilityShaderNonUniformEXT:
  case CapabilityRuntimeDescriptorArrayEXT:
  case CapabilityInputAttachmentArrayDynamicIndexingEXT:
  case CapabilityUniformTexelBufferArrayDynamicIndexingEXT:
  case CapabilityStorageTexelBufferArrayDynamicIndexingEXT:
  case CapabilityUniformBufferArrayNonUniformIndexingEXT:
  case CapabilitySampledImageArrayNonUniformIndexingEXT:
  case CapabilityStorageBufferArrayNonUniformIndexingEXT:
  case CapabilityStorageImageArrayNonUniformIndexingEXT:
  case CapabilityInputAttachmentArrayNonUniformIndexingEXT:
  case CapabilityUniformTexelBufferArrayNonUniformIndexingEXT:
  case CapabilityStorageTexelBufferArrayNonUniformIndexingEXT:
#endif
  case CapabilityDemoteToHelperInvocationEXT:
  case CapabilityAtomicFloat32MinMaxEXT:
  case CapabilityAtomicFloat64MinMaxEXT:
  case CapabilityDotProductKHR:
  case CapabilityDotProductInputAllKHR:
  case CapabilityDotProductInput4x8BitKHR:
  case CapabilityDotProductInput4x8BitPackedKHR:
  case CapabilityWorkgroupMemoryExplicitLayoutKHR:
  case CapabilityWorkgroupMemoryExplicitLayout8BitAccessKHR:
  case CapabilityWorkgroupMemoryExplicitLayout16BitAccessKHR:
    return true;
  default:
    return false;
  }
}

inline bool isValid(spv::Op V) {
  uint32_t Id = V;
  switch (Id) {
  case OpNop:
  case OpUndef:
  case OpSourceContinued:
  case OpSource:
  case OpSourceExtension:
  case OpName:
  case OpMemberName:
  case OpString:
  case OpLine:
  case OpExtension:
  case OpExtInstImport:
  case OpExtInst:
  case OpMemoryModel:
  case OpEntryPoint:
  case OpExecutionMode:
  case OpCapability:
  case OpTypeVoid:
  case OpTypeBool:
  case OpTypeInt:
  case OpTypeFloat:
  case OpTypeVector:
  case OpTypeMatrix:
  case OpTypeImage:
  case OpTypeSampler:
  case OpTypeSampledImage:
  case OpTypeArray:
  case OpTypeRuntimeArray:
  case OpTypeStruct:
  case OpTypePointer:
  case OpTypeFunction:
  case OpTypeForwardPointer:
  case OpConstantTrue:
  case OpConstantFalse:
  case OpConstant:
  case OpConstantComposite:
  case OpConstantNull:
  case OpSpecConstantTrue:
  case OpSpecConstantFalse:
  case OpSpecConstant:
  case OpSpecConstantComposite:
  case OpSpecConstantOp:
  case OpFunction:
  case OpFunctionParameter:
  case OpFunctionEnd:
  case OpFunctionCall:
  case OpVariable:
  case OpImageTexelPointer:
  case OpLoad:
  case OpStore:
  case OpCopyMemory:
  case OpCopyMemorySized:
  case OpAccessChain:
  case OpInBoundsAccessChain:
  case OpPtrAccessChain:
  case OpArrayLength:
  case OpInBoundsPtrAccessChain:
  case OpDecorate:
  case OpMemberDecorate:
  case OpDecorationGroup:
  case OpGroupDecorate:
  case OpGroupMemberDecorate:
  case OpVectorExtractDynamic:
  case OpVectorInsertDynamic:
  case OpVectorShuffle:
  case OpCompositeConstruct:
  case OpCompositeExtract:
  case OpCompositeInsert:
  case OpCopyObject:
  case OpTranspose:
  case OpSampledImage:
  case OpImageSampleImplicitLod:
  case OpImageSampleExplicitLod:
  case OpImageSampleDrefImplicitLod:
  case OpImageSampleDrefExplicitLod:
  case OpImageSampleProjImplicitLod:
  case OpImageSampleProjExplicitLod:
  case OpImageSampleProjDrefImplicitLod:
  case OpImageSampleProjDrefExplicitLod:
  case OpImageFetch:
  case OpImageGather:
  case OpImageDrefGather:
  case OpImageRead:
  case OpImageWrite:
  case OpImage:
  case OpImageQuerySizeLod:
  case OpImageQuerySize:
  case OpImageQueryLod:
  case OpImageQueryLevels:
  case OpImageQuerySamples:
  case OpConvertFToU:
  case OpConvertFToS:
  case OpConvertSToF:
  case OpConvertUToF:
  case OpUConvert:
  case OpSConvert:
  case OpFConvert:
  case OpQuantizeToF16:
  case OpConvertPtrToU:
  case OpConvertUToPtr:
  case OpBitcast:
  case OpSNegate:
  case OpFNegate:
  case OpIAdd:
  case OpFAdd:
  case OpISub:
  case OpFSub:
  case OpIMul:
  case OpFMul:
  case OpUDiv:
  case OpSDiv:
  case OpFDiv:
  case OpUMod:
  case OpSRem:
  case OpSMod:
  case OpFRem:
  case OpFMod:
  case OpVectorTimesScalar:
  case OpMatrixTimesScalar:
  case OpVectorTimesMatrix:
  case OpMatrixTimesVector:
  case OpMatrixTimesMatrix:
  case OpOuterProduct:
  case OpDot:
  case OpIAddCarry:
  case OpISubBorrow:
  case OpUMulExtended:
  case OpSMulExtended:
  case OpAny:
  case OpAll:
  case OpIsNan:
  case OpIsInf:
  case OpLogicalEqual:
  case OpLogicalNotEqual:
  case OpLogicalOr:
  case OpLogicalAnd:
  case OpLogicalNot:
  case OpSelect:
  case OpIEqual:
  case OpINotEqual:
  case OpUGreaterThan:
  case OpSGreaterThan:
  case OpUGreaterThanEqual:
  case OpSGreaterThanEqual:
  case OpULessThan:
  case OpSLessThan:
  case OpULessThanEqual:
  case OpSLessThanEqual:
  case OpFOrdEqual:
  case OpFUnordEqual:
  case OpFOrdNotEqual:
  case OpFUnordNotEqual:
  case OpFOrdLessThan:
  case OpFUnordLessThan:
  case OpFOrdGreaterThan:
  case OpFUnordGreaterThan:
  case OpFOrdLessThanEqual:
  case OpFUnordLessThanEqual:
  case OpFOrdGreaterThanEqual:
  case OpFUnordGreaterThanEqual:
  case OpShiftRightLogical:
  case OpShiftRightArithmetic:
  case OpShiftLeftLogical:
  case OpBitwiseOr:
  case OpBitwiseXor:
  case OpBitwiseAnd:
  case OpNot:
  case OpBitFieldInsert:
  case OpBitFieldSExtract:
  case OpBitFieldUExtract:
  case OpBitReverse:
  case OpBitCount:
  case OpDPdx:
  case OpDPdy:
  case OpFwidth:
  case OpDPdxFine:
  case OpDPdyFine:
  case OpFwidthFine:
  case OpDPdxCoarse:
  case OpDPdyCoarse:
  case OpFwidthCoarse:
  case OpEmitVertex:
  case OpEndPrimitive:
  case OpEmitStreamVertex:
  case OpEndStreamPrimitive:
  case OpControlBarrier:
  case OpMemoryBarrier:
  case OpAtomicLoad:
  case OpAtomicStore:
  case OpAtomicExchange:
  case OpAtomicCompareExchange:
  case OpAtomicIIncrement:
  case OpAtomicIDecrement:
  case OpAtomicIAdd:
  case OpAtomicISub:
  case OpAtomicSMin:
  case OpAtomicUMin:
  case OpAtomicSMax:
  case OpAtomicUMax:
  case OpAtomicAnd:
  case OpAtomicOr:
  case OpAtomicXor:
  case OpAtomicFMinEXT:
  case OpAtomicFMaxEXT:
  case OpAtomicFAddEXT:
  case OpPhi:
  case OpLoopMerge:
  case OpSelectionMerge:
  case OpLabel:
  case OpBranch:
  case OpBranchConditional:
  case OpSwitch:
  case OpKill:
  case OpReturn:
  case OpReturnValue:
  case OpUnreachable:
  case OpGroupAll:
  case OpGroupAny:
  case OpGroupBroadcast:
  case OpGroupIAdd:
  case OpGroupFAdd:
  case OpGroupFMin:
  case OpGroupUMin:
  case OpGroupSMin:
  case OpGroupFMax:
  case OpGroupUMax:
  case OpGroupSMax:
  case OpImageSparseSampleImplicitLod:
  case OpImageSparseSampleExplicitLod:
  case OpImageSparseSampleDrefImplicitLod:
  case OpImageSparseSampleDrefExplicitLod:
  case OpImageSparseSampleProjImplicitLod:
  case OpImageSparseSampleProjExplicitLod:
  case OpImageSparseSampleProjDrefImplicitLod:
  case OpImageSparseSampleProjDrefExplicitLod:
  case OpImageSparseFetch:
  case OpImageSparseGather:
  case OpImageSparseDrefGather:
  case OpImageSparseTexelsResident:
  case OpNoLine:
  case OpImageSparseRead:
  case OpSizeOf:
  case OpModuleProcessed:
  case OpExecutionModeId:
  case OpDecorateId:
#if SPV_VERSION >= 0x10400
  case OpDecorateString:
  case OpMemberDecorateString:
#endif
  case OpGroupNonUniformElect:
  case OpGroupNonUniformAll:
  case OpGroupNonUniformAny:
  case OpGroupNonUniformAllEqual:
  case OpGroupNonUniformBroadcast:
  case OpGroupNonUniformBroadcastFirst:
  case OpGroupNonUniformBallot:
  case OpGroupNonUniformInverseBallot:
  case OpGroupNonUniformBallotBitExtract:
  case OpGroupNonUniformBallotBitCount:
  case OpGroupNonUniformBallotFindLSB:
  case OpGroupNonUniformBallotFindMSB:
  case OpGroupNonUniformShuffle:
  case OpGroupNonUniformShuffleXor:
  case OpGroupNonUniformShuffleUp:
  case OpGroupNonUniformShuffleDown:
  case OpGroupNonUniformIAdd:
  case OpGroupNonUniformFAdd:
  case OpGroupNonUniformIMul:
  case OpGroupNonUniformFMul:
  case OpGroupNonUniformSMin:
  case OpGroupNonUniformUMin:
  case OpGroupNonUniformFMin:
  case OpGroupNonUniformSMax:
  case OpGroupNonUniformUMax:
  case OpGroupNonUniformFMax:
  case OpGroupNonUniformBitwiseAnd:
  case OpGroupNonUniformBitwiseOr:
  case OpGroupNonUniformBitwiseXor:
  case OpGroupNonUniformLogicalAnd:
  case OpGroupNonUniformLogicalOr:
  case OpGroupNonUniformLogicalXor:
  case OpGroupNonUniformQuadBroadcast:
  case OpGroupNonUniformQuadSwap:
#if SPV_VERSION >= 0x10400
  case OpCopyLogical:
  case OpPtrEqual:
  case OpPtrNotEqual:
  case OpPtrDiff:
#endif
  case OpForward:
  case OpTerminateInvocation:
  case OpSubgroupBallotKHR:
  case OpSubgroupFirstInvocationKHR:
  case OpSubgroupReadInvocationKHR:
#if VKI_RAY_TRACING
  case OpTypeRayQueryKHR:
  case OpRayQueryInitializeKHR:
  case OpRayQueryTerminateKHR:
  case OpRayQueryGenerateIntersectionKHR:
  case OpRayQueryConfirmIntersectionKHR:
  case OpRayQueryProceedKHR:
  case OpRayQueryGetIntersectionTypeKHR:
#endif
  case OpSubgroupAllKHR:
  case OpSubgroupAnyKHR:
  case OpSubgroupAllEqualKHR:
  case OpSDotKHR:
  case OpUDotKHR:
  case OpSUDotKHR:
  case OpSDotAccSatKHR:
  case OpUDotAccSatKHR:
  case OpSUDotAccSatKHR:
  case OpFragmentMaskFetchAMD:
  case OpFragmentFetchAMD:
  case OpGroupIAddNonUniformAMD:
  case OpGroupFAddNonUniformAMD:
  case OpGroupFMinNonUniformAMD:
  case OpGroupUMinNonUniformAMD:
  case OpGroupSMinNonUniformAMD:
  case OpGroupFMaxNonUniformAMD:
  case OpGroupUMaxNonUniformAMD:
  case OpGroupSMaxNonUniformAMD:
  case OpReadClockKHR:
#if VKI_RAY_TRACING
  case OpReportIntersectionKHR:
  case OpIgnoreIntersectionKHR:
  case OpIgnoreIntersectionNV:
  case OpTerminateRayKHR:
  case OpTerminateRayNV:
  case OpTraceNV:
  case OpTypeAccelerationStructureKHR:
  case OpExecuteCallableKHR:
  case OpTraceRayKHR:
  case OpConvertUToAccelerationStructureKHR:
#endif
  case OpSubgroupShuffleINTEL:
  case OpSubgroupShuffleDownINTEL:
  case OpSubgroupShuffleUpINTEL:
  case OpSubgroupShuffleXorINTEL:
  case OpSubgroupBlockReadINTEL:
  case OpSubgroupBlockWriteINTEL:
  case OpSubgroupImageBlockReadINTEL:
  case OpSubgroupImageBlockWriteINTEL:
  case OpDemoteToHelperInvocationEXT:
  case OpIsHelperInvocationEXT:
#if VKI_RAY_TRACING
  case OpRayQueryGetRayTMinKHR:
  case OpRayQueryGetRayFlagsKHR:
  case OpRayQueryGetIntersectionTKHR:
  case OpRayQueryGetIntersectionInstanceCustomIndexKHR:
  case OpRayQueryGetIntersectionInstanceIdKHR:
  case OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR:
  case OpRayQueryGetIntersectionGeometryIndexKHR:
  case OpRayQueryGetIntersectionPrimitiveIndexKHR:
  case OpRayQueryGetIntersectionBarycentricsKHR:
  case OpRayQueryGetIntersectionFrontFaceKHR:
  case OpRayQueryGetIntersectionCandidateAABBOpaqueKHR:
  case OpRayQueryGetIntersectionObjectRayDirectionKHR:
  case OpRayQueryGetIntersectionObjectRayOriginKHR:
  case OpRayQueryGetWorldRayDirectionKHR:
  case OpRayQueryGetWorldRayOriginKHR:
  case OpRayQueryGetIntersectionObjectToWorldKHR:
  case OpRayQueryGetIntersectionWorldToObjectKHR:
#endif
    return true;
  default:
    return false;
  }
}

inline bool isValidPackedVectorFormat(spv::PackedVectorFormat V) {
  switch (V) {
  case PackedVectorFormatPackedVectorFormat4x8BitKHR:
    return true;
  default:
    return false;
  }
}

inline bool isValidImageOperandsMask(SPIRVWord Mask) {
  SPIRVWord ValidMask = 0u;
  ValidMask |= ImageOperandsBiasMask;
  ValidMask |= ImageOperandsLodMask;
  ValidMask |= ImageOperandsGradMask;
  ValidMask |= ImageOperandsConstOffsetMask;
  ValidMask |= ImageOperandsOffsetMask;
  ValidMask |= ImageOperandsConstOffsetsMask;
  ValidMask |= ImageOperandsSampleMask;
  ValidMask |= ImageOperandsMinLodMask;
#if SPV_VERSION >= 0x10500
  ValidMask |= ImageOperandsMakeTexelAvailableMask;
  ValidMask |= ImageOperandsMakeTexelVisibleMask;
  ValidMask |= ImageOperandsNonPrivateTexelMask;
  ValidMask |= ImageOperandsVolatileTexelMask;
#endif
#if SPV_VERSION >= 0x10400
  ValidMask |= ImageOperandsSignExtendMask;
  ValidMask |= ImageOperandsZeroExtendMask;
#endif

  return (Mask & ~ValidMask) == 0;
}

inline bool isValidSelectionControlMask(SPIRVWord Mask) {
  SPIRVWord ValidMask = 0u;
  ValidMask |= SelectionControlFlattenMask;
  ValidMask |= SelectionControlDontFlattenMask;

  return (Mask & ~ValidMask) == 0;
}

inline bool isValidLoopControlMask(SPIRVWord Mask) {
  SPIRVWord ValidMask = 0u;
  ValidMask |= LoopControlUnrollMask;
  ValidMask |= LoopControlDontUnrollMask;
  ValidMask |= LoopControlDependencyInfiniteMask;
  ValidMask |= LoopControlDependencyLengthMask;
#if SPV_VERSION >= 0x10400
  ValidMask |= LoopControlMinIterationsMask;
  ValidMask |= LoopControlMaxIterationsMask;
  ValidMask |= LoopControlIterationMultipleMask;
  ValidMask |= LoopControlPeelCountMask;
  ValidMask |= LoopControlPartialCountMask;
#endif

  return (Mask & ~ValidMask) == 0;
}

inline bool isValidFunctionControlMask(SPIRVWord Mask) {
  SPIRVWord ValidMask = 0u;
  ValidMask |= FunctionControlInlineMask;
  ValidMask |= FunctionControlDontInlineMask;
  ValidMask |= FunctionControlPureMask;
  ValidMask |= FunctionControlConstMask;

  return (Mask & ~ValidMask) == 0;
}

inline bool isValidMemorySemanticsMask(SPIRVWord Mask) {
  SPIRVWord ValidMask = 0u;
  ValidMask |= MemorySemanticsAcquireMask;
  ValidMask |= MemorySemanticsReleaseMask;
  ValidMask |= MemorySemanticsAcquireReleaseMask;
  ValidMask |= MemorySemanticsSequentiallyConsistentMask;
  ValidMask |= MemorySemanticsUniformMemoryMask;
  ValidMask |= MemorySemanticsSubgroupMemoryMask;
  ValidMask |= MemorySemanticsWorkgroupMemoryMask;
  ValidMask |= MemorySemanticsCrossWorkgroupMemoryMask;
  ValidMask |= MemorySemanticsAtomicCounterMemoryMask;
  ValidMask |= MemorySemanticsImageMemoryMask;
#if SPV_VERSION >= 0x10500
  ValidMask |= MemorySemanticsOutputMemoryMask;
  ValidMask |= MemorySemanticsMakeAvailableMask;
  ValidMask |= MemorySemanticsMakeVisibleMask;
  ValidMask |= MemorySemanticsVolatileMask;
#endif

  return (Mask & ~ValidMask) == 0;
}

inline bool isValidMemoryAccessMask(SPIRVWord Mask) {
  SPIRVWord ValidMask = 0u;
  ValidMask |= MemoryAccessVolatileMask;
  ValidMask |= MemoryAccessAlignedMask;
  ValidMask |= MemoryAccessNontemporalMask;
#if SPV_VERSION >= 0x10500
  ValidMask |= MemoryAccessMakePointerAvailableMask;
  ValidMask |= MemoryAccessMakePointerVisibleMask;
  ValidMask |= MemoryAccessNonPrivatePointerMask;
#endif

  return (Mask & ~ValidMask) == 0;
}

} /* namespace SPIRV */

#endif
