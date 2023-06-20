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
 * @file  vkgcCapability.h
 * @brief VKGC header file: contains the list of supported capabilities.
 ***********************************************************************************************************************
 */

#pragma once

namespace Vkgc {

static const char *const VkgcSupportedCapabilities[] = {
    "CapabilityMatrix",
    "CapabilityShader",
    "CapabilityGeometry",
    "CapabilityTessellation",
    "CapabilityFloat16",
    "CapabilityFloat64",
    "CapabilityInt64",
    "CapabilityMeshShadingEXT",
    "CapabilityFragmentShaderSampleInterlockEXT",
    "CapabilityFragmentFullyCoveredEXT",
    "CapabilityFragmentShaderShadingRateInterlockEXT",
    "CapabilityInputAttachmentArrayNonUniformIndexingEXT",
    "CapabilityAtomicStorageOps",
    "CapabilityFragmentShaderPixelInterlockEXT",
    "CapabilityDotProductKHR",
    "CapabilityDotProductInputAllKHR",
    "CapabilityDotProductInput4x8BitKHR",
    "CapabilityDotProductInput4x8BitPackedKHR",
    "CapabilityWorkgroupMemoryExplicitLayoutKHR",
    "CapabilityWorkgroupMemoryExplicitLayout8BitAccessKHR",
    "CapabilityWorkgroupMemoryExplicitLayout16BitAccessKHR",
    "CapabilityInt64Atomics",
    "CapabilityGroups",
    "CapabilityAtomicStorage",
    "CapabilityInt16",
    "CapabilityTessellationPointSize",
    "CapabilityGeometryPointSize",
    "CapabilityImageGatherExtended",
    "CapabilityStorageImageMultisample",
    "CapabilityUniformBufferArrayDynamicIndexing",
    "CapabilitySampledImageArrayDynamicIndexing",
    "CapabilityStorageBufferArrayDynamicIndexing",
    "CapabilityStorageImageArrayDynamicIndexing",
    "CapabilityClipDistance",
    "CapabilityCullDistance",
    "CapabilityImageCubeArray",
    "CapabilitySampleRateShading",
    "CapabilityImageRect",
    "CapabilitySampledRect",
    "CapabilityInt8",
    "CapabilityInputAttachment",
    "CapabilitySparseResidency",
    "CapabilityMinLod",
    "CapabilitySampled1D",
    "CapabilityImage1D",
    "CapabilitySampledCubeArray",
    "CapabilitySampledBuffer",
    "CapabilityImageBuffer",
    "CapabilityImageMSArray",
    "CapabilityStorageImageExtendedFormats",
    "CapabilityImageQuery",
    "CapabilityDerivativeControl",
    "CapabilityInterpolationFunction",
    "CapabilityTransformFeedback",
    "CapabilityGeometryStreams",
    "CapabilityStorageImageReadWithoutFormat",
    "CapabilityStorageImageWriteWithoutFormat",
    "CapabilityMultiViewport",
    "CapabilityGroupNonUniform",
    "CapabilityGroupNonUniformVote",
    "CapabilityGroupNonUniformArithmetic",
    "CapabilityGroupNonUniformBallot",
    "CapabilityGroupNonUniformShuffle",
    "CapabilityGroupNonUniformShuffleRelative",
    "CapabilityGroupNonUniformClustered",
    "CapabilityGroupNonUniformQuad",
    "CapabilitySubgroupBallotKHR",
    "CapabilityDrawParameters",
    "CapabilitySubgroupVoteKHR",
    "CapabilityStorageBuffer16BitAccess",
    "CapabilityStorageUniformBufferBlock16",
    "CapabilityStorageUniform16",
    "CapabilityUniformAndStorageBuffer16BitAccess",
    "CapabilityStorageInputOutput16",
    "CapabilityDeviceGroup",
    "CapabilityMultiView",
    "CapabilityVariablePointersStorageBuffer",
    "CapabilityVariablePointers",
    "CapabilitySampleMaskPostDepthCoverage",
    "CapabilityStorageBuffer8BitAccess",
    "CapabilityUniformAndStorageBuffer8BitAccess",
    "CapabilityDenormPreserve",
    "CapabilityDenormFlushToZero",
    "CapabilitySignedZeroInfNanPreserve",
    "CapabilityRoundingModeRTE",
    "CapabilityRoundingModeRTZ",
    "CapabilityFloat16ImageAMD",
    "CapabilityImageGatherBiasLodAMD",
    "CapabilityFragmentMaskAMD",
    "CapabilityStencilExportEXT",
    "CapabilityImageReadWriteLodAMD",
    "CapabilityInt64ImageEXT",
    "CapabilityShaderClockKHR",
    "CapabilityShaderViewportIndexLayerEXT",
    "CapabilityFragmentShadingRateKHR",
    "CapabilityFragmentDensityEXT",
    "CapabilityShaderNonUniformEXT",
    "CapabilityRuntimeDescriptorArrayEXT",
    "CapabilityInputAttachmentArrayDynamicIndexingEXT",
    "CapabilityUniformTexelBufferArrayDynamicIndexingEXT",
    "CapabilityStorageTexelBufferArrayDynamicIndexingEXT",
    "CapabilityUniformBufferArrayNonUniformIndexingEXT",
    "CapabilitySampledImageArrayNonUniformIndexingEXT",
    "CapabilityStorageBufferArrayNonUniformIndexingEXT",
    "CapabilityStorageImageArrayNonUniformIndexingEXT",
    "CapabilityUniformTexelBufferArrayNonUniformIndexingEXT",
    "CapabilityStorageTexelBufferArrayNonUniformIndexingEXT",
    "CapabilityVulkanMemoryModel",
    "CapabilityVulkanMemoryModelKHR",
    "CapabilityVulkanMemoryModelDeviceScope",
    "CapabilityVulkanMemoryModelDeviceScopeKHR",
    "CapabilityPhysicalStorageBufferAddresses",
    "CapabilityPhysicalStorageBufferAddressesEXT",
    "CapabilityDemoteToHelperInvocationEXT",
    "CapabilityAtomicFloat32MinMaxEXT",
    "CapabilityAtomicFloat64MinMaxEXT",
    "CapabilityRayTracingNV",
    "CapabilityRayQueryKHR",
    "CapabilityRayTracingKHR",
    "CapabilityRayCullMaskKHR",
    "CapabilityRayTracingProvisionalKHR",
    "CapabilityRayQueryProvisionalKHR",
    "CapabilityRayTraversalPrimitiveCullingProvisionalKHR",
    "CapabilityRayTracingPositionFetchKHR",
    "CapabilityRayQueryPositionFetchKHR",
};

}; // namespace Vkgc
