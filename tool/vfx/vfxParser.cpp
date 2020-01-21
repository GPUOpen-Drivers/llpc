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
* @file  VfxParser.cpp
* @brief Contains implementation of class VfxParser
***********************************************************************************************************************
*/

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "vfxParser.h"
#include "vfxEnumsConverter.h"
#include "vfxError.h"
#include "vfxRenderDoc.h"
#include "vfxPipelineDoc.h"

namespace Vfx
{
// Parser functions to parse a value by it's type
bool ParseInt(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseFloat(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseFloat16(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseDouble(char* pStr, uint32_t lineNum, IUFValue* pOutput);

bool ParseBool(char* pStr, uint32_t lineNum, IUFValue* pOutput, std::string* pErrorMsg);

bool ParseIVec4(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseI64Vec2(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseFVec4(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseF16Vec4(char* pStr, uint32_t lineNum, IUFValue* pOutput);
bool ParseDVec2(char* pStr, uint32_t lineNum, IUFValue* pOutput);

bool ParseIArray(char* pStr, uint32_t lineNum, bool isSign, std::vector<uint8_t>& bufMem);
bool ParseI64Array(char* pStr, uint32_t lineNum, bool isSign, std::vector<uint8_t>& bufMem);
bool ParseFArray(char* pStr, uint32_t lineNum, std::vector<uint8_t>& bufMem);
bool ParseF16Array(char* pStr, uint32_t lineNum, std::vector<uint8_t>& bufMem);
bool ParseDArray(char* pStr, uint32_t lineNum, std::vector<uint8_t>& bufMem);

bool ParseBinding(char* pStr, uint32_t lineNum, IUFValue* pOutput);

bool ParseEnumName(char* pEnumName, uint32_t lineNum, IUFValue* pOutput, std::string* pErrorMsg);

// Trims space at the beginning of a string.
char* TrimStringBeginning(char * pStr);

// Trims space at the end of a string.
char* TrimStringEnd(char* pStr);

// Parses a key-value pair.
bool ExtractKeyAndValue(char* pLine, uint32_t lineNum, const char delimiter, char** ppKey, char** ppValue, std::string* pErrorMsg);

// Parses an array index access in a pair of brackets.
bool ParseArrayAccess(char* pStr, uint32_t lineNum, uint32_t* pArrayIndex, char** ppLBracket, char** ppRBracket, std::string* pErrorMsg);

// Checks if a string contains array index access, which is a digits string inside a pair of brackets.
bool IsArrayAccess(const char* pStr);

// Gets one word for a string and return the start position of next word, nullptr is returned if word isn't found
// in the string
char* GetWordFromString(char* pStr, char* pWordBuffer);

// =====================================================================================================================
Document::~Document()
{
    for (uint32_t i = 0; i < SectionTypeNameNum; ++i)
    {
        for (uint32_t j = 0; j < m_sections[i].size(); ++j)
        {
            delete m_sections[i][j];
        }
        m_sections[i].clear();
    }
}

// =====================================================================================================================
// Creates Vfx document object with specified document type
Document* Document::CreateDocument(
    VfxDocType type)          // Document type
{
    if (type == VfxDocTypeRender)
    {
        return new RenderDocument;
    }
    else
    {
        return new PipelineDocument;
    }
}

// =====================================================================================================================
// Gets a free section for specified section type
Section* Document::GetFreeSection(
    const char* pSectionName)      // [in] Section name
{
    Section*    pSection        = nullptr;
    SectionType type            = Section::GetSectionType(pSectionName);
    const uint32_t maxSectionCount = GetMaxSectionCount(type);
    if (m_sections[type].size() < maxSectionCount)
    {
        pSection = Section::CreateSection(pSectionName);
        m_sections[type].push_back(pSection);
        m_sectionList.push_back(pSection);
    }
    return pSection;
}

// =====================================================================================================================
// Prints all parsed rule based key-values, for debug purpose.
void Document::PrintSelf()
{
    for (uint32_t i = 0; i < SectionTypeNameNum; ++i)
    {
        for (uint32_t j = 0; j < m_sections[i].size(); ++j)
        {
            m_sections[i][j]->PrintSelf(0);
        }
    }
}

// =====================================================================================================================
// Compiles input shader source to SPIRV binary
bool Document::CompileShader()
{
    bool ret = true;
    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
    {
        for (size_t i = 0; i < m_sections[SectionTypeVertexShader + stage].size(); ++i)
        {
            auto pShaderSection = m_sections[SectionTypeVertexShader + stage][i];
            VFX_ASSERT(m_sections[SectionTypeVertexShaderInfo + stage].size() > i);
            auto pShaderInfoSection = m_sections[SectionTypeVertexShaderInfo + stage][i];
            bool stageRet = reinterpret_cast<SectionShader*>(pShaderSection)->CompileShader(m_fileName, pShaderInfoSection, &m_errorMsg);
            ret = ret && stageRet;
        }
    }
    return ret;
}

// =====================================================================================================================
// Constructs an instance of class VfxParse.
VfxParser::VfxParser()
    :
    m_isValidVfxFile(false),
    m_pCurrentSection(nullptr),
    m_currentLineNum(0),
    m_currentSectionLineNum(0)
{

}

// =====================================================================================================================
// Parses a config file line.
bool VfxParser::ParseLine(
    char* pLine)    // [in] Input test config line.
{
    bool result = true;
    ++m_currentLineNum;

    // Trim comments for blocks other than shader source blocks, shader source strings are passed to compiler as-is.
    if (m_pCurrentSection == nullptr || m_pCurrentSection->IsShaderSourceSection() == false)
    {
        char* pComments = strchr(pLine, ';');
        if (pComments != nullptr)
        {
            *pComments = '\0';
        }
    }

    if (*pLine == '[')
    {
        result = EndSection();
        if (result == true)
        {
            result = BeginSection(pLine);
        }
    }
    else
    {
        m_currentSectionStringBuffer << pLine;
    }

    return result;
}

// =====================================================================================================================
// Begins a section.
bool VfxParser::BeginSection(
    char* pLine)    // [in] Input test config line.
{
    bool result = true;
    VFX_ASSERT(*pLine == '[');
    char* pBracketBack = strchr(pLine, ']');
    if (pBracketBack != nullptr)
    {
        *pBracketBack = '\0';
    }
    else
    {
        PARSE_ERROR(*m_pErrorMsg, m_currentLineNum, "expect ]");
        result = false;
    }

    if (result)
    {
        pLine = pLine + 1;
        char* pSectionName = strtok(pLine, ",");
        m_pCurrentSection = m_pVfxDoc->GetFreeSection(pSectionName);
        if (m_pCurrentSection != nullptr)
        {
            // Next line is the first line of section content.
            m_currentSectionLineNum       = m_currentLineNum + 1;
            m_currentSectionStringBuffer.str("");
            m_currentSectionStringBuffer.clear();
            m_pCurrentSection->SetLineNum(m_currentLineNum);
        }
    }

    return result;
}

// =====================================================================================================================
// Ends a section.
bool VfxParser::EndSection()
{
    bool result = true;

    if (m_pCurrentSection == nullptr)
    {
        // Do nothing
    }
    else if (m_pCurrentSection->IsShaderSourceSection() ||
             (m_pCurrentSection->GetSectionType() == SectionTypeCompileLog))
    {
        // Process shader source sections.
        ParseSectionShaderSource();
    }
    else
    {
        // Process key-value based sections.
        result = ParseSectionKeyValues();
        if (result)
        {
            if (m_pCurrentSection->GetSectionType() == SectionTypeVersion)
            {
                uint32_t version;
                reinterpret_cast<SectionVersion*>(m_pCurrentSection)->GetSubState(version);
                result = m_pVfxDoc->CheckVersion(version);
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Parses a line of a pre-defined key-value section.
bool VfxParser::ParseSectionKeyValues()
{
    bool result = true;

    // Set line number variable which is used in error report.
    uint32_t lineNum = m_currentSectionLineNum;
    char pLineBuffer[MaxLineBufSize];
    while (true)
    {
        m_currentSectionStringBuffer.getline(pLineBuffer, MaxLineBufSize);

        size_t readCount = static_cast<size_t>(m_currentSectionStringBuffer.gcount());
        VFX_ASSERT(readCount < MaxLineBufSize);
        if (readCount == 0)
        {
            break;
        }
        if ((pLineBuffer[0] == '\0') || (memcmp(pLineBuffer, "\r", 2) == 0))
        {
            // Skip empty line
            continue;
        }

        char* pKey   = nullptr;
        char* pValue = nullptr;

        result = ExtractKeyAndValue(pLineBuffer, lineNum, '=', &pKey, &pValue, m_pErrorMsg);

        if (result == false)
        {
            break;
        }

        ParseKeyValue(pKey,
                      pValue,
                      lineNum,
                      m_pCurrentSection);

        ++lineNum;
    }

    return result;
}

// =====================================================================================================================
// Parses a key string to process array access("[]") and member access(".").
bool VfxParser::ParseKey(
    const char*           pKey,                 // [in]  Input key string
    uint32_t              lineNum,              // Line number
    Section*              pSectionObjectIn,     // [in]  Base section object
    Section**             ppSectionObjectOut,   // [out] Target section object after apply array access and member
                                                //       access in key string.
    char*                 pMemberNameBuffer,    // [out] Name of the member to be accessed in target section object.
    uint32_t              memberNameBufferSize, // Size of member name buffer.
    uint32_t*             pArrayIndex)          // [out] Array index applied this member (0 for non array)

{
    bool result = true;
    // Get a copy of key string.
    char keyBuffer[MaxKeyBufSize];
    VFX_ASSERT(strlen(pKey) < MaxKeyBufSize);
    strcpy(keyBuffer, pKey);

    VFX_ASSERT(pSectionObjectIn != nullptr);
    Section* pTempSectionObj = pSectionObjectIn;

    // Process member access
    char* pKeyTok = strtok(keyBuffer, ".");
    pKeyTok = TrimStringBeginning(pKeyTok);
    pKeyTok = TrimStringEnd(pKeyTok);

    bool isSection = false;          // Is this member an Section object
    bool isArrayAccess = false;     // Is containing array access
    uint32_t arrayIndex = 0;        // Array access index
    MemberType memberType;

    while (pKeyTok != nullptr)
    {
        isArrayAccess = IsArrayAccess(pKeyTok);

        if (isArrayAccess)
        {
            char* pLBracket = nullptr;
            result = ParseArrayAccess(pKeyTok, lineNum, &arrayIndex, &pLBracket, nullptr, m_pErrorMsg);
            // Remove bracket from string token
            *pLBracket = '\0';
            pKeyTok = TrimStringEnd(pKeyTok);
        }
        else
        {
            arrayIndex = 0;
        }

        result = pTempSectionObj->IsSection(lineNum, pKeyTok, &isSection, &memberType, m_pErrorMsg);
        if (result == false)
        {
            break;
        }

        if (isSection == false)
        {
            VFX_ASSERT(strlen(pKeyTok) < memberNameBufferSize);
            strncpy(pMemberNameBuffer, pKeyTok, memberNameBufferSize);
        }
        else
        {
            result = pTempSectionObj->GetPtrOfSubSection(lineNum,
                                                         pKeyTok,
                                                         memberType,
                                                         true,
                                                         arrayIndex,
                                                         &pTempSectionObj,
                                                         m_pErrorMsg);
            if (result == false)
            {
                break;
            }
        }

        pKeyTok = strtok(nullptr, ".");
    }

    if (pArrayIndex != nullptr)
    {
        *pArrayIndex = arrayIndex;
    }

    if (ppSectionObjectOut != nullptr)
    {
        *ppSectionObjectOut = pTempSectionObj;
    }

    return result;
}

// =====================================================================================================================
// Parses a key-value pair according to predefined rule.
bool VfxParser::ParseKeyValue(
    char*                         pKey,           // [in] Input key string
    char*                         pValue,         // [in] Input value string
    uint32_t                      lineNum,        // Line number
    Section*                      pSectionObject) // [out] Key-value map to hold the parse results.
{
    bool result = false;

    Section* pAccessedSectionObject = nullptr;
    uint32_t arrayIndex = 0;
    char memberName[MaxKeyBufSize];
    result = ParseKey(pKey,
                      lineNum,
                      pSectionObject,
                      &pAccessedSectionObject,
                      memberName,
                      MaxKeyBufSize,
                      &arrayIndex);

    if (result == true)
    {
        MemberType valueType;
        result = pAccessedSectionObject->GetMemberType(lineNum, memberName, &valueType, m_pErrorMsg);

        if (result == true)
        {
            IUFValue value = {};

            // Parse value according to it's type
            switch (valueType)
            {
            case MemberTypeEnum:
                {
                    result = ParseEnumName(pValue, lineNum, &value, m_pErrorMsg);
                    if (result == true)
                    {
                        result = pAccessedSectionObject->Set(lineNum, memberName, &(value.iVec4[0]));
                    }
                    break;
                }
            case MemberTypeInt:
                {
                    result = ParseInt(pValue, lineNum, &value);
                    if (result == true)
                    {
                        result = pAccessedSectionObject->Set(lineNum, memberName, &(value.iVec4[0]));
                    }
                    break;
                }
            case MemberTypeFloat:
                {
                    result = ParseFloat16(pValue, lineNum, &value);
                    if (result == true)
                    {
                        result = pAccessedSectionObject->Set(lineNum, memberName, &(value.f16Vec4[0]));
                    }
                    break;
                }
            case MemberTypeDouble:
                {
                    result = ParseDouble(pValue, lineNum, &value);
                    if (result == true)
                    {
                        result = pAccessedSectionObject->Set(lineNum, memberName, &(value.dVec2[0]));
                    }
                    break;
                }
            case MemberTypeBool:
                {
                    result = ParseBool(pValue, lineNum, &value, m_pErrorMsg);
                    if (result == true)
                    {
                        static_assert(sizeof(uint8_t) == sizeof(bool), "");
                        uint8_t bValue = value.iVec4[0] ? 1 : 0;
                        result = pAccessedSectionObject->Set(lineNum, memberName, &bValue);
                    }
                    break;
                }
            case MemberTypeIVec4:
                {
                    result = ParseIVec4(pValue, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = pAccessedSectionObject->Set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeI64Vec2:
                {
                    result = ParseI64Vec2(pValue, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = pAccessedSectionObject->Set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeBinding:
                {
                    result = ParseBinding(pValue, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = pAccessedSectionObject->Set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeFVec4:
                {
                    result = ParseFVec4(pValue, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = pAccessedSectionObject->Set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeF16Vec4:
                {
                    result = ParseF16Vec4(pValue, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = pAccessedSectionObject->Set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeDVec2:
                {
                    result = ParseDVec2(pValue, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = pAccessedSectionObject->Set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeIArray:
            case MemberTypeUArray:
                {
                    std::vector<uint8_t>** ppIntData = nullptr;
                    pAccessedSectionObject->GetPtrOf(lineNum, memberName, true, 0, &ppIntData, m_pErrorMsg);
                    result = ParseIArray(pValue, lineNum, valueType == MemberTypeIArray, **ppIntData);
                    break;
                }
            case MemberTypeI64Array:
            case MemberTypeU64Array:
                {
                    std::vector<uint8_t>** ppIntData = nullptr;
                    pAccessedSectionObject->GetPtrOf(lineNum, memberName, true, 0, &ppIntData, m_pErrorMsg);
                    result = ParseI64Array(pValue, lineNum, valueType == MemberTypeI64Array, **ppIntData);
                    break;
                }
            case MemberTypeFArray:
                {
                    std::vector<uint8_t>** ppFloatData = nullptr;
                    pAccessedSectionObject->GetPtrOf(lineNum, memberName, true, 0, &ppFloatData, m_pErrorMsg);
                    result = ParseFArray(pValue, lineNum, **ppFloatData);
                    break;
                }
            case MemberTypeF16Array:
                {
                    std::vector<uint8_t>** ppFloatData = nullptr;
                    pAccessedSectionObject->GetPtrOf(lineNum, memberName, true, 0, &ppFloatData, m_pErrorMsg);
                    result = ParseF16Array(pValue, lineNum, **ppFloatData);
                    break;
                }
            case MemberTypeDArray:
                {
                    std::vector<uint8_t>** ppDoubleData;
                    pAccessedSectionObject->GetPtrOf(lineNum, memberName, true, 0, &ppDoubleData, m_pErrorMsg);
                    result = ParseDArray(pValue, lineNum, **ppDoubleData);
                    break;
                }
            case MemberTypeString:
                {
                    std::string str = pValue;
                    result = pAccessedSectionObject->Set(lineNum, memberName, &str);
                    break;
                }
            default:
                {
                    VFX_NEVER_CALLED();
                }
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Parses shader source section.
void VfxParser::ParseSectionShaderSource()
{
    char pLineBuffer[MaxLineBufSize];

    while (true)
    {
        m_currentSectionStringBuffer.getline(pLineBuffer, MaxLineBufSize);

        size_t readCount = static_cast<size_t>(m_currentSectionStringBuffer.gcount());
        VFX_ASSERT(readCount < MaxLineBufSize);
        if (readCount == 0)
        {
            break;
        }

        // Line ending is not returned by getline(), so append them manually.
        pLineBuffer[readCount- 1] = '\n';
        pLineBuffer[readCount]    = '\0';
        m_pCurrentSection->AddLine(pLineBuffer);
    }
}

// =====================================================================================================================
// Parses a VFX config file.
bool VfxParser::Parse(
    const TestCaseInfo& info,  // [in] Name of VFX file to parse.
    Document*           pDoc)  // [out] Parse result
{
    bool result = true;
    m_pVfxDoc   = pDoc;
    m_pErrorMsg = pDoc->GetErrorMsg();

    FILE* pConfigFile = fopen(info.vfxFile.c_str(), "r");
    if (pConfigFile != nullptr)
    {
        pDoc->SetFileName(info.vfxFile);
        char lineBuf[MaxLineBufSize];
        char* pLinePtr = nullptr;

        while (true)
        {
            pLinePtr = fgets(lineBuf, MaxLineBufSize, pConfigFile);

            if (pLinePtr == nullptr)
            {
                result = EndSection();
                break;
            }
            else
            {
                result = MacroSubstituteLine(pLinePtr, m_currentLineNum+1, &info.macros, MaxLineBufSize);
                if (result == false)
                {
                    break;
                }

                result = ParseLine(pLinePtr);
                if (result == false)
                {
                    break;
                }
            }
        }

        fclose(pConfigFile);

        if (result)
        {
            result = m_pVfxDoc->Validate();
        }

        if (result)
        {
            result = m_pVfxDoc->CompileShader();
        }
    }
    else
    {
        result = false;
    }

    m_isValidVfxFile = result;

    return result;
}

// =====================================================================================================================
// Parses an int number from a string.
bool ParseInt(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = true;

    bool isHex = false;
    char* p0x = strstr(pStr, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    if (isHex)
    {
        pOutput->uVec4[0] = strtoul(pStr, nullptr, 0);
    }
    else
    {
        pOutput->iVec4[0] = strtol(pStr, nullptr, 0);
    }

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = false;
    pOutput->props.isHex = isHex;
    pOutput->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parses a float number from a string.
bool ParseFloat(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = true;

    pOutput->fVec4[0] = static_cast<float>(strtod(pStr, nullptr));

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = true;
    pOutput->props.isDouble = false;
    pOutput->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parses a float16 number from a string.
bool ParseFloat16(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = true;

    float v = static_cast<float>(strtod(pStr, nullptr));
    Float16 v16;
    v16.FromFloat32(v);
    pOutput->f16Vec4[0] = v16;

    pOutput->props.isInt64      = false;
    pOutput->props.isFloat      = false;
    pOutput->props.isFloat16    = true;
    pOutput->props.isDouble     = false;
    pOutput->props.length       = 1;

    return result;
}

// =====================================================================================================================
// Parses a double number from a string.
bool ParseDouble(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = true;

    pOutput->dVec2[0] = strtod(pStr, nullptr);

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = true;
    pOutput->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parse a boolean value from a string.
bool ParseBool(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput,    // [out] Stores parsed value
    std::string* pErrorMsg)
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = true;

    if (strcmp(pStr, "true") == 0)
    {
        pOutput->iVec4[0] = 1;
    }
    else if (strcmp(pStr, "false") == 0)
    {
        pOutput->iVec4[0] = 0;
    }
    else
    {
        pOutput->iVec4[0] = strtol(pStr, nullptr, 0);
    }

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = false;
    pOutput->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parses a integer vec4 from a string.
// NOTE: content of pStr will be changed.
bool ParseIVec4(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = false;

    bool isHex = false;
    char* p0x = strstr(pStr, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    char* pNumber = strtok(pStr, ", ");
    uint32_t numberId = 0;
    while (pNumber != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 4);
        if (isHex == true)
        {
            pOutput->uVec4[numberId] = strtoul(pNumber, nullptr, 0);
        }
        else
        {
            pOutput->iVec4[numberId] = strtol(pNumber, nullptr, 0);
        }
        pNumber = strtok(nullptr, ", ");
        ++numberId;
    }

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = false;
    pOutput->props.isHex = isHex;
    pOutput->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a int64 vec2 from a string.
// NOTE: content of pStr will be changed.
bool ParseI64Vec2(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = false;

    bool isHex = false;
    char* p0x = strstr(pStr, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    char* pNumber = strtok(pStr, ", ");
    uint32_t numberId = 0;
    while (pNumber != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 2);
        if (isHex == true)
        {
            pOutput->i64Vec2[numberId] = strtoull(pNumber, nullptr, 0);
        }
        else
        {
            pOutput->i64Vec2[numberId] = strtoll(pNumber, nullptr, 0);
        }
        pNumber = strtok(nullptr, ", ");
        ++numberId;
    }

    pOutput->props.isInt64 = true;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = false;
    pOutput->props.isHex = isHex;
    pOutput->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a float vec4 from a string.
// NOTE: content of pStr will be changed.
bool ParseFVec4(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = false;

    char* pNumber = strtok(pStr, ", ");
    uint32_t numberId = 0;
    while (pNumber != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 4);

        pOutput->fVec4[numberId] = static_cast<float>(strtod(pNumber, nullptr));

        pNumber = strtok(nullptr, ", ");
        ++numberId;
    }

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = true;
    pOutput->props.isDouble = false;
    pOutput->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a float16 vec4 from a string.
// NOTE: content of pStr will be changed.
bool ParseF16Vec4(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = false;

    char* pNumber = strtok(pStr, ", ");
    uint32_t numberId = 0;
    while (pNumber != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 4);

        float v = static_cast<float>(strtod(pNumber, nullptr));
        Float16 v16;
        v16.FromFloat32(v);
        pOutput->f16Vec4[numberId] = v16;

        pNumber = strtok(nullptr, ", ");
        ++numberId;
    }

    pOutput->props.isInt64      = false;
    pOutput->props.isFloat      = false;
    pOutput->props.isFloat16    = true;
    pOutput->props.isDouble     = false;
    pOutput->props.length       = numberId;

    return result;
}

// =====================================================================================================================
// Parses a double vec2 from a string.
// NOTE: content of pStr will be changed.
bool ParseDVec2(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = false;

    char* pNumber = strtok(pStr, ", ");
    uint32_t numberId = 0;
    while (pNumber != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 2);

        pOutput->dVec2[numberId] = strtod(pNumber, nullptr);

        pNumber = strtok(nullptr, ", ");
        ++numberId;
    }

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = true;
    pOutput->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated integer values
// NOTE: content of pStr will be changed.
bool ParseIArray(
    char*                  pStr,       // [in]  Input string
    uint32_t               lineNum,    // Current line number
    bool                   isSign,     // True if it is signed integer
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* pNumber = strtok(pStr, ", ");
    while (pNumber != nullptr)
    {
        bool isHex = false;
        char* p0x = strstr(pNumber, "0x");
        if (p0x != nullptr)
        {
            isHex = true;
        }

        union
        {
            int32_t  iVal;
            uint32_t uVal;
            uint8_t  bVal[4];
        };
        iVal = 0;

        if ((isHex == true) || (isSign == false))
        {
            uVal = strtoul(pNumber, nullptr, 0);
        }
        else
        {
            iVal = strtol(pNumber, nullptr, 0);
        }

        for (uint32_t i = 0; i < sizeof(bVal); ++i)
        {
            bufMem.push_back(bVal[i]);
        }

        pNumber = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated int64 values
// NOTE: content of pStr will be changed.
bool ParseI64Array(
    char*                  pStr,       // [in]  Input string
    uint32_t               lineNum,    // Current line number
    bool                   isSign,     // True if it is signed integer
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* pNumber = strtok(pStr, ", ");
    while (pNumber != nullptr)
    {
        bool isHex = false;
        char* p0x = strstr(pNumber, "0x");
        if (p0x != nullptr)
        {
            isHex = true;
        }

        union
        {
            int64_t  i64Val;
            uint64_t u64Val;
            uint32_t uVal[2];
            uint8_t  bVal[8];
        };
        i64Val = 0;

        if ((isHex == true) || (isSign == false))
        {
            u64Val = strtoull(pNumber, nullptr, 0);
        }
        else
        {
            i64Val = strtoll(pNumber, nullptr, 0);
        }

        for (uint32_t i = 0; i < sizeof(bVal); ++i)
        {
            bufMem.push_back(bVal[i]);
        }

        pNumber = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated float values
// NOTE: content of pStr will be changed.
bool ParseFArray(
    char*                  pStr,       // [in]  Input string
    uint32_t               lineNum,    // Current line number
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* pNumber = strtok(pStr, ", ");
    while (pNumber != nullptr)
    {
        union
        {
           float    fVal;
           uint32_t uVal;
           uint8_t  bVal[4];
        };

        fVal = static_cast<float>(strtod(pNumber, nullptr));

        for (uint32_t i = 0; i < sizeof(bVal); ++i)
        {
            bufMem.push_back(bVal[i]);
        }

        pNumber = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated float16 values
// NOTE: content of pStr will be changed.
bool ParseF16Array(
    char*                  pStr,       // [in]  Input string
    uint32_t               lineNum,    // Current line number
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* pNumber = strtok(pStr, ", ");
    while (pNumber != nullptr)
    {
        union
        {
           Float16Bits fVal;
           uint16_t    uVal;
           uint8_t     bVal[2];
        };

        float v = static_cast<float>(strtod(pNumber, nullptr));
        Float16 v16;
        v16.FromFloat32(v);
        fVal = v16.GetBits();

        for (uint32_t i = 0; i < sizeof(bVal); ++i)
        {
            bufMem.push_back(bVal[i]);
        }

        pNumber = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated double values
// NOTE: content of pStr will be changed.
bool ParseDArray(
    char*               pStr,          // [in]  Input string
    uint32_t            lineNum,       // Current line number
    std::vector<uint8_t>& bufMem)      // [in,out] Buffer data
{
    bool result = true;

    char* pNumber = strtok(pStr, ", ");
    while (pNumber != nullptr)
    {
        union
        {
           double   dVal;
           uint32_t uVal[2];
           uint8_t  bVal[8];
        };

        dVal = strtod(pNumber, nullptr);

        for (uint32_t i = 0; i < sizeof(bVal); ++i)
        {
            bufMem.push_back(bVal[i]);
        }

        pNumber = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses binding, it's a integer vec3 from a string.
// NOTE: content of pStr will be changed.
bool ParseBinding(
    char*       pStr,       // [in]  Input string
    uint32_t    lineNum,    // Current line number
    IUFValue*   pOutput)    // [out] Stores parsed value
{
    VFX_ASSERT(pOutput != nullptr);
    bool result = false;

    bool isHex = false;
    char* p0x = strstr(pStr, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    char* pNumber = strtok(pStr, ", ");
    uint32_t numberId = 0;
    while (pNumber != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 3);
        if (strcmp(pNumber, "vb") == 0)
        {
            pOutput->uVec4[numberId] = VfxVertexBufferSetId;
        }
        else if (strcmp(pNumber, "ib") == 0)
        {
            pOutput->uVec4[numberId] = VfxIndexBufferSetId;
        }
        else
        {
            if (isHex == true)
            {
                pOutput->uVec4[numberId] = strtoul(pNumber, nullptr, 0);
            }
            else
            {
                pOutput->iVec4[numberId] = strtol(pNumber, nullptr, 0);
            }
        }
        pNumber = strtok(nullptr, ", ");
        ++numberId;
    }

    pOutput->props.isInt64 = false;
    pOutput->props.isFloat = false;
    pOutput->props.isDouble = false;
    pOutput->props.isHex = isHex;
    pOutput->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a enum string
bool ParseEnumName(
    char*        pEnumName,  // Enum name
    uint32_t     lineNum,    // Line No.
    IUFValue*    pOutput,    // [Out] Enum value
    std::string* pErrorMsg)  // [Out] Error message
{
    bool result = false;
    int32_t value = VfxInvalidValue;
    result = GetEnumValue(pEnumName, value);

    if (result == false)
    {
        PARSE_ERROR(*pErrorMsg, lineNum, "unknow enum");
    }
    else
    {
        pOutput->iVec4[0] = value;
    }

    return result;
}

// =====================================================================================================================
// Trims space at the beginning of a string.
char * TrimStringBeginning(
    char * pStr)    // [in] Input string pointer.
{
    while (*pStr)
    {
        if (*pStr == ' ' || *pStr == '\t' || *pStr == '\n' || *pStr == '\r')
        {
            ++pStr;
        }
        else
        {
            break;
        }
    }
    return pStr;
}

// =====================================================================================================================
// Trims space at the end of a string.
// NOTE: The function will change the contents of an input string
char * TrimStringEnd(
    char * pStr)    // [in] Input string pointer.
{
    size_t len = strlen(pStr);
    char *sRev = pStr + len - 1;

    while (sRev >= pStr)
    {
        if (*sRev == ' ' || *sRev == '\t' || *sRev == '\n' || *sRev == '\r')
        {
            --sRev;
        }
        else
        {
            break;
        }
    }

    if (sRev != pStr + len - 1)
    {
        *(sRev + 1) = '\0';
    }

    return pStr;
}

// =====================================================================================================================
// Parses a key-value pair.
// NOTE: The function will change the contents of an input string
bool ExtractKeyAndValue(
    char*      pLine,        // [in]  Input key-value pair.
    uint32_t   lineNum,      // Current line number.
    const char delimiter,    // Key-value splitter.
    char**     ppKey,        // [out] Key string, a substring of the input.
    char**     ppValue,      // [out] Value string, a substring of the input.
    std::string* pErrorMsg)  // [out] Error message
{
    bool result = true;

    char* pEqual = strchr(pLine, delimiter);
    if (pEqual != nullptr)
    {
        *ppKey = TrimStringBeginning(pLine);

        // Terminates key string.
        *pEqual = '\0';

        *ppValue = pEqual + 1;
        if (**ppValue != '\0')
        {
            *ppValue = TrimStringBeginning(*ppValue);
        }
        else
        {
            PARSE_ERROR(*pErrorMsg, lineNum, "Expect value after %c", delimiter);
            result = false;
        }
    }
    else
    {
        PARSE_ERROR(*pErrorMsg, lineNum, "Expect %c", delimiter);
        result = false;
    }

    if (result != false)
    {
        TrimStringEnd(*ppKey);
        TrimStringEnd(*ppValue);
    }

    return result;
}

// =====================================================================================================================
// Parses an array index access in a pair of brackets.
bool ParseArrayAccess(
    char*        pStr,            // [in]  Input string pointer
    uint32_t     lineNum,         // Line number used to report error
    uint32_t*    pArrayIndex,     // [out] Parsed array index result
    char**       ppLBracket,      // [out] Position of '['
    char**       ppRBracket,      // [out] Position of ']'
    std::string* pErrorMsg)       // [out] Error message
{
    bool result = true;

    char* pLBracket = strchr(pStr, '[');
    char* pRBracket = strchr(pStr, ']');
    if ((pLBracket == nullptr) || (pRBracket == nullptr))
    {
        PARSE_ERROR(*pErrorMsg, lineNum, "Expect [] for array access");
        result = false;
    }

    if (result == true)
    {
        if (ppLBracket != nullptr)
        {
            *ppLBracket = pLBracket;
        }
        if (ppRBracket != nullptr)
        {
            *ppRBracket = pRBracket;
        }
        if (pArrayIndex != nullptr)
        {
            uint32_t arrayIndex = strtol(pLBracket + 1, nullptr, 10);
            *pArrayIndex = arrayIndex;
        }
    }

    return result;
}

// =====================================================================================================================
// Checks if a string contains array index access, which is a digits string inside a pair of brackets.
bool IsArrayAccess(
    const char* pStr)   // [in] Input string pointer
{
    bool result = true;

    const char* pLBracket = strchr(pStr, '[');
    const char* pRBracket = strchr(pStr, ']');
    if ((pLBracket == nullptr) || (pRBracket == nullptr))
    {
        result = false;
    }

    if (result == true)
    {
        for (const char* p = pLBracket + 1; p != pRBracket; ++p)
        {
            if ((*p >= '0' && *p <= '9') ||
                *p == ' ' ||
                *p == '\t')
            {
                continue;
            }
            else
            {
                result = false;
                break;
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Gets one word for a string and return the start position of next word, nullptr is returned if word isn't found
// in the string
char * GetWordFromString(
    char* pStr,           // [in] Input string
    char* pWordBuffer)    // [out] A word from input string
{
    char *p    = TrimStringBeginning(pStr);
    char *pDst = pWordBuffer;

    while ((*p != '\0') && (*p != ' ') && (*p != '\t'))
    {
       *pDst++ = *p++;
    }

    *pDst = '\0';
    return strlen(pWordBuffer)==0 ? nullptr : p;
}

// =====================================================================================================================
// Substitutes marcros for 1 line.
// Returns false if line length after substitution exceeds MaxLineBufSize
bool VfxParser::MacroSubstituteLine(
    char*                  pLine,                 // [in] Line string
    uint32_t               lineNum,               // Line number
    const MacroDefinition* pMacroDefinition,      // [in] Map of macro definitions
    uint32_t               maxLineLength)         // Max line length allowed for the substituted string.
{
    bool result = true;
    VFX_ASSERT(pMacroDefinition != nullptr);

    for (MacroDefinition::const_iterator iter = pMacroDefinition->begin();
         iter != pMacroDefinition->end();
         ++iter)
    {
        const char* pName  = iter->first.c_str();
        const char* pValue = iter->second.c_str();

        char* pNamePos = strstr(pLine, pName);
        if (pNamePos != nullptr)
        {
            size_t nameLen   = strlen(pName);
            size_t valueLen  = strlen(pValue);

            char*  pLineRest = pNamePos + nameLen;
            size_t restLen   = strlen(pLineRest);
            size_t beforeLen = pNamePos - pLine;

            if (beforeLen + valueLen + restLen >= maxLineLength)
            {
                PARSE_ERROR(*m_pErrorMsg, lineNum, "Line length after macro substitution exceeds MaxLineBufSize.");
                result = false;
                break;
            }

            sprintf(pNamePos, "%s%s", pValue, pLineRest);
            pLineRest = pNamePos + nameLen + valueLen;
            MacroDefinition macros2;
            macros2[iter->first] = iter->second;
            result = MacroSubstituteLine(pLineRest, lineNum, &macros2, static_cast<uint32_t>(maxLineLength - beforeLen - valueLen));
            if (result == false)
            {
                break;
            }
        }
    }

    return result;
}

}

namespace Vfx
{
// =====================================================================================================================
// Parses input file
bool VFXAPI vfxParseFile(
    const char*     pFilename,      // [in] Input file name
    unsigned int    numMacro,       // Number of marcos
    const char*     pMacros[],      // [in] Marco list, Two strings are a macro, and macro will be extract before parse
    VfxDocType      type,           // Document type
    void**          ppDoc,          // [out] Document handle
    const char**    ppErrorMsg)     // [out] Error message
{
    VfxParser    parser;
    TestCaseInfo testCase;

    testCase.vfxFile = pFilename;
    for (uint32_t i = 0; i < numMacro / 2; ++i)
    {
        testCase.macros[pMacros[2 * i]] = pMacros[2 * i + 1];
    }

    Document* pDoc = Document::CreateDocument(type);
    bool ret  = parser.Parse(testCase, pDoc);

    *ppDoc  = pDoc;
    *ppErrorMsg = pDoc->GetErrorMsg()->c_str();

    return ret;
}

// =====================================================================================================================
// Closes document handle
void VFXAPI vfxCloseDoc(
    void* pDoc)    // [in] Document handle
{
    delete reinterpret_cast<Document*>(pDoc);
}

// =====================================================================================================================
// Gets render document from document handle
//
// NOTE: The document contents are not accessable after call vfxCloseDoc
void VFXAPI vfxGetRenderDoc(
    void*              pDoc,         // [in] Document handle
    VfxRenderStatePtr* pRenderState) // [out] Pointer of struct VfxRenderState
{
   *pRenderState = reinterpret_cast<RenderDocument*>(pDoc)->GetDocument();
}

// =====================================================================================================================
// Gets pipeline document from document handle
//
// NOTE: The document contents are not accessable after call vfxCloseDoc
void VFXAPI vfxGetPipelineDoc(
    void*                pDoc,            // [in] Document handle
    VfxPipelineStatePtr* pPipelineState)  // [out] Pointer of struct VfxPipelineState
{
   *pPipelineState = reinterpret_cast<PipelineDocument*>(pDoc)->GetDocument();
}

// =====================================================================================================================
// Print Document to STDOUT
void VFXAPI vfxPrintDoc(
    void*                pDoc)            // [in] Document handle
{
   reinterpret_cast<Document*>(pDoc)->PrintSelf();
}

} // Vfx
