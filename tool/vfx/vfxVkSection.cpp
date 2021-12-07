#include "vfxEnumsConverter.h"
#include "vfxSection.h"

#if VFX_SUPPORT_VK_PIPELINE
#include "vfxVkSection.h"

using namespace Vkgc;

namespace Vfx {

StrToMemberAddr SectionDescriptorRangeValueItem::m_addrTable[SectionDescriptorRangeValueItem::MemberCount];
StrToMemberAddr SectionResourceMappingNode::m_addrTable[SectionResourceMappingNode::MemberCount];
StrToMemberAddr SectionShaderInfo::m_addrTable[SectionShaderInfo::MemberCount];
StrToMemberAddr SectionResourceMapping::m_addrTable[SectionResourceMapping::MemberCount];
StrToMemberAddr SectionGraphicsState::m_addrTable[SectionGraphicsState::MemberCount];
StrToMemberAddr SectionComputeState::m_addrTable[SectionComputeState::MemberCount];
StrToMemberAddr SectionPipelineOption::m_addrTable[SectionPipelineOption::MemberCount];
StrToMemberAddr SectionShaderOption::m_addrTable[SectionShaderOption::MemberCount];
StrToMemberAddr SectionNggState::m_addrTable[SectionNggState::MemberCount];
StrToMemberAddr SectionExtendedRobustness::m_addrTable[SectionExtendedRobustness::MemberCount];

// =====================================================================================================================
// Dummy class used to initialize all VK special sections
class VkSectionParserInit {
public:
  VkSectionParserInit() {
    initEnumMap();

    // Sections for PipelineDocument
    INIT_SECTION_INFO("GraphicsPipelineState", SectionTypeGraphicsState, 0)
    INIT_SECTION_INFO("ComputePipelineState", SectionTypeComputeState, 0)
    INIT_SECTION_INFO("VertexInputState", SectionTypeVertexInputState, 0)
    INIT_SECTION_INFO("VsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageVertex)
    INIT_SECTION_INFO("TcsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTessControl)
    INIT_SECTION_INFO("TesInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTessEval)
    INIT_SECTION_INFO("GsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageGeometry)
    INIT_SECTION_INFO("FsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageFragment)
    INIT_SECTION_INFO("CsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageCompute)
    INIT_SECTION_INFO("ResourceMapping", SectionTypeResourceMapping, 0)

    SectionGraphicsState::initialAddrTable();
    SectionComputeState::initialAddrTable();
    SectionDescriptorRangeValueItem::initialAddrTable();
    SectionResourceMappingNode::initialAddrTable();
    SectionShaderInfo::initialAddrTable();
    SectionResourceMapping::initialAddrTable();
    SectionPipelineOption::initialAddrTable();
    SectionShaderOption::initialAddrTable();
    SectionNggState::initialAddrTable();
    SectionExtendedRobustness::initialAddrTable();
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
#if  (LLPC_CLIENT_INTERFACE_MAJOR_VERSION>= 50)
    ADD_CLASS_ENUM_MAP(ResourceMappingNodeType, InlineBuffer)
#endif
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, Auto)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, MaximumSize)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, HalfSize)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, OptimizeForVerts)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, OptimizeForPrims)
    ADD_CLASS_ENUM_MAP(NggSubgroupSizingType, Explicit)

    ADD_ENUM_MAP(NggCompactMode, NggCompactDisable)
    ADD_ENUM_MAP(NggCompactMode, NggCompactVertices)

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
  }
};

// =====================================================================================================================
// Initialize VK pipeline special sections.
void initVkSections() {
  static VkSectionParserInit init;
}

} // namespace Vfx
#endif
