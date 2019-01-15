/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  vfxParser.h
* @brief Contains declarations needed to parse a vulkan FX test file.
***********************************************************************************************************************
*/

#pragma once

#include <string.h>
#include <stddef.h>
#include <vector>
#include <sstream>
#include <map>

#include "vfxSection.h"

namespace Vfx
{

typedef std::map<std::string, std::string> MacroDefinition;

// =====================================================================================================================
// Represents the information of one test case, include file name and parameters
struct TestCaseInfo
{
    std::string                        vfxFile;   // The file name of vfx file
    MacroDefinition                    macros;    // The macros of this test
};

// =====================================================================================================================
// Represents the parse result of Vfx parser
class Document
{
public:
    Document() {}
    virtual ~Document();

    virtual uint32_t GetMaxSectionCount(SectionType type) = 0;
    virtual bool CheckVersion(uint32_t ver) { return true; }
    virtual bool Validate() { return true; }

    static Document* CreateDocument(VfxDocType type);

    Section* GetFreeSection(const char* pSectionName);
    void PrintSelf();
    bool CompileShader();
    void SetFileName(const std::string& fileName) { m_fileName = fileName; }

    std::string* GetErrorMsg()  { return &m_errorMsg; }

protected:
    std::vector<Section*>  m_sections[SectionTypeNameNum]; // Contains sections
    std::vector<Section*>  m_sectionList;
    std::string            m_errorMsg;                     // Error message

private:
    std::string            m_fileName;                     // Name of source file
};

// =====================================================================================================================
// Represents the Vfx parser
class VfxParser
{
public:
    VfxParser();
    bool IsValidVfxFile() { return m_isValidVfxFile; }

    bool Parse(const TestCaseInfo& info, Document* pDoc);

private:
    bool MacroSubstituteLine(char* pLine,
                             uint32_t lineNum,
                             const MacroDefinition* pMacroDefinition,
                             uint32_t maxLineLength);

    bool ParseLine(char* pLine);

    bool BeginSection(char* pLine);

    bool EndSection();

    void ParseSectionShaderSource();

    bool ParseSectionKeyValues();

    bool ParseKey(const char* pKey,
                  uint32_t    lineNum,
                  Section*    pSectionObjectIn,
                  Section**   ppSectionObjectOut,
                  char*       pSectionMemberName,
                  uint32_t    memberNameBufferSize,
                  uint32_t*   pArrayIndex);

    bool ParseKeyValue(char*      pKey,
                       char*      pValue,
                       uint32_t   lineNum,
                       Section*   pSectionObject);

    Document*           m_pVfxDoc;                       // Parse result
    bool                m_isValidVfxFile;                // If vfx file is valid
    Section*            m_pCurrentSection;               // Current section
    uint32_t            m_currentLineNum;                // Current line number
    std::stringstream   m_currentSectionStringBuffer;    // Current section string buffer
    uint32_t            m_currentSectionLineNum;         // Current section line number
    std::string*        m_pErrorMsg;                     // Error message
};

}

