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
* @file  vfxSection.cpp
* @brief Contains implmentation of class Section and derived classes
***********************************************************************************************************************
*/

#include <inttypes.h>
#include "vfxEnumsConverter.h"
#include "vfxSection.h"

#if VFX_INSIDE_SPVGEN
#define SH_EXPORTING
#endif
#include "spvgen.h"

namespace Vfx
{

// =====================================================================================================================
// Static variables in class Section and derived class
std::map<std::string, SectionInfo> Section::m_sectionInfo;

StrToMemberAddr SectionResultItem::m_addrTable[SectionResultItem::MemberCount];
StrToMemberAddr SectionResult::m_addrTable[SectionResult::MemberCount];
StrToMemberAddr SectionSpecConstItem::m_addrTable[SectionSpecConstItem::MemberCount];
StrToMemberAddr SectionSpecConst::m_addrTable[SectionSpecConst::MemberCount];
StrToMemberAddr SectionVertexBufferBinding::m_addrTable[SectionVertexBufferBinding::MemberCount];
StrToMemberAddr SectionVertexAttribute::m_addrTable[SectionVertexAttribute::MemberCount];
StrToMemberAddr SectionVertexState::m_addrTable[SectionVertexState::MemberCount];
StrToMemberAddr SectionBufferView::m_addrTable[SectionBufferView::MemberCount];
StrToMemberAddr SectionDrawState::m_addrTable[SectionDrawState::MemberCount];
StrToMemberAddr SectionPushConstRange::m_addrTable[SectionPushConstRange::MemberCount];
StrToMemberAddr SectionImageView::m_addrTable[SectionImageView::MemberCount];
StrToMemberAddr SectionSampler::m_addrTable[SectionSampler::MemberCount];
StrToMemberAddr SectionShader::m_addrTable[SectionShader::MemberCount];
StrToMemberAddr SectionColorBuffer::m_addrTable[SectionColorBuffer::MemberCount];
StrToMemberAddr SectionGraphicsState::m_addrTable[SectionGraphicsState::MemberCount];
StrToMemberAddr SectionComputeState::m_addrTable[SectionComputeState::MemberCount];
StrToMemberAddr SectionVertexInputBinding::m_addrTable[SectionVertexInputBinding::MemberCount];
StrToMemberAddr SectionVertexInputAttribute::m_addrTable[SectionVertexInputAttribute::MemberCount];
StrToMemberAddr SectionVertexInputDivisor::m_addrTable[SectionVertexInputDivisor::MemberCount];
StrToMemberAddr SectionVertexInput::m_addrTable[SectionVertexInput::MemberCount];
StrToMemberAddr SectionSpecEntryItem::m_addrTable[SectionSpecEntryItem::MemberCount];
StrToMemberAddr SectionSpecInfo::m_addrTable[SectionSpecInfo::MemberCount];
StrToMemberAddr SectionDescriptorRangeValueItem::m_addrTable[SectionDescriptorRangeValueItem::MemberCount];
StrToMemberAddr SectionResourceMappingNode::m_addrTable[SectionResourceMappingNode::MemberCount];
StrToMemberAddr SectionShaderInfo::m_addrTable[SectionShaderInfo::MemberCount];
StrToMemberAddr SectionVersion::m_addrTable[SectionVersion::MemberCount];
StrToMemberAddr SectionCompileLog::m_addrTable[SectionCompileLog::MemberCount];
StrToMemberAddr SectionPipelineOption::m_addrTable[SectionPipelineOption::MemberCount];
StrToMemberAddr SectionShaderOption::m_addrTable[SectionShaderOption::MemberCount];
StrToMemberAddr SectionNggState::m_addrTable[SectionNggState::MemberCount];

// =====================================================================================================================
// Dummy class used to initialize all static variables
class ParserInit
{
public:
    ParserInit()
    {
        InitEnumMap();

        Section::InitSectionInfo();

        SectionResultItem::InitialAddrTable();
        SectionResult::InitialAddrTable();
        SectionSpecConstItem::InitialAddrTable();
        SectionSpecConst::InitialAddrTable();
        SectionVertexBufferBinding::InitialAddrTable();
        SectionVertexAttribute::InitialAddrTable();
        SectionVertexState::InitialAddrTable();
        SectionBufferView::InitialAddrTable();
        SectionDrawState::InitialAddrTable();
        SectionPushConstRange::InitialAddrTable();
        SectionImageView::InitialAddrTable();
        SectionSampler::InitialAddrTable();
        SectionVersion::InitialAddrTable();
        SectionCompileLog::InitialAddrTable();
        SectionShader::InitialAddrTable();
        SectionColorBuffer::InitialAddrTable();
        SectionGraphicsState::InitialAddrTable();
        SectionComputeState::InitialAddrTable();
        SectionVertexInputBinding::InitialAddrTable();
        SectionVertexInputAttribute::InitialAddrTable();
        SectionVertexInputDivisor::InitialAddrTable();
        SectionVertexInput::InitialAddrTable();
        SectionSpecEntryItem::InitialAddrTable();
        SectionSpecInfo::InitialAddrTable();
        SectionDescriptorRangeValueItem::InitialAddrTable();
        SectionResourceMappingNode::InitialAddrTable();
        SectionShaderInfo::InitialAddrTable();
        SectionPipelineOption::InitialAddrTable();
        SectionShaderOption::InitialAddrTable();
        SectionNggState::InitialAddrTable();

    };
};

static ParserInit g_init;

// =====================================================================================================================
// Constructs an Section object.
Section::Section(
    StrToMemberAddr* pAddrTable,    // [in] Table to map member name to member address
    unsigned         tableSize,     // Size of above table
    SectionType      sectionType,   // Section type
    const char*      pSectionName)  // [in] Name of this section.
    :
    m_sectionType(sectionType),
    m_pSectionName(pSectionName),
    m_lineNum(0),
    m_pMemberTable(pAddrTable),
    m_tableSize(tableSize),
    m_isActive(false)
{

};

// =====================================================================================================================
// Initializes static variable m_sectionInfo
void Section::InitSectionInfo()
{
    // Shader sections
    INIT_SECTION_INFO("VertexShaderGlsl", SectionTypeVertexShader, Glsl)
    INIT_SECTION_INFO("TessControlShaderGlsl", SectionTypeTessControlShader, Glsl)
    INIT_SECTION_INFO("TessEvalShaderGlsl", SectionTypeTessEvalShader, Glsl)
    INIT_SECTION_INFO("GeometryShaderGlsl", SectionTypeGeometryShader, Glsl)
    INIT_SECTION_INFO("FragmentShaderGlsl", SectionTypeFragmentShader, Glsl)
    INIT_SECTION_INFO("ComputeShaderGlsl", SectionTypeComputeShader, Glsl)

    INIT_SECTION_INFO("VertexShaderSpirv", SectionTypeVertexShader, SpirvAsm)
    INIT_SECTION_INFO("TessControlShaderSpirv", SectionTypeTessControlShader, SpirvAsm)
    INIT_SECTION_INFO("TessEvalShaderSpirv", SectionTypeTessEvalShader, SpirvAsm)
    INIT_SECTION_INFO("GeometryShaderSpirv", SectionTypeGeometryShader, SpirvAsm)
    INIT_SECTION_INFO("FragmentShaderSpirv", SectionTypeFragmentShader, SpirvAsm)
    INIT_SECTION_INFO("ComputeShaderSpirv", SectionTypeComputeShader, SpirvAsm)

    INIT_SECTION_INFO("VsGlsl", SectionTypeVertexShader, Glsl)
    INIT_SECTION_INFO("TcsGlsl", SectionTypeTessControlShader, Glsl)
    INIT_SECTION_INFO("TesGlsl", SectionTypeTessEvalShader, Glsl)
    INIT_SECTION_INFO("GsGlsl", SectionTypeGeometryShader, Glsl)
    INIT_SECTION_INFO("FsGlsl", SectionTypeFragmentShader, Glsl)
    INIT_SECTION_INFO("CsGlsl", SectionTypeComputeShader, Glsl)

    INIT_SECTION_INFO("VsSpirv", SectionTypeVertexShader, SpirvAsm)
    INIT_SECTION_INFO("TcsSpirv", SectionTypeTessControlShader, SpirvAsm)
    INIT_SECTION_INFO("TesSpirv", SectionTypeTessEvalShader, SpirvAsm)
    INIT_SECTION_INFO("GsSpirv", SectionTypeGeometryShader, SpirvAsm)
    INIT_SECTION_INFO("FsSpirv", SectionTypeFragmentShader, SpirvAsm)
    INIT_SECTION_INFO("CsSpirv", SectionTypeComputeShader, SpirvAsm)

    INIT_SECTION_INFO("VsGlslFile", SectionTypeVertexShader, GlslFile)
    INIT_SECTION_INFO("TcsGlslFile", SectionTypeTessControlShader, GlslFile)
    INIT_SECTION_INFO("TesGlslFile", SectionTypeTessEvalShader, GlslFile)
    INIT_SECTION_INFO("GsGlslFile", SectionTypeGeometryShader, GlslFile)
    INIT_SECTION_INFO("FsGlslFile", SectionTypeFragmentShader, GlslFile)
    INIT_SECTION_INFO("CsGlslFile", SectionTypeComputeShader, GlslFile)

    INIT_SECTION_INFO("VsSpvFile", SectionTypeVertexShader, SpirvFile)
    INIT_SECTION_INFO("TcsSpvFile", SectionTypeTessControlShader, SpirvFile)
    INIT_SECTION_INFO("TesSpvFile", SectionTypeTessEvalShader, SpirvFile)
    INIT_SECTION_INFO("GsSpvFile", SectionTypeGeometryShader, SpirvFile)
    INIT_SECTION_INFO("FsSpvFile", SectionTypeFragmentShader, SpirvFile)
    INIT_SECTION_INFO("CsSpvFile", SectionTypeComputeShader, SpirvFile)

    INIT_SECTION_INFO("VsSpvasmFile", SectionTypeVertexShader, SpirvAsmFile)
    INIT_SECTION_INFO("TcsSpvasmFile", SectionTypeTessControlShader, SpirvAsmFile)
    INIT_SECTION_INFO("TesSpvasmFile", SectionTypeTessEvalShader, SpirvAsmFile)
    INIT_SECTION_INFO("GsSpvasmFile", SectionTypeGeometryShader, SpirvAsmFile)
    INIT_SECTION_INFO("FsSpvasmFile", SectionTypeFragmentShader, SpirvAsmFile)
    INIT_SECTION_INFO("CsSpvasmFile", SectionTypeComputeShader, SpirvAsmFile)

    INIT_SECTION_INFO("VsHlsl", SectionTypeVertexShader, Hlsl)
    INIT_SECTION_INFO("TcsHlsl", SectionTypeTessControlShader, Hlsl)
    INIT_SECTION_INFO("TesHlsl", SectionTypeTessEvalShader, Hlsl)
    INIT_SECTION_INFO("GsHlsl", SectionTypeGeometryShader, Hlsl)
    INIT_SECTION_INFO("FsHlsl", SectionTypeFragmentShader, Hlsl)
    INIT_SECTION_INFO("CsHlsl", SectionTypeComputeShader, Hlsl)

    INIT_SECTION_INFO("VsHlslFile", SectionTypeVertexShader, HlslFile)
    INIT_SECTION_INFO("TcsHlslFile", SectionTypeTessControlShader, HlslFile)
    INIT_SECTION_INFO("TesHlslFile", SectionTypeTessEvalShader, HlslFile)
    INIT_SECTION_INFO("GsHlslFile", SectionTypeGeometryShader, HlslFile)
    INIT_SECTION_INFO("FsHlslFile", SectionTypeFragmentShader, HlslFile)
    INIT_SECTION_INFO("CsHlslFile", SectionTypeComputeShader, HlslFile)

    INIT_SECTION_INFO("Version", SectionTypeVersion, 0)
    INIT_SECTION_INFO("CompileLog", SectionTypeCompileLog, 0)

    // Sections for RenderDocument
    INIT_SECTION_INFO("Result", SectionTypeResult, 0)
    INIT_SECTION_INFO("BufferView", SectionTypeBufferView, 0)
    INIT_SECTION_INFO("VertexState", SectionTypeVertexState, 0)
    INIT_SECTION_INFO("DrawState", SectionTypeDrawState, 0)
    INIT_SECTION_INFO("ImageView", SectionTypeImageView, 0)
    INIT_SECTION_INFO("Sampler", SectionTypeSampler, 0)

    // Sections for PipelineDocument
    INIT_SECTION_INFO("GraphicsPipelineState", SectionTypeGraphicsState, 0)
    INIT_SECTION_INFO("ComputePipelineState", SectionTypeComputeState, 0)
    INIT_SECTION_INFO("VertexInputState", SectionTypeVertexInputState, 0)
    INIT_SECTION_INFO("VsInfo", SectionTypeVertexShaderInfo, 0)
    INIT_SECTION_INFO("TcsInfo", SectionTypeTessControlShaderInfo, 0)
    INIT_SECTION_INFO("TesInfo", SectionTypeTessEvalShaderInfo, 0)
    INIT_SECTION_INFO("GsInfo", SectionTypeGeometryShaderInfo, 0)
    INIT_SECTION_INFO("FsInfo", SectionTypeFragmentShaderInfo, 0)
    INIT_SECTION_INFO("CsInfo", SectionTypeComputeShaderInfo, 0)
}

// =====================================================================================================================
// Gets data type of a member.
bool Section::GetMemberType(
    unsigned     lineNum,         // Line No.
    const char*  memberName,     // [in]  Member string name
    MemberType*  pValueType,      // [out] Member data type.
    std::string* errorMsg)       // [out] Error message
{
    bool result = false;
    for (unsigned i = 0; i < m_tableSize; ++i)
    {
        if ((m_pMemberTable[i].memberName != nullptr) && strcmp(memberName, m_pMemberTable[i].memberName) == 0)
        {
            result = true;

            if (pValueType != nullptr)
            {
                *pValueType = m_pMemberTable[i].memberType;
            }

            break;
        }
    }

    if (result == false)
    {
        PARSE_WARNING(*errorMsg, lineNum, "Invalid member name: %s", memberName);
    }

    return result;
}

// =====================================================================================================================
// Is this member a section object.
bool Section::IsSection(
    unsigned     lineNum,        // Line number
    const char*  memberName,    // [in] Member name
    bool*        pOutput,        // [out] Is this memeber a section object
    MemberType*  pType,          // [out] Object type
    std::string* errorMsg)      // [out] Error message
{
    bool result = false;

    for (unsigned i = 0; i < m_tableSize; ++i)
    {
        if ((m_pMemberTable[i].memberName != nullptr) && strcmp(memberName, m_pMemberTable[i].memberName) == 0)
        {
            result = true;
            if (pOutput != nullptr)
            {
                *pOutput = m_pMemberTable[i].isSection;
            }

            if (pType != nullptr)
            {
                *pType = m_pMemberTable[i].memberType;
            }
            break;
        }
    }

    if (result == false)
    {
        PARSE_WARNING(*errorMsg, lineNum, "Invalid member name: %s", memberName);
    }

    return result;
}

// =====================================================================================================================
// Prints all data in this object, for debug purpose.
void Section::PrintSelf(
    unsigned level)     // Nest level from the base object
{
    if (m_isActive == true)
    {
        for (unsigned l = 0; l < level; ++l) { printf("\t"); }
        printf("[%s]\n", m_pSectionName);
        for (unsigned i = 0; i < m_tableSize; ++i)
        {
            if (m_pMemberTable[i].memberName != nullptr)
            {
                continue;
            }
            for (unsigned arrayIndex = 0; arrayIndex < m_pMemberTable[i].arrayMaxSize; ++arrayIndex)
            {
                if (m_pMemberTable[i].isSection)
                {
                    Section* pSubObj;
                    std::string dummyMsg;
                    if (GetPtrOfSubSection(0,
                                           m_pMemberTable[i].memberName,
                                           m_pMemberTable[i].memberType,
                                           false,
                                           arrayIndex,
                                           &pSubObj,
                                           &dummyMsg))
                    {
                        if (pSubObj->m_isActive == true)
                        {
                            pSubObj->PrintSelf(level + 1);
                        }
                    }
                }
                else
                {
                    for (unsigned l = 0; l < level; ++l) { printf("\t"); }
                    int tempValue = *(((int*)(GetMemberAddr(i))) + arrayIndex);
                    if (static_cast<unsigned>(tempValue) != VfxInvalidValue)
                    {
                        switch (m_pMemberTable[i].memberType)
                        {
                        case MemberTypeEnum:
                        case MemberTypeInt:
                        {
                            printf("%s = %d\n",
                                m_pMemberTable[i].memberName,
                                *(((int*)(GetMemberAddr(i))) + arrayIndex));
                            break;
                        }
                        case MemberTypeBool:
                        {
                            printf("%s = %d\n",
                                   m_pMemberTable[i].memberName,
                                   *(((bool*)(GetMemberAddr(i))) + arrayIndex));
                            break;
                        }
                        case MemberTypeFloat:
                        {
                            printf("%s = %.3f\n",
                                   m_pMemberTable[i].memberName,
                                   *(((float*)(GetMemberAddr(i))) + arrayIndex));
                            break;
                        }
                        case MemberTypeFloat16:
                        {
                            float v = (((Float16*)(GetMemberAddr(i))) + arrayIndex)->GetValue();
                            printf("%s = %.3fhf\n", m_pMemberTable[i].memberName, v);
                            break;
                        }
                        case MemberTypeDouble:
                        {
                            printf("%s = %.3f\n", m_pMemberTable[i].memberName, *(((double*)(GetMemberAddr(i))) + arrayIndex));
                            break;
                        }
                        case MemberTypeIVec4:
                        {
                            IUFValue* pIUFValue = static_cast<IUFValue*>(GetMemberAddr(i));
                            pIUFValue += arrayIndex;

                            if ((pIUFValue->props.isDouble == false) && (pIUFValue->props.isFloat == false))
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned j = 0; j < pIUFValue->props.length; ++j)
                                {
                                    if (pIUFValue->props.isHex == true)
                                    {
                                        printf(" 0x%x", pIUFValue->iVec4[j]);
                                    }
                                    else
                                    {
                                        printf(" %d", pIUFValue->iVec4[j]);
                                    }
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeI64Vec2:
                        {
                            IUFValue* pIUFValue = static_cast<IUFValue*>(GetMemberAddr(i));
                            pIUFValue += arrayIndex;

                            if ((pIUFValue->props.isDouble == false) && (pIUFValue->props.isFloat == false))
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned j = 0; j < pIUFValue->props.length; ++j)
                                {
                                    if (pIUFValue->props.isHex == true)
                                    {
                                        printf(" 0x%" PRIx64, pIUFValue->i64Vec2[j]);
                                    }
                                    else
                                    {
                                        printf(" %" PRId64, pIUFValue->i64Vec2[j]);
                                    }
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeFVec4:
                        {
                            IUFValue* pIUFValue = static_cast<IUFValue*>(GetMemberAddr(i));
                            pIUFValue += arrayIndex;

                            if ((pIUFValue->props.isDouble == false) && (pIUFValue->props.isFloat == true))
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned j = 0; j < pIUFValue->props.length; ++j)
                                {
                                    printf(" %.3f", pIUFValue->fVec4[j]);
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeF16Vec4:
                        {
                            IUFValue* pIUFValue = static_cast<IUFValue*>(GetMemberAddr(i));
                            pIUFValue += arrayIndex;

                            if ((pIUFValue->props.isDouble == false) && (pIUFValue->props.isFloat16 == true))
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned j = 0; j < pIUFValue->props.length; ++j)
                                {
                                    printf(" %.3fhf", pIUFValue->f16Vec4[j].GetValue());
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeDVec2:
                        {
                            IUFValue* pIUFValue = static_cast<IUFValue*>(GetMemberAddr(i));
                            pIUFValue += arrayIndex;

                            if ((pIUFValue->props.isDouble == true) && (pIUFValue->props.isFloat == false))
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned j = 0; j < pIUFValue->props.length; ++j)
                                {
                                    printf(" %.3f", pIUFValue->dVec2[j]);
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeIArray:
                        case MemberTypeUArray:
                        {
                            std::vector<unsigned>* pIntBufData =
                                *static_cast<std::vector<unsigned>**>(GetMemberAddr(i));

                            if (pIntBufData->size() > 0)
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned i = 0; i < pIntBufData->size(); ++i)
                                {
                                    printf(" 0x%x", (*pIntBufData)[i]);
                                }
                                printf("\n");
                            }

                            break;
                        }
                        case MemberTypeI64Array:
                        case MemberTypeU64Array:
                        {
                            std::vector<unsigned>* pInt64BufData =
                                *static_cast<std::vector<unsigned>**>(GetMemberAddr(i));
                            union
                            {
                                uint64_t u64Val;
                                unsigned uVal[2];
                            };

                            if (pInt64BufData->size() > 0)
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned i = 0; i < pInt64BufData->size(); i += 2)
                                {
                                    uVal[0] = (*pInt64BufData)[i];
                                    uVal[1] = (*pInt64BufData)[i + 1];
                                    printf(" 0x%" PRIx64, u64Val);
                                }
                                printf("\n");
                            }

                            break;
                        }
                        case MemberTypeFArray:
                        {
                            std::vector<unsigned>* pFloatBufData =
                                *static_cast<std::vector<unsigned>**>(GetMemberAddr(i));
                            union
                            {
                                float    fVal;
                                unsigned uVal;
                            };
                            if (pFloatBufData->size() > 0)
                            {
                                printf("%s =", m_pMemberTable[i].memberName);

                                for (unsigned i = 0; i < pFloatBufData->size(); ++i)
                                {
                                    uVal = (*pFloatBufData)[i];
                                    printf(" %.3f", fVal);
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeF16Array:
                        {
                            std::vector<uint16_t>* pFloat16BufData =
                                *static_cast<std::vector<uint16_t>**>(GetMemberAddr(i));
                            union
                            {
                                Float16  f16Val;
                                uint16_t uVal;
                            };
                            if (pFloat16BufData->size() > 0)
                            {
                                printf("%s =", m_pMemberTable[i].memberName);

                                for (unsigned i = 0; i < pFloat16BufData->size(); ++i)
                                {
                                    uVal = (*pFloat16BufData)[i];
                                    printf(" %.3f", f16Val.GetValue());
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeDArray:
                        {
                            std::vector<unsigned>* pDoubleBufData =
                                *static_cast<std::vector<unsigned>**>(GetMemberAddr(i));
                            union
                            {
                                double   dVal;
                                unsigned uVal[2];
                            };

                            if (pDoubleBufData->size() > 1)
                            {
                                printf("%s =", m_pMemberTable[i].memberName);
                                for (unsigned i = 0; i < pDoubleBufData->size() - 1; i+=2)
                                {
                                    uVal[0] = (*pDoubleBufData)[i];
                                    uVal[1] = (*pDoubleBufData)[i + 1];
                                    printf(" %.3f", dVal);
                                }
                                printf("\n");
                            }
                            break;
                        }
                        case MemberTypeString:
                        {
                            std::string* pStr = static_cast<std::string*>(GetMemberAddr(i));
                            printf("%s = %s\n", m_pMemberTable[i].memberName, pStr->c_str());
                            break;
                        }
                        default:
                            ;
                        }
                    }
                }
            }
        }
        putchar('\n');
    }
}

// =====================================================================================================================
// Creates a section object according to section name
Section* Section::CreateSection(
    const char* pSectionName)    // [in] Section name
{
    auto it = m_sectionInfo.find(pSectionName);

    VFX_ASSERT(it->second.type != SectionTypeUnset);

    Section* pSection = nullptr;
    switch (it->second.type)
    {
    case SectionTypeResult:
        pSection = new SectionResult();
        break;
    case SectionTypeBufferView:
        pSection = new SectionBufferView();
        break;
    case SectionTypeVertexState:
        pSection = new SectionVertexState();
        break;
    case SectionTypeDrawState:
        pSection = new SectionDrawState();
        break;
    case SectionTypeImageView:
        pSection = new SectionImageView();
        break;
    case SectionTypeSampler:
        pSection = new SectionSampler();
        break;
    case SectionTypeVersion:
        pSection = new SectionVersion();
        break;
    case SectionTypeCompileLog:
        pSection = new SectionCompileLog();
        break;
    case SectionTypeGraphicsState:
        pSection = new SectionGraphicsState();
        break;
    case SectionTypeComputeState:
        pSection = new SectionComputeState();
        break;
    case SectionTypeVertexInputState:
        pSection = new SectionVertexInput();
        break;
    case SectionTypeVertexShaderInfo:
    case SectionTypeTessControlShaderInfo:
    case SectionTypeTessEvalShaderInfo:
    case SectionTypeGeometryShaderInfo:
    case SectionTypeFragmentShaderInfo:
    case SectionTypeComputeShaderInfo:
        pSection = new SectionShaderInfo(it->second.type);
        break;
    case SectionTypeVertexShader:
    case SectionTypeTessControlShader:
    case SectionTypeTessEvalShader:
    case SectionTypeGeometryShader:
    case SectionTypeFragmentShader:
    case SectionTypeComputeShader:
        pSection = new SectionShader(it->second);
        break;
    default:
        VFX_NEVER_CALLED();
        break;
    }

    return pSection;
}

// =====================================================================================================================
// Gets section type according to section name
SectionType Section::GetSectionType(
    const char* pSectionName)   // [in] Section name
{
    SectionType type = SectionTypeUnset;
    auto it = m_sectionInfo.find(pSectionName);
    if (it != m_sectionInfo.end())
    {
        type = it->second.type;
    }
    return type;
}

// =====================================================================================================================
// Gets the pointer of sub section according to member name
bool Section::GetPtrOfSubSection(
    unsigned     lineNum,         // Line No.
    const char*  memberName,     // [in] Member name
    MemberType   memberType,      // Member type
    bool         isWriteAccess,   // Whether the sub section will be written
    unsigned     arrayIndex,      // Array index
    Section**    ptrOut,          // [out] Pointer of sub section
    std::string* errorMsg)       // [out] Error message
{
    bool result = false;

    switch (memberType)
    {
    CASE_SUBSECTION(MemberTypeResultItem, SectionResultItem)
    CASE_SUBSECTION(MemberTypeVertexBufferBindingItem, SectionVertexBufferBinding)
    CASE_SUBSECTION(MemberTypeVertexAttributeItem, SectionVertexAttribute)
    CASE_SUBSECTION(MemberTypeSpecConstItem, SectionSpecConstItem)
    CASE_SUBSECTION(MemberTypeSpecConst, SectionSpecConst)
    CASE_SUBSECTION(MemberTypePushConstRange, SectionPushConstRange)
    CASE_SUBSECTION(MemberTypeVertexInputBindingItem, SectionVertexInputBinding)
    CASE_SUBSECTION(MemberTypeVertexInputAttributeItem, SectionVertexInputAttribute)
    CASE_SUBSECTION(MemberTypeVertexInputDivisorItem, SectionVertexInputDivisor)
    CASE_SUBSECTION(MemberTypeColorBufferItem, SectionColorBuffer)
    CASE_SUBSECTION(MemberTypeSpecEntryItem, SectionSpecEntryItem)
    CASE_SUBSECTION(MemberTypeResourceMappingNode, SectionResourceMappingNode)
    CASE_SUBSECTION(MemberTypeSpecInfo, SectionSpecInfo)
    CASE_SUBSECTION(MemberTypeDescriptorRangeValue, SectionDescriptorRangeValueItem)
    CASE_SUBSECTION(MemberTypePipelineOption, SectionPipelineOption)
    CASE_SUBSECTION(MemberTypeShaderOption, SectionShaderOption)
    CASE_SUBSECTION(MemberTypeNggState, SectionNggState)
        break;
    default:
        VFX_NEVER_CALLED();
        break;
    }

    return result;
}

// =====================================================================================================================
// Reads whole file content
// NOTE: The input file name is  from class member "fileName" and read result is stored in shaderSource or m_spvBin
// according to file type
bool Section::ReadFile(
    const std::string&    docFilename,      // [in] File name of parent document
    const std::string&    fileName,         // [in] File name
    bool                  isBinary,         // Whether file is SPIRV binary file
    std::vector<uint8_t>* pBinaryData,      // [out] Binary data
    std::string*          pTextData,        // [out] Text data
    std::string*          errorMsg)        // [out] Error message
{
    bool result = true;

    // Prepend directory from "docFilename" to the given filename.
    std::string path;
    auto separatorIndex = docFilename.find_last_of("/\\");
    if (separatorIndex != std::string::npos)
    {
        path = docFilename.substr(0, separatorIndex + 1);
    }
    path += fileName;

    // Open file
    FILE* pInFile = fopen(path.c_str(), isBinary ? "rb" : "r");
    if (pInFile == nullptr)
    {
        PARSE_ERROR(*errorMsg, 0, "Fails to open input file: %s\n", path.c_str());
        return false;
    }

    // Get file size
    fseek(pInFile, 0, SEEK_END);
    size_t fileSize = ftell(pInFile);
    fseek(pInFile, 0, SEEK_SET);

    // Allocate temp buffer and read file
    char* pData = new char[fileSize + 1];
    VFX_ASSERT(pData != nullptr);
    memset(pData, 0, fileSize + 1);
    size_t readSize = fread(pData, 1, fileSize, pInFile);

    // Copy to destination
    if (isBinary)
    {
        (*pBinaryData).resize(readSize);
        memcpy(&(*pBinaryData)[0], pData, readSize);
    }
    else
    {
        *pTextData = pData;
    }

    // Clean up
    delete[] pData;
    fclose(pInFile);

    return result;
}

// =====================================================================================================================
// Compiles GLSL source text file (input) to SPIR-V binary file (output).
bool SectionShader::CompileGlsl(
    const Section* pShaderInfo, // [in] Shader info section
    std::string*   errorMsg)   // [out] Error message
{
    int           sourceStringCount = 1;
    const char*const* sourceList[1]     = {};
    const char*const* fileList[1]       = {};

    bool        result    = true;
    const char* pGlslText = shaderSource.c_str();
    const char* pFileName = fileName.c_str();
    SpvGenStage stage     = static_cast<SpvGenStage>(m_sectionType - SectionTypeVertexShader);
    void*       pProgram  = nullptr;
    const char* pLog      = nullptr;

    if (InitSpvGen() == false)
    {
        PARSE_ERROR(*errorMsg, m_lineNum, "Failed to load SPVGEN: cannot compile GLSL\n");
        return false;
    }

    sourceList[0] = &pGlslText;
    fileList[0] = &pFileName;
    int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules | SpvGenOptionDebug;
    if ((m_shaderType == Hlsl) || (m_shaderType == HlslFile))
    {
        compileOption |= SpvGenOptionReadHlsl;
    }
    const char* pEntryPoint = nullptr;
    if (pShaderInfo != nullptr)
    {
        pEntryPoint = reinterpret_cast<const SectionShaderInfo*>(pShaderInfo)->GetEntryPoint();
    }
    bool compileResult = spvCompileAndLinkProgramEx(1,
                                                   &stage,
                                                   &sourceStringCount,
                                                   sourceList,
                                                   fileList,
                                                   &pEntryPoint,
                                                   &pProgram,
                                                   &pLog,
                                                   compileOption);

    if (compileResult)
    {
        const unsigned* pSpvBin = nullptr;
        unsigned binSize = spvGetSpirvBinaryFromProgram(pProgram, 0, &pSpvBin);
        m_spvBin.resize(binSize);
        memcpy(&m_spvBin[0], pSpvBin, binSize);
    }
    else
    {
        PARSE_ERROR(*errorMsg, m_lineNum, "Fail to compile GLSL\n%s\n", pLog);
        result = false;
    }

    if (pProgram)
    {
        spvDestroyProgram(pProgram);
    }

    return result;
}

// =====================================================================================================================
// Assemble Spirv assemble code
bool SectionShader::AssembleSpirv(
    std::string* errorMsg)  // [out] Error message
{
    bool result = true;
    const char* pText = shaderSource.c_str();

    if (InitSpvGen() == false)
    {
        PARSE_ERROR(*errorMsg, m_lineNum, "Failed to load SPVGEN: cannot assemble SPIR-V assembler source\n");
        return false;
    }

    const char* pLog = nullptr;
    unsigned bufSize = static_cast<unsigned>(shaderSource.size()) * 4 + 1024;
    unsigned* pBuffer = new unsigned[bufSize / 4];

    int binSize = spvAssembleSpirv(pText, bufSize, pBuffer, &pLog);

    if (binSize > 0)
    {
        m_spvBin.resize(binSize);
        memcpy(&m_spvBin[0], pBuffer, binSize);
    }
    else
    {
        PARSE_ERROR(*errorMsg, m_lineNum, "Fail to Assemble SPIRV\n%s\n", pLog);
        result = false;
    }

    delete[] pBuffer;
    return result;
}

// =====================================================================================================================
// Returns true if this section contains shader source
bool SectionShader::IsShaderSourceSection()
{
    bool ret = false;
    switch (m_shaderType)
    {
    case Glsl:
    case SpirvAsm:
        ret = true;
        break;
    default:
        ret = false;
        break;
    }
    return ret;
}

// =====================================================================================================================
// Loads external shader code, and translate shader code to SPIRV binary
bool SectionShader::CompileShader(
    const std::string&  docFilename,      // [in] File name of parent document
    const Section*      pShaderInfo,      // [in] Shader info sections
    std::string*        errorMsg)        // [out] Error message
{
    bool result = false;
    switch (m_shaderType)
    {
    case Glsl:
    case Hlsl:
        {
            result = CompileGlsl(pShaderInfo, errorMsg);
            break;
        }
    case GlslFile:
    case HlslFile:
        {
            result = ReadFile(docFilename, fileName, false, &m_spvBin, &shaderSource, errorMsg);
            if (result)
            {
                CompileGlsl(pShaderInfo, errorMsg);
            }
            break;
        }
    case SpirvAsm:
        {
            result = AssembleSpirv(errorMsg);
            break;
        }
    case SpirvAsmFile:
        {
            result = ReadFile(docFilename, fileName, false, &m_spvBin, &shaderSource, errorMsg);
            if (result)
            {
                AssembleSpirv(errorMsg);
            }
            break;
        }
    case SpirvFile:
        {
            result = ReadFile(docFilename, fileName, true, &m_spvBin, &shaderSource, errorMsg);
            break;
        }
    default:
        {
            VFX_NEVER_CALLED();
            break;
        }
    }
    return result;
}

void SectionShader::GetSubState(SectionShader::SubState& state)
{
    state.dataSize = static_cast<unsigned>(m_spvBin.size());
    state.pData = state.dataSize > 0 ? &m_spvBin[0] : nullptr;

    switch (m_sectionType)
    {
    case SectionTypeVertexShader:
        state.stage = Vkgc::ShaderStageVertex;
        break;
    case SectionTypeTessControlShader:
        state.stage = Vkgc::ShaderStageTessControl;
        break;
    case SectionTypeTessEvalShader:
        state.stage = Vkgc::ShaderStageTessEval;
        break;
    case SectionTypeGeometryShader:
        state.stage = Vkgc::ShaderStageGeometry;
        break;
    case SectionTypeFragmentShader:
        state.stage = Vkgc::ShaderStageFragment;
        break;
    case SectionTypeComputeShader:
        state.stage = Vkgc::ShaderStageCompute;
        break;
    default:
        VFX_NEVER_CALLED();
        state.stage = Vkgc::ShaderStageInvalid;
        break;
    }
}

}

