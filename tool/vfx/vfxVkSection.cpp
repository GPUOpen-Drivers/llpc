/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

#include "vfxEnumsConverter.h"
#include "vfxError.h"
#include "vfxSection.h"

#if VFX_SUPPORT_VK_PIPELINE
#include "vfxVkSection.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

using namespace Vkgc;

namespace Vfx {

// =====================================================================================================================
// Dummy class used to initialize all VK special sections
class VkSectionParserInit {
public:
  VkSectionParserInit() {
    initEnumMap();

    // Sections for PipelineDocument
    INIT_SECTION_INFO("GraphicsPipelineState", SectionTypeGraphicsState, 0)
    INIT_SECTION_INFO("ComputePipelineState", SectionTypeComputeState, 0)
    INIT_SECTION_INFO("RayTracingPipelineState", SectionTypeRayTracingState, 0)
    INIT_SECTION_INFO("RtState", SectionTypeRtState, 0)
    INIT_SECTION_INFO("RayTracingLibrarySummary", SectionTypeRayTracingLibrarySummary, 0)
    INIT_SECTION_INFO("VertexInputState", SectionTypeVertexInputState, 0)
    INIT_SECTION_INFO("TaskInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTask)
    INIT_SECTION_INFO("VsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageVertex)
    INIT_SECTION_INFO("TcsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTessControl)
    INIT_SECTION_INFO("TesInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTessEval)
    INIT_SECTION_INFO("GsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageGeometry)
    INIT_SECTION_INFO("MeshInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageMesh)
    INIT_SECTION_INFO("FsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageFragment)
    INIT_SECTION_INFO("CsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageCompute)
    INIT_SECTION_INFO("rgenInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingRayGen)
    INIT_SECTION_INFO("sectInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingIntersect)
    INIT_SECTION_INFO("ahitInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingAnyHit)
    INIT_SECTION_INFO("chitInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingClosestHit)
    INIT_SECTION_INFO("missInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingMiss)
    INIT_SECTION_INFO("callInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingCallable)
    INIT_SECTION_INFO("ResourceMapping", SectionTypeResourceMapping, 0)
    INIT_SECTION_INFO("GraphicsLibrary", SectionTypeGraphicsLibrary, 0)
  };

  void initEnumMap() {
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorResource)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorSampler)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorYCbCrSampler)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorCombinedTexture)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorTexelBuffer)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorFmask)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorBuffer)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorTableVaPtr)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, IndirectUserDataVaPtr)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, PushConst)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorBufferCompact)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, StreamOutTableVaPtr)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorConstBuffer)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorConstBufferCompact)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorImage)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorConstTexelBuffer)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, InlineBuffer)
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 63
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorAtomicCounter)
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, DescriptorMutable)
#endif
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, Auto)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, MaximumSize)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, HalfSize)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, OptimizeForVerts)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, OptimizeForPrims)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, Explicit)

    ADD_CLASS_ENUM_MAP(WaveBreakSize, None)
    ADD_CLASS_ENUM_MAP(WaveBreakSize, _8x8)
    ADD_CLASS_ENUM_MAP(WaveBreakSize, _16x16)
    ADD_CLASS_ENUM_MAP(WaveBreakSize, _32x32)

    ADD_CLASS_ENUM_MAP(ShadowDescriptorTableUsage, Auto)
    ADD_CLASS_ENUM_MAP(ShadowDescriptorTableUsage, Enable)
    ADD_CLASS_ENUM_MAP(ShadowDescriptorTableUsage, Disable)

    ADD_CLASS_ENUM_MAP(DenormalMode, Auto)
    ADD_CLASS_ENUM_MAP(DenormalMode, FlushToZero)
    ADD_CLASS_ENUM_MAP(DenormalMode, Preserve)

    ADD_CLASS_ENUM_MAP(ResourceLayoutScheme, Compact)
    ADD_CLASS_ENUM_MAP(ResourceLayoutScheme, Indirect)

    ADD_CLASS_ENUM_MAP(ThreadGroupSwizzleMode, Default)
    ADD_CLASS_ENUM_MAP(ThreadGroupSwizzleMode, _4x4)
    ADD_CLASS_ENUM_MAP(ThreadGroupSwizzleMode, _8x8)
    ADD_CLASS_ENUM_MAP(ThreadGroupSwizzleMode, _16x16)

    ADD_CLASS_ENUM_MAP(InvariantLoads, Auto)
    ADD_CLASS_ENUM_MAP(InvariantLoads, EnableOptimization)
    ADD_CLASS_ENUM_MAP(InvariantLoads, DisableOptimization)
    ADD_CLASS_ENUM_MAP(InvariantLoads, ClearInvariants)

    ADD_CLASS_ENUM_MAP(LlvmScheduleStrategy, None)
    ADD_CLASS_ENUM_MAP(LlvmScheduleStrategy, MaxIlp)
    ADD_CLASS_ENUM_MAP(LlvmScheduleStrategy, MaxMemoryClause)
  }
};

// =====================================================================================================================
// Initialize VK pipeline special sections.
void initVkSections() {
  static VkSectionParserInit init;
}

// =====================================================================================================================
// Convert the raytracing library summary from YAML to msgpack
Vkgc::BinaryData SectionRayTracingLibrarySummary::getSubState() {
  std::string errorMsgTmp;
  llvm::msgpack::Document doc;
  if (!doc.fromYAML(m_yaml))
    PARSE_ERROR(errorMsgTmp, getLineNum(), "Failed to parse YAML for raytracing library summary");
  doc.writeToBlob(m_msgpack);

  Vkgc::BinaryData data;
  data.pCode = m_msgpack.data();
  data.codeSize = m_msgpack.size();
  return data;
}

// =====================================================================================================================
// Parse the RT IP version
bool SectionRtState::parseRtIpVersion(RtIpVersion *rtIpVersion) {
  if (m_rtIpVersion.empty())
    return true;

  const char *p = m_rtIpVersion.c_str();
  if (!isdigit(*p))
    return false;

  char *np;
  rtIpVersion->major = strtol(p, &np, 10);
  if (p == np || *np != '.')
    return false;

  p = np + 1;
  if (!isdigit(*p))
    return false;

  rtIpVersion->minor = strtol(p, &np, 10);
  return *np == 0;
}

} // namespace Vfx
#endif
