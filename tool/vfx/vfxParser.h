/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "vfxSection.h"
#include <map>
#include <sstream>
#include <stddef.h>
#include <string.h>
#include <vector>

namespace Vfx {

typedef std::map<std::string, std::string> MacroDefinition;

// =====================================================================================================================
// Represents the information of one test case, include file name and parameters
struct TestCaseInfo {
  std::string vfxFile;    // The file name of vfx file
  MacroDefinition macros; // The macros of this test
};

// =====================================================================================================================
// Represents the VFX parser
class Document {
public:
  Document();
  virtual ~Document();

  virtual unsigned getMaxSectionCount(SectionType type) = 0;
  virtual bool checkVersion(unsigned ver) { return true; }
  virtual bool validate() { return true; }
  virtual Section *createSection(const char *sectionName);
  virtual bool getPtrOfSubSection(Section *pSection, unsigned lineNum, const char *memberName, MemberType memberType,
                                  bool isWriteAccess, unsigned arrayIndex, Section **ptrOut, std::string *errorMsg);

  static Document *createDocument(VfxDocType type);

  Section *getFreeSection(const char *sectionName);
  void printSelf();
  bool compileShader();
  void setFileName(const std::string &fileName) { m_fileName = fileName; }

  std::string *getErrorMsg() { return &m_errorMsg; }

  bool isValidVfxFile() { return m_isValidVfxFile; }

  bool parse(const TestCaseInfo &info);

private:
  bool macroSubstituteLine(char *line, unsigned lineNum, const MacroDefinition *macroDefinition,
                           unsigned maxLineLength);

  bool parseLine(char *line);

  bool beginSection(char *line);

  bool endSection();

  void parseSectionShaderSource();

  bool parseSectionKeyValues();

  bool parseKey(const char *key, unsigned lineNum, Section *sectionObjectIn, Section **ppSectionObjectOut,
                char *sectionMemberName, unsigned memberNameBufferSize, unsigned *arrayIndex);

  bool parseKeyValue(char *key, char *value, unsigned lineNum, Section *sectionObject);

protected:
  std::vector<Section *> m_sections[SectionTypeNameNum]; // Contains sections
  std::vector<Section *> m_sectionList;                  // All sections ordered with line number
  std::string m_errorMsg;                                // Error message
  std::string m_fileName;                                // Name of source file

  bool m_isValidVfxFile;                          // If VFX file is valid
  Section *m_currentSection;                      // Current section
  unsigned m_currentLineNum;                      // Current line number
  std::stringstream m_currentSectionStringBuffer; // Current section string buffer
  unsigned m_currentSectionLineNum;               // Current section line number
};

// Splits the input string by modifying it in place. Returns a vector of (inner) fragment strings. This can be used
// thread-safe replacement for `strtok`, although the semantics are not identical.
std::vector<char *> split(char *str, const char *delimiters);

} // namespace Vfx
