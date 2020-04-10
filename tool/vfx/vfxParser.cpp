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
bool parseInt(char* str, unsigned lineNum, IUFValue* output);
bool parseFloat(char* str, unsigned lineNum, IUFValue* output);
bool parseFloat16(char* str, unsigned lineNum, IUFValue* output);
bool parseDouble(char* str, unsigned lineNum, IUFValue* output);

bool parseBool(char* str, unsigned lineNum, IUFValue* output, std::string* errorMsg);

bool parseIVec4(char* str, unsigned lineNum, IUFValue* output);
bool parseI64Vec2(char* str, unsigned lineNum, IUFValue* output);
bool parseFVec4(char* str, unsigned lineNum, IUFValue* output);
bool parseF16Vec4(char* str, unsigned lineNum, IUFValue* output);
bool parseDVec2(char* str, unsigned lineNum, IUFValue* output);

bool parseIArray(char* str, unsigned lineNum, bool isSign, std::vector<uint8_t>& bufMem);
bool parseI64Array(char* str, unsigned lineNum, bool isSign, std::vector<uint8_t>& bufMem);
bool parseFArray(char* str, unsigned lineNum, std::vector<uint8_t>& bufMem);
bool parseF16Array(char* str, unsigned lineNum, std::vector<uint8_t>& bufMem);
bool parseDArray(char* str, unsigned lineNum, std::vector<uint8_t>& bufMem);

bool parseBinding(char* str, unsigned lineNum, IUFValue* output);

bool parseEnumName(char* enumName, unsigned lineNum, IUFValue* output, std::string* errorMsg);

// Trims space at the beginning of a string.
char* trimStringBeginning(char * str);

// Trims space at the end of a string.
char* trimStringEnd(char* str);

// Parses a key-value pair.
bool extractKeyAndValue(char* line, unsigned lineNum, const char delimiter, char** ppKey, char** ppValue, std::string* errorMsg);

// Parses an array index access in a pair of brackets.
bool parseArrayAccess(char* str, unsigned lineNum, unsigned* arrayIndex, char** ppLBracket, char** ppRBracket, std::string* errorMsg);

// Checks if a string contains array index access, which is a digits string inside a pair of brackets.
bool isArrayAccess(const char* str);

// Gets one word for a string and return the start position of next word, nullptr is returned if word isn't found
// in the string
char* getWordFromString(char* str, char* wordBuffer);

// =====================================================================================================================
Document::~Document()
{
    for (unsigned i = 0; i < SectionTypeNameNum; ++i)
    {
        for (unsigned j = 0; j < m_sections[i].size(); ++j)
        {
            delete m_sections[i][j];
        }
        m_sections[i].clear();
    }
}

// =====================================================================================================================
// Creates Vfx document object with specified document type
Document* Document::createDocument(
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
Section* Document::getFreeSection(
    const char* sectionName)      // [in] Section name
{
    Section*    section        = nullptr;
    SectionType type            = Section::getSectionType(sectionName);
    const unsigned maxSectionCount = getMaxSectionCount(type);
    if (m_sections[type].size() < maxSectionCount)
    {
        section = Section::createSection(sectionName);
        m_sections[type].push_back(section);
        m_sectionList.push_back(section);
    }
    return section;
}

// =====================================================================================================================
// Prints all parsed rule based key-values, for debug purpose.
void Document::printSelf()
{
    for (unsigned i = 0; i < SectionTypeNameNum; ++i)
    {
        for (unsigned j = 0; j < m_sections[i].size(); ++j)
        {
            m_sections[i][j]->printSelf(0);
        }
    }
}

// =====================================================================================================================
// Compiles input shader source to SPIRV binary
bool Document::compileShader()
{
    bool ret = true;
    for (unsigned stage = 0; stage < ShaderStageCount; ++stage)
    {
        for (size_t i = 0; i < m_sections[SectionTypeVertexShader + stage].size(); ++i)
        {
            auto shaderSection = m_sections[SectionTypeVertexShader + stage][i];
            VFX_ASSERT(m_sections[SectionTypeVertexShaderInfo + stage].size() > i);
            auto shaderInfoSection = m_sections[SectionTypeVertexShaderInfo + stage][i];
            bool stageRet = reinterpret_cast<SectionShader*>(shaderSection)->compileShader(m_fileName, shaderInfoSection, &m_errorMsg);
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
    m_currentSection(nullptr),
    m_currentLineNum(0),
    m_currentSectionLineNum(0)
{

}

// =====================================================================================================================
// Parses a config file line.
bool VfxParser::parseLine(
    char* line)    // [in] Input test config line.
{
    bool result = true;
    ++m_currentLineNum;

    // Trim comments for blocks other than shader source blocks, shader source strings are passed to compiler as-is.
    if (m_currentSection == nullptr || m_currentSection->isShaderSourceSection() == false)
    {
        char* comments = strchr(line, ';');
        if (comments != nullptr)
        {
            *comments = '\0';
        }
    }

    if (*line == '[')
    {
        result = endSection();
        if (result == true)
        {
            result = beginSection(line);
        }
    }
    else
    {
        m_currentSectionStringBuffer << line;
    }

    return result;
}

// =====================================================================================================================
// Begins a section.
bool VfxParser::beginSection(
    char* line)    // [in] Input test config line.
{
    bool result = true;
    VFX_ASSERT(*line == '[');
    char* bracketBack = strchr(line, ']');
    if (bracketBack != nullptr)
    {
        *bracketBack = '\0';
    }
    else
    {
        PARSE_ERROR(*m_errorMsg, m_currentLineNum, "expect ]");
        result = false;
    }

    if (result)
    {
        line = line + 1;
        char* sectionName = strtok(line, ",");
        m_currentSection = m_vfxDoc->getFreeSection(sectionName);
        if (m_currentSection != nullptr)
        {
            // Next line is the first line of section content.
            m_currentSectionLineNum       = m_currentLineNum + 1;
            m_currentSectionStringBuffer.str("");
            m_currentSectionStringBuffer.clear();
            m_currentSection->setLineNum(m_currentLineNum);
        }
    }

    return result;
}

// =====================================================================================================================
// Ends a section.
bool VfxParser::endSection()
{
    bool result = true;

    if (m_currentSection == nullptr)
    {
        // Do nothing
    }
    else if (m_currentSection->isShaderSourceSection() ||
             (m_currentSection->getSectionType() == SectionTypeCompileLog))
    {
        // Process shader source sections.
        parseSectionShaderSource();
    }
    else
    {
        // Process key-value based sections.
        result = parseSectionKeyValues();
        if (result)
        {
            if (m_currentSection->getSectionType() == SectionTypeVersion)
            {
                unsigned version;
                reinterpret_cast<SectionVersion*>(m_currentSection)->getSubState(version);
                result = m_vfxDoc->checkVersion(version);
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Parses a line of a pre-defined key-value section.
bool VfxParser::parseSectionKeyValues()
{
    bool result = true;

    // Set line number variable which is used in error report.
    unsigned lineNum = m_currentSectionLineNum;
    char lineBuffer[MaxLineBufSize];
    while (true)
    {
        m_currentSectionStringBuffer.getline(lineBuffer, MaxLineBufSize);

        size_t readCount = static_cast<size_t>(m_currentSectionStringBuffer.gcount());
        VFX_ASSERT(readCount < MaxLineBufSize);
        if (readCount == 0)
        {
            break;
        }
        if ((lineBuffer[0] == '\0') || (memcmp(lineBuffer, "\r", 2) == 0))
        {
            // Skip empty line
            continue;
        }

        char* key   = nullptr;
        char* value = nullptr;

        result = extractKeyAndValue(lineBuffer, lineNum, '=', &key, &value, m_errorMsg);

        if (result == false)
        {
            break;
        }

        parseKeyValue(key,
                      value,
                      lineNum,
                      m_currentSection);

        ++lineNum;
    }

    return result;
}

// =====================================================================================================================
// Parses a key string to process array access("[]") and member access(".").
bool VfxParser::parseKey(
    const char*           key,                 // [in]  Input key string
    unsigned              lineNum,              // Line number
    Section*              sectionObjectIn,     // [in]  Base section object
    Section**             ppSectionObjectOut,   // [out] Target section object after apply array access and member
                                                //       access in key string.
    char*                 memberNameBuffer,    // [out] Name of the member to be accessed in target section object.
    unsigned              memberNameBufferSize, // Size of member name buffer.
    unsigned*             arrayIndex)          // [out] Array index applied this member (0 for non array)

{
    bool result = true;
    // Get a copy of key string.
    char keyBuffer[MaxKeyBufSize];
    VFX_ASSERT(strlen(key) < MaxKeyBufSize);
    strcpy(keyBuffer, key);

    VFX_ASSERT(sectionObjectIn != nullptr);
    Section* tempSectionObj = sectionObjectIn;

    // Process member access
    char* keyTok = strtok(keyBuffer, ".");
    keyTok = trimStringBeginning(keyTok);
    keyTok = trimStringEnd(keyTok);

    bool isSection = false;          // Is this member an Section object
    unsigned parsedArrayIndex = 0;        // Array access index
    MemberType memberType;

    while (keyTok != nullptr)
    {
        if (isArrayAccess(keyTok))
        {
            char* lBracket = nullptr;
            result = parseArrayAccess(keyTok, lineNum, &parsedArrayIndex, &lBracket, nullptr, m_errorMsg);
            // Remove bracket from string token
            *lBracket = '\0';
            keyTok = trimStringEnd(keyTok);
        }
        else
        {
            parsedArrayIndex = 0;
        }

        result = tempSectionObj->isSection(lineNum, keyTok, &isSection, &memberType, m_errorMsg);
        if (result == false)
        {
            break;
        }

        if (isSection == false)
        {
            VFX_ASSERT(strlen(keyTok) < memberNameBufferSize);
            strncpy(memberNameBuffer, keyTok, memberNameBufferSize);
        }
        else
        {
            result = tempSectionObj->getPtrOfSubSection(lineNum,
                                                         keyTok,
                                                         memberType,
                                                         true,
                                                         parsedArrayIndex,
                                                         &tempSectionObj,
                                                         m_errorMsg);
            if (result == false)
            {
                break;
            }
        }

        keyTok = strtok(nullptr, ".");
    }

    if (arrayIndex != nullptr)
    {
        *arrayIndex = parsedArrayIndex;
    }

    if (ppSectionObjectOut != nullptr)
    {
        *ppSectionObjectOut = tempSectionObj;
    }

    return result;
}

// =====================================================================================================================
// Parses a key-value pair according to predefined rule.
bool VfxParser::parseKeyValue(
    char*                         key,           // [in] Input key string
    char*                         valueStr,         // [in] Input value string
    unsigned                      lineNum,        // Line number
    Section*                      sectionObject) // [out] Key-value map to hold the parse results.
{
    bool result = false;

    Section* accessedSectionObject = nullptr;
    unsigned arrayIndex = 0;
    char memberName[MaxKeyBufSize];
    result = parseKey(key,
                      lineNum,
                      sectionObject,
                      &accessedSectionObject,
                      memberName,
                      MaxKeyBufSize,
                      &arrayIndex);

    if (result == true)
    {
        MemberType valueType;
        result = accessedSectionObject->getMemberType(lineNum, memberName, &valueType, m_errorMsg);

        if (result == true)
        {
            IUFValue value = {};

            // Parse value according to it's type
            switch (valueType)
            {
            case MemberTypeEnum:
                {
                    result = parseEnumName(valueStr, lineNum, &value, m_errorMsg);
                    if (result == true)
                    {
                        result = accessedSectionObject->set(lineNum, memberName, &(value.iVec4[0]));
                    }
                    break;
                }
            case MemberTypeInt:
                {
                    result = parseInt(valueStr, lineNum, &value);
                    if (result == true)
                    {
                        result = accessedSectionObject->set(lineNum, memberName, &(value.iVec4[0]));
                    }
                    break;
                }
            case MemberTypeFloat:
                {
                    result = parseFloat16(valueStr, lineNum, &value);
                    if (result == true)
                    {
                        result = accessedSectionObject->set(lineNum, memberName, &(value.f16Vec4[0]));
                    }
                    break;
                }
            case MemberTypeDouble:
                {
                    result = parseDouble(valueStr, lineNum, &value);
                    if (result == true)
                    {
                        result = accessedSectionObject->set(lineNum, memberName, &(value.dVec2[0]));
                    }
                    break;
                }
            case MemberTypeBool:
                {
                    result = parseBool(valueStr, lineNum, &value, m_errorMsg);
                    if (result == true)
                    {
                        static_assert(sizeof(uint8_t) == sizeof(bool), "");
                        uint8_t boolValue = value.iVec4[0] ? 1 : 0;
                        result = accessedSectionObject->set(lineNum, memberName, &boolValue);
                    }
                    break;
                }
            case MemberTypeIVec4:
                {
                    result = parseIVec4(valueStr, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = accessedSectionObject->set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeI64Vec2:
                {
                    result = parseI64Vec2(valueStr, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = accessedSectionObject->set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeBinding:
                {
                    result = parseBinding(valueStr, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = accessedSectionObject->set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeFVec4:
                {
                    result = parseFVec4(valueStr, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = accessedSectionObject->set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeF16Vec4:
                {
                    result = parseF16Vec4(valueStr, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = accessedSectionObject->set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeDVec2:
                {
                    result = parseDVec2(valueStr, lineNum, &value);
                    if (result == false)
                    {
                        break;
                    }
                    result = accessedSectionObject->set(lineNum, memberName, &value);
                    break;
                }
            case MemberTypeIArray:
            case MemberTypeUArray:
                {
                    std::vector<uint8_t>** ppIntData = nullptr;
                    accessedSectionObject->getPtrOf(lineNum, memberName, true, 0, &ppIntData, m_errorMsg);
                    result = parseIArray(valueStr, lineNum, valueType == MemberTypeIArray, **ppIntData);
                    break;
                }
            case MemberTypeI64Array:
            case MemberTypeU64Array:
                {
                    std::vector<uint8_t>** ppIntData = nullptr;
                    accessedSectionObject->getPtrOf(lineNum, memberName, true, 0, &ppIntData, m_errorMsg);
                    result = parseI64Array(valueStr, lineNum, valueType == MemberTypeI64Array, **ppIntData);
                    break;
                }
            case MemberTypeFArray:
                {
                    std::vector<uint8_t>** ppFloatData = nullptr;
                    accessedSectionObject->getPtrOf(lineNum, memberName, true, 0, &ppFloatData, m_errorMsg);
                    result = parseFArray(valueStr, lineNum, **ppFloatData);
                    break;
                }
            case MemberTypeF16Array:
                {
                    std::vector<uint8_t>** ppFloatData = nullptr;
                    accessedSectionObject->getPtrOf(lineNum, memberName, true, 0, &ppFloatData, m_errorMsg);
                    result = parseF16Array(valueStr, lineNum, **ppFloatData);
                    break;
                }
            case MemberTypeDArray:
                {
                    std::vector<uint8_t>** ppDoubleData;
                    accessedSectionObject->getPtrOf(lineNum, memberName, true, 0, &ppDoubleData, m_errorMsg);
                    result = parseDArray(valueStr, lineNum, **ppDoubleData);
                    break;
                }
            case MemberTypeString:
                {
                    std::string str = valueStr;
                    result = accessedSectionObject->set(lineNum, memberName, &str);
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
void VfxParser::parseSectionShaderSource()
{
    char lineBuffer[MaxLineBufSize];

    while (true)
    {
        m_currentSectionStringBuffer.getline(lineBuffer, MaxLineBufSize);

        size_t readCount = static_cast<size_t>(m_currentSectionStringBuffer.gcount());
        VFX_ASSERT(readCount < MaxLineBufSize);
        if (readCount == 0)
        {
            break;
        }

        // Line ending is not returned by getline(), so append them manually.
        lineBuffer[readCount- 1] = '\n';
        lineBuffer[readCount]    = '\0';
        m_currentSection->addLine(lineBuffer);
    }
}

// =====================================================================================================================
// Parses a VFX config file.
bool VfxParser::parse(
    const TestCaseInfo& info,  // [in] Name of VFX file to parse.
    Document*           doc)  // [out] Parse result
{
    bool result = true;
    m_vfxDoc   = doc;
    m_errorMsg = doc->getErrorMsg();

    FILE* configFile = fopen(info.vfxFile.c_str(), "r");
    if (configFile != nullptr)
    {
        doc->setFileName(info.vfxFile);
        char lineBuf[MaxLineBufSize];
        char* linePtr = nullptr;

        while (true)
        {
            linePtr = fgets(lineBuf, MaxLineBufSize, configFile);

            if (linePtr == nullptr)
            {
                result = endSection();
                break;
            }
            else
            {
                result = macroSubstituteLine(linePtr, m_currentLineNum+1, &info.macros, MaxLineBufSize);
                if (result == false)
                {
                    break;
                }

                result = parseLine(linePtr);
                if (result == false)
                {
                    break;
                }
            }
        }

        fclose(configFile);

        if (result)
        {
            result = m_vfxDoc->validate();
        }

        if (result)
        {
            result = m_vfxDoc->compileShader();
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
bool parseInt(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = true;

    bool isHex = false;
    char* p0x = strstr(str, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    if (isHex)
    {
        output->uVec4[0] = strtoul(str, nullptr, 0);
    }
    else
    {
        output->iVec4[0] = strtol(str, nullptr, 0);
    }

    output->props.isInt64 = false;
    output->props.isFloat = false;
    output->props.isDouble = false;
    output->props.isHex = isHex;
    output->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parses a float number from a string.
bool parseFloat(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = true;

    output->fVec4[0] = static_cast<float>(strtod(str, nullptr));

    output->props.isInt64 = false;
    output->props.isFloat = true;
    output->props.isDouble = false;
    output->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parses a float16 number from a string.
bool parseFloat16(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = true;

    float v = static_cast<float>(strtod(str, nullptr));
    Float16 v16;
    v16.FromFloat32(v);
    output->f16Vec4[0] = v16;

    output->props.isInt64      = false;
    output->props.isFloat      = false;
    output->props.isFloat16    = true;
    output->props.isDouble     = false;
    output->props.length       = 1;

    return result;
}

// =====================================================================================================================
// Parses a double number from a string.
bool parseDouble(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = true;

    output->dVec2[0] = strtod(str, nullptr);

    output->props.isInt64 = false;
    output->props.isFloat = false;
    output->props.isDouble = true;
    output->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parse a boolean value from a string.
bool parseBool(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output,    // [out] Stores parsed value
    std::string* errorMsg)
{
    VFX_ASSERT(output != nullptr);
    bool result = true;

    if (strcmp(str, "true") == 0)
    {
        output->iVec4[0] = 1;
    }
    else if (strcmp(str, "false") == 0)
    {
        output->iVec4[0] = 0;
    }
    else
    {
        output->iVec4[0] = strtol(str, nullptr, 0);
    }

    output->props.isInt64 = false;
    output->props.isFloat = false;
    output->props.isDouble = false;
    output->props.length = 1;

    return result;
}

// =====================================================================================================================
// Parses a integer vec4 from a string.
// NOTE: content of pStr will be changed.
bool parseIVec4(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = false;

    bool isHex = false;
    char* p0x = strstr(str, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    char* number = strtok(str, ", ");
    unsigned numberId = 0;
    while (number != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 4);
        if (isHex == true)
        {
            output->uVec4[numberId] = strtoul(number, nullptr, 0);
        }
        else
        {
            output->iVec4[numberId] = strtol(number, nullptr, 0);
        }
        number = strtok(nullptr, ", ");
        ++numberId;
    }

    output->props.isInt64 = false;
    output->props.isFloat = false;
    output->props.isDouble = false;
    output->props.isHex = isHex;
    output->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a int64 vec2 from a string.
// NOTE: content of pStr will be changed.
bool parseI64Vec2(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = false;

    bool isHex = false;
    char* p0x = strstr(str, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    char* number = strtok(str, ", ");
    unsigned numberId = 0;
    while (number != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 2);
        if (isHex == true)
        {
            output->i64Vec2[numberId] = strtoull(number, nullptr, 0);
        }
        else
        {
            output->i64Vec2[numberId] = strtoll(number, nullptr, 0);
        }
        number = strtok(nullptr, ", ");
        ++numberId;
    }

    output->props.isInt64 = true;
    output->props.isFloat = false;
    output->props.isDouble = false;
    output->props.isHex = isHex;
    output->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a float vec4 from a string.
// NOTE: content of pStr will be changed.
bool parseFVec4(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = false;

    char* number = strtok(str, ", ");
    unsigned numberId = 0;
    while (number != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 4);

        output->fVec4[numberId] = static_cast<float>(strtod(number, nullptr));

        number = strtok(nullptr, ", ");
        ++numberId;
    }

    output->props.isInt64 = false;
    output->props.isFloat = true;
    output->props.isDouble = false;
    output->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a float16 vec4 from a string.
// NOTE: content of pStr will be changed.
bool parseF16Vec4(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = false;

    char* number = strtok(str, ", ");
    unsigned numberId = 0;
    while (number != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 4);

        float v = static_cast<float>(strtod(number, nullptr));
        Float16 v16;
        v16.FromFloat32(v);
        output->f16Vec4[numberId] = v16;

        number = strtok(nullptr, ", ");
        ++numberId;
    }

    output->props.isInt64      = false;
    output->props.isFloat      = false;
    output->props.isFloat16    = true;
    output->props.isDouble     = false;
    output->props.length       = numberId;

    return result;
}

// =====================================================================================================================
// Parses a double vec2 from a string.
// NOTE: content of pStr will be changed.
bool parseDVec2(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = false;

    char* number = strtok(str, ", ");
    unsigned numberId = 0;
    while (number != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 2);

        output->dVec2[numberId] = strtod(number, nullptr);

        number = strtok(nullptr, ", ");
        ++numberId;
    }

    output->props.isInt64 = false;
    output->props.isFloat = false;
    output->props.isDouble = true;
    output->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated integer values
// NOTE: content of pStr will be changed.
bool parseIArray(
    char*                  str,       // [in]  Input string
    unsigned               lineNum,    // Current line number
    bool                   isSign,     // True if it is signed integer
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* number = strtok(str, ", ");
    while (number != nullptr)
    {
        bool isHex = false;
        char* p0x = strstr(number, "0x");
        if (p0x != nullptr)
        {
            isHex = true;
        }

        union
        {
            int  iVal;
            unsigned uVal;
            uint8_t  val[4];
        };
        iVal = 0;

        if ((isHex == true) || (isSign == false))
        {
            uVal = strtoul(number, nullptr, 0);
        }
        else
        {
            iVal = strtol(number, nullptr, 0);
        }

        for (unsigned i = 0; i < sizeof(val); ++i)
        {
            bufMem.push_back(val[i]);
        }

        number = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated int64 values
// NOTE: content of pStr will be changed.
bool parseI64Array(
    char*                  str,       // [in]  Input string
    unsigned               lineNum,    // Current line number
    bool                   isSign,     // True if it is signed integer
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* number = strtok(str, ", ");
    while (number != nullptr)
    {
        bool isHex = false;
        char* p0x = strstr(number, "0x");
        if (p0x != nullptr)
        {
            isHex = true;
        }

        union
        {
            int64_t  i64Val;
            uint64_t u64Val;
            unsigned uVal[2];
            uint8_t  val[8];
        };
        i64Val = 0;

        if ((isHex == true) || (isSign == false))
        {
            u64Val = strtoull(number, nullptr, 0);
        }
        else
        {
            i64Val = strtoll(number, nullptr, 0);
        }

        for (unsigned i = 0; i < sizeof(val); ++i)
        {
            bufMem.push_back(val[i]);
        }

        number = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated float values
// NOTE: content of pStr will be changed.
bool parseFArray(
    char*                  str,       // [in]  Input string
    unsigned               lineNum,    // Current line number
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* number = strtok(str, ", ");
    while (number != nullptr)
    {
        union
        {
           float    fVal;
           unsigned uVal;
           uint8_t  val[4];
        };

        fVal = static_cast<float>(strtod(number, nullptr));

        for (unsigned i = 0; i < sizeof(val); ++i)
        {
            bufMem.push_back(val[i]);
        }

        number = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated float16 values
// NOTE: content of pStr will be changed.
bool parseF16Array(
    char*                  str,       // [in]  Input string
    unsigned               lineNum,    // Current line number
    std::vector<uint8_t>&  bufMem)     // [in,out] Buffer data
{
    bool result = true;

    char* number = strtok(str, ", ");
    while (number != nullptr)
    {
        union
        {
           Float16Bits fVal;
           uint16_t    uVal;
           uint8_t     val[2];
        };

        float v = static_cast<float>(strtod(number, nullptr));
        Float16 v16;
        v16.FromFloat32(v);
        fVal = v16.GetBits();

        for (unsigned i = 0; i < sizeof(val); ++i)
        {
            bufMem.push_back(val[i]);
        }

        number = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses an array of comma separated double values
// NOTE: content of pStr will be changed.
bool parseDArray(
    char*               str,          // [in]  Input string
    unsigned            lineNum,       // Current line number
    std::vector<uint8_t>& bufMem)      // [in,out] Buffer data
{
    bool result = true;

    char* number = strtok(str, ", ");
    while (number != nullptr)
    {
        union
        {
           double   dVal;
           unsigned uVal[2];
           uint8_t  val[8];
        };

        dVal = strtod(number, nullptr);

        for (unsigned i = 0; i < sizeof(val); ++i)
        {
            bufMem.push_back(val[i]);
        }

        number = strtok(nullptr, ", ");
    }

    return result;
}

// =====================================================================================================================
// Parses binding, it's a integer vec3 from a string.
// NOTE: content of pStr will be changed.
bool parseBinding(
    char*       str,       // [in]  Input string
    unsigned    lineNum,    // Current line number
    IUFValue*   output)    // [out] Stores parsed value
{
    VFX_ASSERT(output != nullptr);
    bool result = false;

    bool isHex = false;
    char* p0x = strstr(str, "0x");
    if (p0x != nullptr)
    {
        isHex = true;
    }

    char* number = strtok(str, ", ");
    unsigned numberId = 0;
    while (number != nullptr)
    {
        result = true;
        VFX_ASSERT(numberId < 3);
        if (strcmp(number, "vb") == 0)
        {
            output->uVec4[numberId] = VfxVertexBufferSetId;
        }
        else if (strcmp(number, "ib") == 0)
        {
            output->uVec4[numberId] = VfxIndexBufferSetId;
        }
        else
        {
            if (isHex == true)
            {
                output->uVec4[numberId] = strtoul(number, nullptr, 0);
            }
            else
            {
                output->iVec4[numberId] = strtol(number, nullptr, 0);
            }
        }
        number = strtok(nullptr, ", ");
        ++numberId;
    }

    output->props.isInt64 = false;
    output->props.isFloat = false;
    output->props.isDouble = false;
    output->props.isHex = isHex;
    output->props.length = numberId;

    return result;
}

// =====================================================================================================================
// Parses a enum string
bool parseEnumName(
    char*        enumName,  // Enum name
    unsigned     lineNum,    // Line No.
    IUFValue*    output,    // [Out] Enum value
    std::string* errorMsg)  // [Out] Error message
{
    bool result = false;
    int value = VfxInvalidValue;
    result = getEnumValue(enumName, value);

    if (result == false)
    {
        PARSE_ERROR(*errorMsg, lineNum, "unknow enum");
    }
    else
    {
        output->iVec4[0] = value;
    }

    return result;
}

// =====================================================================================================================
// Trims space at the beginning of a string.
char * trimStringBeginning(
    char * str)    // [in] Input string pointer.
{
    while (*str)
    {
        if (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
        {
            ++str;
        }
        else
        {
            break;
        }
    }
    return str;
}

// =====================================================================================================================
// Trims space at the end of a string.
// NOTE: The function will change the contents of an input string
char * trimStringEnd(
    char * str)    // [in] Input string pointer.
{
    size_t len = strlen(str);
    char *sRev = str + len - 1;

    while (sRev >= str)
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

    if (sRev != str + len - 1)
    {
        *(sRev + 1) = '\0';
    }

    return str;
}

// =====================================================================================================================
// Parses a key-value pair.
// NOTE: The function will change the contents of an input string
bool extractKeyAndValue(
    char*      line,        // [in]  Input key-value pair.
    unsigned   lineNum,      // Current line number.
    const char delimiter,    // Key-value splitter.
    char**     ppKey,        // [out] Key string, a substring of the input.
    char**     ppValue,      // [out] Value string, a substring of the input.
    std::string* errorMsg)  // [out] Error message
{
    bool result = true;

    char* equal = strchr(line, delimiter);
    if (equal != nullptr)
    {
        *ppKey = trimStringBeginning(line);

        // Terminates key string.
        *equal = '\0';

        *ppValue = equal + 1;
        if (**ppValue != '\0')
        {
            *ppValue = trimStringBeginning(*ppValue);
        }
        else
        {
            PARSE_ERROR(*errorMsg, lineNum, "Expect value after %c", delimiter);
            result = false;
        }
    }
    else
    {
        PARSE_ERROR(*errorMsg, lineNum, "Expect %c", delimiter);
        result = false;
    }

    if (result != false)
    {
        trimStringEnd(*ppKey);
        trimStringEnd(*ppValue);
    }

    return result;
}

// =====================================================================================================================
// Parses an array index access in a pair of brackets.
bool parseArrayAccess(
    char*        str,            // [in]  Input string pointer
    unsigned     lineNum,         // Line number used to report error
    unsigned*    arrayIndex,     // [out] Parsed array index result
    char**       ppLBracket,      // [out] Position of '['
    char**       ppRBracket,      // [out] Position of ']'
    std::string* errorMsg)       // [out] Error message
{
    bool result = true;

    char* lBracket = strchr(str, '[');
    char* rBracket = strchr(str, ']');
    if ((lBracket == nullptr) || (rBracket == nullptr))
    {
        PARSE_ERROR(*errorMsg, lineNum, "Expect [] for array access");
        result = false;
    }

    if (result == true)
    {
        if (ppLBracket != nullptr)
        {
            *ppLBracket = lBracket;
        }
        if (ppRBracket != nullptr)
        {
            *ppRBracket = rBracket;
        }
        if (arrayIndex != nullptr)
        {
            unsigned parsedArrayIndex = strtol(lBracket + 1, nullptr, 10);
            *arrayIndex = parsedArrayIndex;
        }
    }

    return result;
}

// =====================================================================================================================
// Checks if a string contains array index access, which is a digits string inside a pair of brackets.
bool isArrayAccess(
    const char* str)   // [in] Input string pointer
{
    bool result = true;

    const char* lBracket = strchr(str, '[');
    const char* rBracket = strchr(str, ']');
    if ((lBracket == nullptr) || (rBracket == nullptr))
    {
        result = false;
    }

    if (result == true)
    {
        for (const char* p = lBracket + 1; p != rBracket; ++p)
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
char * getWordFromString(
    char* str,           // [in] Input string
    char* wordBuffer)    // [out] A word from input string
{
    char *p    = trimStringBeginning(str);
    char *dst = wordBuffer;

    while ((*p != '\0') && (*p != ' ') && (*p != '\t'))
    {
       *dst++ = *p++;
    }

    *dst = '\0';
    return strlen(wordBuffer)==0 ? nullptr : p;
}

// =====================================================================================================================
// Substitutes marcros for 1 line.
// Returns false if line length after substitution exceeds MaxLineBufSize
bool VfxParser::macroSubstituteLine(
    char*                  line,                 // [in] Line string
    unsigned               lineNum,               // Line number
    const MacroDefinition* macroDefinition,      // [in] Map of macro definitions
    unsigned               maxLineLength)         // Max line length allowed for the substituted string.
{
    bool result = true;
    VFX_ASSERT(macroDefinition != nullptr);

    for (MacroDefinition::const_iterator iter = macroDefinition->begin();
         iter != macroDefinition->end();
         ++iter)
    {
        const char* name  = iter->first.c_str();
        const char* value = iter->second.c_str();

        char* namePos = strstr(line, name);
        if (namePos != nullptr)
        {
            size_t nameLen   = strlen(name);
            size_t valueLen  = strlen(value);

            char*  lineRest = namePos + nameLen;
            size_t restLen   = strlen(lineRest);
            size_t beforeLen = namePos - line;

            if (beforeLen + valueLen + restLen >= maxLineLength)
            {
                PARSE_ERROR(*m_errorMsg, lineNum, "Line length after macro substitution exceeds MaxLineBufSize.");
                result = false;
                break;
            }

            sprintf(namePos, "%s%s", value, lineRest);
            lineRest = namePos + nameLen + valueLen;
            MacroDefinition macros2;
            macros2[iter->first] = iter->second;
            result = macroSubstituteLine(lineRest, lineNum, &macros2, static_cast<unsigned>(maxLineLength - beforeLen - valueLen));
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
    const char*     filename,      // [in] Input file name
    unsigned int    numMacro,       // Number of marcos
    const char*     macros[],      // [in] Marco list, Two strings are a macro, and macro will be extract before parse
    VfxDocType      type,           // Document type
    void**          ppDoc,          // [out] Document handle
    const char**    ppErrorMsg)     // [out] Error message
{
    VfxParser    parser;
    TestCaseInfo testCase;

    testCase.vfxFile = filename;
    for (unsigned i = 0; i < numMacro / 2; ++i)
    {
        testCase.macros[macros[2 * i]] = macros[2 * i + 1];
    }

    Document* doc = Document::createDocument(type);
    bool ret  = parser.parse(testCase, doc);

    *ppDoc  = doc;
    *ppErrorMsg = doc->getErrorMsg()->c_str();

    return ret;
}

// =====================================================================================================================
// Closes document handle
void VFXAPI vfxCloseDoc(
    void* doc)    // [in] Document handle
{
    delete reinterpret_cast<Document*>(doc);
}

// =====================================================================================================================
// Gets render document from document handle
//
// NOTE: The document contents are not accessable after call vfxCloseDoc
void VFXAPI vfxGetRenderDoc(
    void*              doc,         // [in] Document handle
    VfxRenderStatePtr* renderState) // [out] Pointer of struct VfxRenderState
{
   *renderState = reinterpret_cast<RenderDocument*>(doc)->getDocument();
}

// =====================================================================================================================
// Gets pipeline document from document handle
//
// NOTE: The document contents are not accessable after call vfxCloseDoc
void VFXAPI vfxGetPipelineDoc(
    void*                doc,            // [in] Document handle
    VfxPipelineStatePtr* pipelineState)  // [out] Pointer of struct VfxPipelineState
{
   *pipelineState = reinterpret_cast<PipelineDocument*>(doc)->getDocument();
}

// =====================================================================================================================
// Print Document to STDOUT
void VFXAPI vfxPrintDoc(
    void*                doc)            // [in] Document handle
{
   reinterpret_cast<Document*>(doc)->printSelf();
}

} // Vfx
