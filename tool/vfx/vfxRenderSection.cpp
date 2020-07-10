#include "vfxEnumsConverter.h"
#include "vfxSection.h"

#if VFX_SUPPORT_RENDER_DOCOUMENT
#include "vfxRenderSection.h"

namespace Vfx {

StrToMemberAddr SectionResultItem::m_addrTable[SectionResultItem::MemberCount];
StrToMemberAddr SectionResult::m_addrTable[SectionResult::MemberCount];
StrToMemberAddr SectionVertexBufferBinding::m_addrTable[SectionVertexBufferBinding::MemberCount];
StrToMemberAddr SectionVertexAttribute::m_addrTable[SectionVertexAttribute::MemberCount];
StrToMemberAddr SectionVertexState::m_addrTable[SectionVertexState::MemberCount];
StrToMemberAddr SectionBufferView::m_addrTable[SectionBufferView::MemberCount];
StrToMemberAddr SectionDrawState::m_addrTable[SectionDrawState::MemberCount];
StrToMemberAddr SectionPushConstRange::m_addrTable[SectionPushConstRange::MemberCount];
StrToMemberAddr SectionImageView::m_addrTable[SectionImageView::MemberCount];
StrToMemberAddr SectionSampler::m_addrTable[SectionSampler::MemberCount];

// =====================================================================================================================
// Dummy class used to initialize VFX render document special sections
class RenderSectionParserInit {
public:
  RenderSectionParserInit() {
    initEnumMap();

    // Sections for RenderDocument
    INIT_SECTION_INFO("Result", SectionTypeResult, 0)
    INIT_SECTION_INFO("BufferView", SectionTypeBufferView, 0)
    INIT_SECTION_INFO("VertexState", SectionTypeVertexState, 0)
    INIT_SECTION_INFO("DrawState", SectionTypeDrawState, 0)
    INIT_SECTION_INFO("ImageView", SectionTypeImageView, 0)
    INIT_SECTION_INFO("Sampler", SectionTypeSampler, 0)

    SectionResultItem::initialAddrTable();
    SectionResult::initialAddrTable();
    SectionVertexBufferBinding::initialAddrTable();
    SectionVertexAttribute::initialAddrTable();
    SectionVertexState::initialAddrTable();
    SectionBufferView::initialAddrTable();
    SectionDrawState::initialAddrTable();
    SectionPushConstRange::initialAddrTable();
    SectionImageView::initialAddrTable();
    SectionSampler::initialAddrTable();
  };

  // Add Vfx enums
  void initEnumMap() {
    ADD_ENUM_MAP(ResultSource, ResultSourceColor)
    ADD_ENUM_MAP(ResultSource, ResultSourceDepthStencil)
    ADD_ENUM_MAP(ResultSource, ResultSourceBuffer)

    ADD_ENUM_MAP(ResultCompareMethod, ResultCompareMethodEqual)
    ADD_ENUM_MAP(ResultCompareMethod, ResultCompareMethodNotEqual)

    ADD_ENUM_MAP(SamplerPattern, SamplerNearest)
    ADD_ENUM_MAP(SamplerPattern, SamplerLinear)
    ADD_ENUM_MAP(SamplerPattern, SamplerNearestMipNearest)
    ADD_ENUM_MAP(SamplerPattern, SamplerLinearMipLinear)

    ADD_ENUM_MAP(ImagePattern, ImageCheckBoxUnorm)
    ADD_ENUM_MAP(ImagePattern, ImageCheckBoxFloat)
    ADD_ENUM_MAP(ImagePattern, ImageCheckBoxDepth)
    ADD_ENUM_MAP(ImagePattern, ImageLinearUnorm)
    ADD_ENUM_MAP(ImagePattern, ImageLinearFloat)
    ADD_ENUM_MAP(ImagePattern, ImageLinearDepth)
    ADD_ENUM_MAP(ImagePattern, ImageSolidUnorm)
    ADD_ENUM_MAP(ImagePattern, ImageSolidFloat)
    ADD_ENUM_MAP(ImagePattern, ImageSolidDepth)
  }
};

// =====================================================================================================================
// Initialize Render document special sections.
void initRenderSections() {
  static RenderSectionParserInit init;
}

} // namespace Vfx
#endif
