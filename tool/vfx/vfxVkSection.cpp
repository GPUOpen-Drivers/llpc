#include "vfxEnumsConverter.h"
#include "vfxSection.h"

#if VFX_SUPPORT_VK_PIPELINE
#include "vfxVkSection.h"

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
#if VKI_RAY_TRACING
    INIT_SECTION_INFO("RayTracingPipelineState", SectionTypeRayTracingState, 0)
    INIT_SECTION_INFO("RtState", SectionTypeRtState, 0)
#endif
    INIT_SECTION_INFO("VertexInputState", SectionTypeVertexInputState, 0)
    INIT_SECTION_INFO("TaskInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTask)
    INIT_SECTION_INFO("VsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageVertex)
    INIT_SECTION_INFO("TcsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTessControl)
    INIT_SECTION_INFO("TesInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageTessEval)
    INIT_SECTION_INFO("GsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageGeometry)
    INIT_SECTION_INFO("MeshInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageMesh)
    INIT_SECTION_INFO("FsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageFragment)
    INIT_SECTION_INFO("CsInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
    INIT_SECTION_INFO("rgenInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingRayGen)
    INIT_SECTION_INFO("sectInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingIntersect)
    INIT_SECTION_INFO("ahitInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingAnyHit)
    INIT_SECTION_INFO("chitInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingClosestHit)
    INIT_SECTION_INFO("missInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingMiss)
    INIT_SECTION_INFO("callInfo", SectionTypeShaderInfo, ShaderStage::ShaderStageRayTracingCallable)
#endif
    INIT_SECTION_INFO("ResourceMapping", SectionTypeResourceMapping, 0)
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
  }
};

// =====================================================================================================================
// Initialize VK pipeline special sections.
void initVkSections() {
  static VkSectionParserInit init;
}

} // namespace Vfx
#endif
