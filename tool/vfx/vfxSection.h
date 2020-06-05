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
* @file  vfxSection.h
* @brief Contains declaration of class Section and derived classes
***********************************************************************************************************************
*/

#pragma once
#include "vfx.h"
#include "vfxError.h"
#include <map>
#include <sstream>
#include <stddef.h>
#include <string.h>
#include <vector>

namespace Vfx {
// =====================================================================================================================
// Enumerates VFX section type.
enum SectionType : unsigned {
  SectionTypeUnset = 0, // Initial state, not entering any section.
  // Beginning of rule based key-value sections
  SectionTypeResult,        // Result section
  SectionTypeBufferView,    // Buffer view section
  SectionTypeVertexState,   // Vertex state section
  SectionTypeDrawState,     // Draw state section
  SectionTypeImageView,     // Image view section
  SectionTypeSampler,       // Sampler section
  SectionTypeVersion,       // Version section
  SectionTypeGraphicsState, // Graphics state section
  SectionTypeComputeState,  // Compute state section
  SectionTypeVertexInputState,      // Vertex input state section
  SectionTypeVertexShaderInfo,      // Vertex shader info section
  SectionTypeTessControlShaderInfo, // Tess control shader info section
  SectionTypeTessEvalShaderInfo,    // Tess enval shader info section
  SectionTypeGeometryShaderInfo,    // Geometry shader info section
  SectionTypeFragmentShaderInfo,    // Fragment shader info section
  SectionTypeComputeShaderInfo,     // Compute shader info section
  SectionTypeCompileLog,        // Compile log section
  SectionTypeVertexShader,      // Vertex shader source section
  SectionTypeTessControlShader, // Tess control shader source section
  SectionTypeTessEvalShader,    // Tess eval shader source section
  SectionTypeGeometryShader,    // Geometry shader source section
  SectionTypeFragmentShader,    // Fragment shader source section
  SectionTypeComputeShader,     // Compute shader source section
  SectionTypeNameNum, // Name num section
};

// =====================================================================================================================
// Enumerates VFX member type.
enum MemberType : unsigned {
  MemberTypeInt,                      // VFX member type: 32 bit integer
  MemberTypeFloat,                    // VFX member type: 32 bit float
  MemberTypeFloat16,                  // VFX member type: 16 bit float
  MemberTypeDouble,                   // VFX member type: 64 bit double
  MemberTypeBool,                     // VFX member type: boolean
  MemberTypeIVec4,                    // VFX member type: int vec4
  MemberTypeI64Vec2,                  // VFX member type: int64 vec2
  MemberTypeFVec4,                    // VFX member type: float vec4
  MemberTypeF16Vec4,                  // VFX member type: float16 vec4
  MemberTypeDVec2,                    // VFX member type: double vec2
  MemberTypeIArray,                   // VFX member type: int vector (dynamic array)
  MemberTypeUArray,                   // VFX member type: uint vector (dynamic array)
  MemberTypeI64Array,                 // VFX member type: int64 vector (dynamic array)
  MemberTypeU64Array,                 // VFX member type: uint64 vector (dynamic array)
  MemberTypeFArray,                   // VFX member type: float vector (dynamic array)
  MemberTypeF16Array,                 // VFX member type: float16 vector (dynamic array)
  MemberTypeDArray,                   // VFX member type: double vector (dynamic array)
  MemberTypeEnum,                     // VFX member type: Enums from Vulkan API
  MemberTypeBinding,                  // VFX member type: Binding
  MemberTypeString,                   // VFX member type: String
  MemberTypeResultItem,               // VFX member type: SectionResultItem
  MemberTypeVertexBufferBindingItem,  // VFX member type: SectionVertexBufferBinding
  MemberTypeVertexAttributeItem,      // VFX member type: SectionVertexAttribute
  MemberTypeSpecConstItem,            // VFX member type: SectionSpecConstItem
  MemberTypeSpecConst,                // VFX member type: SectionSpecConst
  MemberTypePushConstRange,           // VFX member type: SectionPushConstRange
  MemberTypeVertexInputBindingItem,   // VFX member type: SectionVertexInputBinding
  MemberTypeVertexInputAttributeItem, // VFX member type: SectionVertexInputAttribute
  MemberTypeVertexInputDivisorItem,   // VFX member type: SectionVertexInputDivisor
  MemberTypeColorBufferItem,          // VFX member type: SectionColorBuffer
  MemberTypeSpecEntryItem,            // VFX member type: SectionSpecEntryItem
  MemberTypeResourceMappingNode,      // VFX member type: SectionResourceMappingNode
  MemberTypeSpecInfo,                 // VFX member type: SectionSpecInfo
  MemberTypeDescriptorRangeValue,     // VFX member type: SectionDescriptorRangeValueItem
  MemberTypePipelineOption,           // VFX member type: SectionPipelineOption
  MemberTypeShaderOption,             // VFX member type: SectionShaderOption
  MemberTypeNggState,                 // VFX member type: SectionNggState
  MemberTypeExtendedRobustness,       // VFX member type: SectionExtendedRobustness
};

// =====================================================================================================================
// Enumerates the property of shader source section
enum ShaderType {
  Glsl,         // GLSL source
  Hlsl,         // HLSL source
  SpirvAsm,     // SPIRV assemble code
  GlslFile,     // GLSL source in extenal file
  HlslFile,     // HLSL source in external file
  SpirvFile,    // SPIRV binary code in external file
  SpirvAsmFile, // SPIRV assemble code in external file
};

// =====================================================================================================================
template <typename T, T> struct GetMemberHelper;
template <typename T, typename R, R(T::*member)> struct GetMemberHelper<R(T::*), member> {
  static void *getMemberPtr(void *obj) {
    T *t = static_cast<T *>(obj);
    return &(t->*member);
  }
};

template <typename T2, typename T, T> struct GetSubStateMemberHelper;
template <typename T2, typename T, typename R, R(T::*member)> struct GetSubStateMemberHelper<T2, R(T::*), member> {
  static void *getMemberPtr(void *obj) {
    T2 *t = static_cast<T2 *>(obj);
    return &(t->getSubStateRef().*member);
  }
};

// =====================================================================================================================
// Initiates a member to address table
#define INIT_MEMBER_NAME_TO_ADDR(T, name, type, _isObject)                                                             \
  tableItem->memberName = STRING(name);                                                                                \
  if (!strncmp(tableItem->memberName, "m_", 2))                                                                        \
    tableItem->memberName += 2;                                                                                        \
  tableItem->getMember = GetMemberHelper<decltype(&T::name), &T::name>::getMemberPtr;                                  \
  tableItem->memberType = type;                                                                                        \
  tableItem->arrayMaxSize = 1;                                                                                         \
  tableItem->isSection = _isObject;                                                                                    \
  ++tableItem;

// =====================================================================================================================
// Initiates a state's member to address table
#define INIT_STATE_MEMBER_NAME_TO_ADDR(T, name, type, _isObject)                                                       \
  tableItem->memberName = STRING(name);                                                                                \
  if (!strncmp(tableItem->memberName, "m_", 2))                                                                        \
    tableItem->memberName += 2;                                                                                        \
  tableItem->getMember = GetSubStateMemberHelper<T, decltype(&SubState::name), &SubState::name>::getMemberPtr;         \
  tableItem->memberType = type;                                                                                        \
  tableItem->arrayMaxSize = 1;                                                                                         \
  tableItem->isSection = _isObject;                                                                                    \
  ++tableItem;

// =====================================================================================================================
// Initiates a state's member to address table with explicit name
#define INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(T, name, member, getter, type, _isObject)                               \
  tableItem->memberName = STRING(name);                                                                                \
  if (!strncmp(tableItem->memberName, "m_", 2))                                                                        \
    tableItem->memberName += 2;                                                                                        \
  tableItem->getMember = getter;                                                                                       \
  tableItem->memberType = type;                                                                                        \
  tableItem->arrayMaxSize = 1;                                                                                         \
  tableItem->isSection = _isObject;                                                                                    \
  ++tableItem;

// =====================================================================================================================
// Initiates a array member to address table
#define INIT_MEMBER_ARRAY_NAME_TO_ADDR(T, name, type, maxSize, _isObject)                                              \
  tableItem->memberName = STRING(name);                                                                                \
  if (!strncmp(tableItem->memberName, "m_", 2))                                                                        \
    tableItem->memberName += 2;                                                                                        \
  tableItem->getMember = GetMemberHelper<decltype(&T::name), &T::name>::getMemberPtr;                                  \
  tableItem->memberType = type;                                                                                        \
  tableItem->arrayMaxSize = maxSize;                                                                                   \
  tableItem->isSection = _isObject;                                                                                    \
  ++tableItem;

// =====================================================================================================================
// Initiates a dynamic array member to address table
#define INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(T, name, type, _isObject)                                                    \
  tableItem->memberName = STRING(name);                                                                                \
  if (!strncmp(tableItem->memberName, "m_", 2))                                                                        \
    tableItem->memberName += 2;                                                                                        \
  tableItem->getMember = GetMemberHelper<decltype(&T::name), &T::name>::getMemberPtr;                                  \
  tableItem->memberType = type;                                                                                        \
  tableItem->arrayMaxSize = VfxDynamicArrayId;                                                                         \
  tableItem->isSection = _isObject;                                                                                    \
  ++tableItem;

// =====================================================================================================================
// Csses a section to sub section
#define CASE_SUBSECTION(ENUM, TYPE)                                                                                    \
  case ENUM: {                                                                                                         \
    TYPE *subSectionObj = nullptr;                                                                                     \
    result = getPtrOf(lineNum, memberName, true, arrayIndex, &subSectionObj, errorMsg);                                \
    *ptrOut = subSectionObj;                                                                                           \
    break;                                                                                                             \
  }

// =====================================================================================================================
// Initiates section info
#define INIT_SECTION_INFO(NAME, type, property)                                                                        \
  {                                                                                                                    \
    SectionInfo sectionInfo = {type, property};                                                                        \
    m_sectionInfo[NAME] = sectionInfo;                                                                                 \
  }

// =====================================================================================================================
// Represents the structure that maps a string to a class member
struct StrToMemberAddr {
  const char *memberName; // String form name
  MemberType memberType;  // Member value type
  void *(*getMember)(void *obj); // Get the member from an object
  unsigned arrayMaxSize;  // If greater than 1, this member is an array
  bool isSection;         // Is this member another Section object
};

// =====================================================================================================================
// Represents the info of section type
struct SectionInfo {
  SectionType type;  // Section type
  unsigned property; // Additional section information
};

// =====================================================================================================================
// Represents an object whose member can be set throught it's string form name.
class Section {
public:
  Section(StrToMemberAddr *addrTable, unsigned tableSize, SectionType type, const char *sectionName);
  virtual ~Section() {}

  static Section *createSection(const char *sectionName);
  static SectionType getSectionType(const char *sectionName);
  static void initSectionInfo();
  static bool readFile(const std::string &docFilename, const std::string &fileName, bool isBinary,
                       std::vector<uint8_t> *binaryData, std::string *textData, std::string *errorMsg);

  virtual bool isShaderSourceSection() { return false; }

  // Adds a new line to section, it is only valid for non-rule based section
  virtual void addLine(const char *line){};

  // Gets section type.
  SectionType getSectionType() const { return m_sectionType; }

  void *getMemberAddr(unsigned i) { return m_memberTable[i].getMember(this); }

  bool getMemberType(unsigned lineNum, const char *memberName, MemberType *valueType, std::string *errorMsg);

  bool getPtrOfSubSection(unsigned lineNum, const char *memberName, MemberType memberType, bool isWriteAccess,
                          unsigned arrayIndex, Section **ptrOut, std::string *errorMsg);

  // Gets ptr of a member.
  template <typename TValue>
  bool getPtrOf(unsigned lineNum, const char *memberName, bool isWriteAccess, unsigned arrayIndex, TValue **ptrOut,
                std::string *errorMsg);

  // Sets value to a member array
  template <typename TValue> bool set(unsigned lineNum, const char *fieldName, unsigned arrayIndex, TValue *value);

  // Sets value to a member
  template <typename TValue> bool set(unsigned lineNum, const char *fieldName, TValue *value) {
    return set(lineNum, fieldName, 0, value);
  };

  bool isSection(unsigned lineNum, const char *memberName, bool *output, MemberType *type, std::string *errorMsg);

  // Has this object been configured in VFX file.
  bool isActive() { return m_isActive; }

  void setActive(bool isActive) { m_isActive = isActive; }

  void printSelf(unsigned level);

  void setLineNum(unsigned lineNum) { m_lineNum = lineNum; }

  unsigned getLineNum() const { return m_lineNum; }

private:
  Section(){};

protected:
  SectionType m_sectionType; // Section type
  const char *m_sectionName; // Section name
  unsigned m_lineNum;        // Line number of this section
private:
  StrToMemberAddr *m_memberTable;                          // Member address table
  unsigned m_tableSize;                                    // Address table size
  bool m_isActive;                                         // If the scestion is active
  static std::map<std::string, SectionInfo> m_sectionInfo; // Section info
};

// =====================================================================================================================
// Gets ptr of a member. return true if operation success
template <typename TValue>
//
// @param lineNum : Line No.
// @param memberName : Member name
// @param isWriteAccess : True for write
// @param arrayIndex : Array index
// @param [out] ptrOut : Pointer of section member
// @param [out] errorMsg : Error message
bool Section::getPtrOf(unsigned lineNum, const char *memberName, bool isWriteAccess, unsigned arrayIndex,
                       TValue **ptrOut, std::string *errorMsg) {
  bool result = true;
  void *memberAddr = reinterpret_cast<void *>(static_cast<size_t>(VfxInvalidValue));
  unsigned arrayMaxSize = 0;

  if (isWriteAccess)
    setActive(true);
  // Search section member
  for (unsigned i = 0; i < m_tableSize; ++i) {
    if (strcmp(memberName, m_memberTable[i].memberName) == 0) {
      memberAddr = getMemberAddr(i);
      if (arrayIndex >= m_memberTable[i].arrayMaxSize) {
        PARSE_ERROR(*errorMsg, lineNum, "Array access out of bound: %u of %s[%u]", arrayIndex, memberName,
                    m_memberTable[i].arrayMaxSize);
        result = false;
      }
      arrayMaxSize = m_memberTable[i].arrayMaxSize;
      break;
    }
  }

  if (result && memberAddr == reinterpret_cast<void *>(static_cast<size_t>(VfxInvalidValue))) {
    PARSE_WARNING(*errorMsg, lineNum, "Invalid member name: %s", memberName);
    result = false;
  }

  if (result) {
    VFX_ASSERT(ptrOut);
    if (arrayMaxSize == VfxDynamicArrayId) {
      // Member is dynamic array, cast to std::vector
      std::vector<TValue> *memberVector = reinterpret_cast<std::vector<TValue> *>(memberAddr);
      if (memberVector->size() <= arrayIndex)
        memberVector->resize(arrayIndex + 1);
      *ptrOut = &((*memberVector)[arrayIndex]);
    } else {
      TValue *member = reinterpret_cast<TValue *>(memberAddr);
      *ptrOut = member + arrayIndex;
    }
  }

  return result;
};

// =====================================================================================================================
// Sets value to a member array
template <typename TValue>
//
// @param lineNum : Line No.
// @param memberName : Name of section member
// @param arrayIndex : Array index
// @param value : Value to be set
bool Section::set(unsigned lineNum, const char *memberName, unsigned arrayIndex, TValue *value) {
  bool result = false;
  VFX_ASSERT(value);
  TValue *memberPtr = nullptr;
  std::string dummyMsg;
  result = getPtrOf(lineNum, memberName, true, arrayIndex, &memberPtr, &dummyMsg);
  VFX_ASSERT(result == true);
  if (result)
    *memberPtr = *value;

  return result;
};

// =====================================================================================================================
// Represents the document version.
class SectionVersion : public Section {
public:
  SectionVersion() : Section(m_addrTable, MemberCount, SectionTypeVersion, nullptr) { m_version = 0; };

  // Setup member name to member mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_NAME_TO_ADDR(SectionVersion, m_version, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(unsigned &state) { state = m_version; };

private:
  static const unsigned MemberCount = 1;
  static StrToMemberAddr m_addrTable[MemberCount];

  unsigned m_version; // Document version
};
// =====================================================================================================================
// Represents the class that includes data to verify a test result.
class SectionResultItem : public Section {
public:
  typedef ResultItem SubState;

  SectionResultItem() : Section(m_addrTable, MemberCount, SectionTypeUnset, "ResultItem") {
    memset(&m_state, 0, sizeof(m_state));
    m_state.resultSource = ResultSourceMaxEnum;
    m_state.compareMethod = ResultCompareMethodEqual;
  };

  // Setup member name to member mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, resultSource, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, bufferBinding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, offset, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, iVec4Value, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, i64Vec2Value, MemberTypeI64Vec2, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, fVec4Value, MemberTypeFVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, f16Vec4Value, MemberTypeF16Vec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, dVec2Value, MemberTypeDVec2, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, compareMethod, MemberTypeEnum, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 9;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent the result section
class SectionResult : public Section {
public:
  typedef TestResult SubState;

  SectionResult() : Section(m_addrTable, MemberCount, SectionTypeResult, nullptr){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionResult, m_result, MemberTypeResultItem, MaxResultCount, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state.numResult = 0;
    for (unsigned i = 0; i < MaxResultCount; ++i) {
      if (m_result[i].isActive())
        m_result[i].getSubState(state.result[state.numResult++]);
    }
  }

private:
  static const unsigned MemberCount = 1;
  static StrToMemberAddr m_addrTable[MemberCount];

  SectionResultItem m_result[MaxResultCount]; // section result items
};

// =====================================================================================================================
// Represents the class that includes data to represent one spec constant
class SectionSpecConstItem : public Section {
public:
  typedef SpecConstItem SubState;

  SectionSpecConstItem() : Section(m_addrTable, MemberCount, SectionTypeUnset, "specConst"){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, i, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, f, MemberTypeFVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, d, MemberTypeDVec2, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent spec constants in on shader stage
class SectionSpecConst : public Section {
public:
  typedef SpecConst SubState;

  SectionSpecConst(const char *name = nullptr) : Section(m_addrTable, MemberCount, SectionTypeUnset, name){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionSpecConst, m_specConst, MemberTypeSpecConstItem, MaxSpecConstantCount, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state.numSpecConst = 0;
    for (unsigned i = 0; i < MaxResultCount; ++i) {
      if (m_specConst[i].isActive())
        m_specConst[i].getSubState(state.specConst[state.numSpecConst++]);
    }
  }

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SectionSpecConstItem m_specConst[MaxSpecConstantCount]; // Spec constant for one shader stage
};

// =====================================================================================================================
// Represents the class of vertex buffer binding.
class SectionVertexBufferBinding : public Section {
public:
  typedef VertrexBufferBinding SubState;

  SectionVertexBufferBinding() : Section(m_addrTable, MemberCount, SectionTypeUnset, "VertexBufferBinding") {
    m_state.binding = VfxInvalidValue;
    m_state.strideInBytes = VfxInvalidValue;
    m_state.stepRate = VK_VERTEX_INPUT_RATE_VERTEX;
  };

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, strideInBytes, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, stepRate, MemberTypeEnum, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class of vertex attribute.
class SectionVertexAttribute : public Section {
public:
  typedef VertexAttribute SubState;

  SectionVertexAttribute() : Section(m_addrTable, MemberCount, SectionTypeUnset, "VertexAttribute") {
    m_state.binding = VfxInvalidValue;
    m_state.format = VK_FORMAT_UNDEFINED;
    m_state.location = VfxInvalidValue;
    m_state.offsetInBytes = VfxInvalidValue;
  };

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, format, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, location, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, offsetInBytes, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 4;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class of vertex state
class SectionVertexState : public Section {
public:
  typedef VertexState SubState;

  SectionVertexState() : Section(m_addrTable, MemberCount, SectionTypeVertexState, nullptr){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionVertexState, m_vbBinding, MemberTypeVertexBufferBindingItem,
                                   MaxVertexBufferBindingCount, true);
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionVertexState, m_attribute, MemberTypeVertexAttributeItem,
                                   MaxVertexAttributeCount, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state.numVbBinding = 0;
    for (unsigned i = 0; i < MaxVertexBufferBindingCount; ++i) {
      if (m_vbBinding[i].isActive())
        m_vbBinding[i].getSubState(state.vbBinding[state.numVbBinding++]);
    }

    state.numAttribute = 0;
    for (unsigned i = 0; i < MaxVertexAttributeCount; ++i) {
      if (m_attribute[i].isActive())
        m_attribute[i].getSubState(state.attribute[state.numAttribute++]);
    }
  }

private:
  static const unsigned MemberCount = 2;
  static StrToMemberAddr m_addrTable[MemberCount];
  SectionVertexBufferBinding m_vbBinding[MaxVertexBufferBindingCount]; // Binding info of all vertex buffers
  SectionVertexAttribute m_attribute[MaxVertexAttributeCount];         // Attribute info of all vertex attributes
};

// =====================================================================================================================
// Represents the class that includes data needed to create buffer and buffer view object.
class SectionBufferView : public Section {
public:
  typedef BufferView SubState;

  SectionBufferView()
      : Section(m_addrTable, MemberCount, SectionTypeBufferView, nullptr), m_intData(&m_bufMem), m_uintData(&m_bufMem),
        m_int64Data(&m_bufMem), m_uint64Data(&m_bufMem), m_floatData(&m_bufMem), m_doubleData(&m_bufMem),
        m_float16Data(&m_bufMem) {
    memset(&m_state, 0, sizeof(m_state));
    m_state.size = VfxInvalidValue;
    m_state.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  };

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, binding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, descriptorType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, size, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, format, MemberTypeEnum, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_intData, MemberTypeIArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_int64Data, MemberTypeI64Array, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_uint64Data, MemberTypeU64Array, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_floatData, MemberTypeFArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_doubleData, MemberTypeDArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionBufferView, m_float16Data, MemberTypeF16Array, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_state.dataSize = static_cast<unsigned>(m_bufMem.size());
    m_state.pData = m_state.dataSize > 0 ? &m_bufMem[0] : nullptr;
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 11;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;                    // State of BufferView
  std::vector<uint8_t> m_bufMem;       // Underlying buffer for all data types.
  std::vector<uint8_t> *m_intData;     // Contains int data of this buffer
  std::vector<uint8_t> *m_uintData;    // Contains uint data of this buffer
  std::vector<uint8_t> *m_int64Data;   // Contains int64 data of this buffer
  std::vector<uint8_t> *m_uint64Data;  // Contains uint64 data of this buffer
  std::vector<uint8_t> *m_floatData;   // Contains float data of this buffer
  std::vector<uint8_t> *m_doubleData;  // Contains double data of this buffer
  std::vector<uint8_t> *m_float16Data; // Contains float16 data of this buffer
};

// =====================================================================================================================
// Represents the class of image/image view object
class SectionImageView : public Section {
public:
  typedef ImageView SubState;

  SectionImageView() : Section(m_addrTable, MemberCount, SectionTypeImageView, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
    m_state.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    m_state.viewType = VK_IMAGE_VIEW_TYPE_2D;
    m_state.dataPattern = ImageCheckBoxUnorm;
    m_state.samples = 1;
    m_state.mipmap = 0;
  }

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, binding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, descriptorType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, size, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, viewType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, dataPattern, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, samples, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, mipmap, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 7;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
};

// =====================================================================================================================
// Represents the class of sampler object
class SectionSampler : public Section {
public:
  typedef Sampler SubState;

  SectionSampler()
      : Section(m_addrTable, MemberCount, SectionTypeSampler, nullptr)

  {
    memset(&m_state, 0, sizeof(m_state));
    m_state.descriptorType = static_cast<VkDescriptorType>(VfxInvalidValue);
    m_state.dataPattern = static_cast<SamplerPattern>(VfxInvalidValue);
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, binding, MemberTypeBinding, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, descriptorType, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, dataPattern, MemberTypeEnum, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent one push constant range
class SectionPushConstRange : public Section {
public:
  typedef PushConstRange SubState;

  SectionPushConstRange()
      : Section(m_addrTable, MemberCount, SectionTypeUnset, "pushConstRange"), m_intData(&m_bufMem),
        m_uintData(&m_bufMem), m_floatData(&m_bufMem), m_doubleData(&m_bufMem){};

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPushConstRange, start, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPushConstRange, length, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_intData, MemberTypeIArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_floatData, MemberTypeFArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange, m_doubleData, MemberTypeDArray, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_state.dataSize = static_cast<unsigned>(m_bufMem.size()) * sizeof(unsigned);
    m_state.pData = state.dataSize > 0 ? &m_bufMem[0] : nullptr;
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 6;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<unsigned> m_bufMem;      // Underlying buffer for all data types.
  std::vector<unsigned> *m_intData;    // Contains int data of this push constant range
  std::vector<unsigned> *m_uintData;   // Contains uint data of this push constant range
  std::vector<unsigned> *m_floatData;  // Contains float data of this push constant range
  std::vector<unsigned> *m_doubleData; // Contains double data of this push constant range
  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data needed to create global draw state
class SectionDrawState : public Section {
public:
  typedef DrawState SubState;

  SectionDrawState()
      : Section(m_addrTable, MemberCount, SectionTypeDrawState, nullptr), m_vs("vs"), m_tcs("tcs"), m_tes("tes"),
        m_gs("gs"), m_fs("fs"), m_cs("cs") {
    initDrawState(m_state);
  }

  // Set initial value for DrawState.
  static void initDrawState(SubState &state) {
    state.dispatch.iVec4[0] = 1;
    state.dispatch.iVec4[1] = 1;
    state.dispatch.iVec4[2] = 1;
    state.viewport.iVec4[0] = 0;
    state.viewport.iVec4[1] = 0;
    state.viewport.iVec4[2] = 0;
    state.viewport.iVec4[3] = 0;
    state.instance = 1;
    state.vertex = 4;
    state.firstInstance = 0;
    state.firstVertex = 0;
    state.index = 6;
    state.firstIndex = 0;
    state.vertexOffset = 0;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    state.polygonMode = VK_POLYGON_MODE_FILL;
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.depthBiasEnable = false;
    state.width = 0;
    state.height = 0;
    state.lineWidth = 1.0f;
  }
  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, instance, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, vertex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstInstance, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstVertex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, index, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstIndex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, vertexOffset, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, topology, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, polygonMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, cullMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, frontFace, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, depthBiasEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, patchControlPoints, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, dispatch, MemberTypeIVec4, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, width, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, height, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, lineWidth, MemberTypeFloat, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, viewport, MemberTypeIVec4, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_vs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_tcs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_tes, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_gs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_fs, MemberTypeSpecConst, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, m_cs, MemberTypeSpecConst, true);
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionDrawState, m_pushConstRange, MemberTypePushConstRange, MaxPushConstRangCount,
                                   true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    state = m_state;
    m_vs.getSubState(state.vs);
    m_tcs.getSubState(state.tcs);
    m_tes.getSubState(state.tes);
    m_gs.getSubState(state.gs);
    m_fs.getSubState(state.fs);
    m_cs.getSubState(state.cs);
    state.numPushConstRange = 0;
    for (unsigned i = 0; i < MaxPushConstRangCount; ++i) {
      if (m_pushConstRange[i].isActive())
        m_pushConstRange[i].getSubState(state.pushConstRange[state.numPushConstRange++]);
    }
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 25;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  SectionSpecConst m_vs;                                         // Vertex shader's spec constant
  SectionSpecConst m_tcs;                                        // Tessellation control shader's spec constant
  SectionSpecConst m_tes;                                        // Tessellation evaluation shader's spec constant
  SectionSpecConst m_gs;                                         // Geometry shader's spec constant
  SectionSpecConst m_fs;                                         // Fragment shader's spec constant
  SectionSpecConst m_cs;                                         // Compute shader shader's spec constant
  SectionPushConstRange m_pushConstRange[MaxPushConstRangCount]; // Pipeline push constant ranges
};

// =====================================================================================================================
// Represents the class that includes all kinds of shader source
class SectionShader : public Section {
public:
  typedef ShaderSource SubState;
  SectionShader(const SectionInfo &info)
      : Section(m_addrTable, MemberCount, info.type, nullptr), m_shaderType(static_cast<ShaderType>(info.property)) {}

  // Setup member name to member address mapping.
  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_NAME_TO_ADDR(SectionShader, m_fileName, MemberTypeString, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  virtual bool isShaderSourceSection();

  virtual void addLine(const char *line) { m_shaderSource += line; };

  bool compileShader(const std::string &docFilename, const Section *shaderInfo, std::string *errorMsg);

  void getSubState(SubState &state);

private:
  bool compileGlsl(const Section *shaderInfo, std::string *errorMsg);
  bool assembleSpirv(std::string *errorMsg);

  static const unsigned MemberCount = 1;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::string m_fileName;        // External shader source file name
  std::string m_shaderSource;    // Shader source code
  ShaderType m_shaderType;       // Shader type
  std::vector<uint8_t> m_spvBin; // SPIRV shader binary
};

// =====================================================================================================================
// Represents the class that includes all kinds of compile log, This section is ignored in Document::GetDocument
class SectionCompileLog : public Section {
public:
  SectionCompileLog() : Section(m_addrTable, MemberCount, SectionTypeCompileLog, nullptr) {}

  // Setup member name to member address mapping.
  static void initialAddrTable() {}

  virtual void addLine(const char *line) { m_compileLog += line; };

private:
  static const unsigned MemberCount = 1;
  static StrToMemberAddr m_addrTable[MemberCount];
  std::string m_compileLog; // Compile Log
};

// =====================================================================================================================
// Represents the sub section color target
class SectionColorBuffer : public Section {
public:
  typedef ColorBuffer SubState;

  SectionColorBuffer() : Section(m_addrTable, MemberCount, SectionTypeUnset, "colorBuffer") {
    memset(&m_state, 0, sizeof(m_state));
    m_state.channelWriteMask = 0xF;
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, format, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, blendEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, blendSrcAlphaToColor, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, channelWriteMask, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 4;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section ExtendedRobustness
class SectionExtendedRobustness : public Section {
public:
  typedef Vkgc::ExtendedRobustness SubState;

  SectionExtendedRobustness() : Section(m_addrTable, MemberCount, SectionTypeUnset, "extendedRobustness") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, robustBufferAccess, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, robustImageAccess, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionExtendedRobustness, nullDescriptor, MemberTypeBool, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section pipeline option
class SectionPipelineOption : public Section {
public:
  typedef Vkgc::PipelineOptions SubState;

  SectionPipelineOption() : Section(m_addrTable, MemberCount, SectionTypeUnset, "options") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, includeDisassembly, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, scalarBlockLayout, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, includeIr, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, robustBufferAccess, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, reconfigWorkgroupLayout, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTableUsage, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPipelineOption, shadowDescriptorTablePtrHigh, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionPipelineOption, m_extendedRobustness, MemberTypeExtendedRobustness, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_extendedRobustness.getSubState(m_state.extendedRobustness);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 8;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
  SectionExtendedRobustness m_extendedRobustness;
};

// =====================================================================================================================
// Represents the sub section shader option
class SectionShaderOption : public Section {
public:
  typedef Vkgc::PipelineShaderOptions SubState;

  SectionShaderOption() : Section(m_addrTable, MemberCount, SectionTypeUnset, "options") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, trapPresent, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, debugMode, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, enablePerformanceData, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, allowReZ, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, vgprLimit, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, sgprLimit, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, maxThreadGroupsPerComputeUnit, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, waveSize, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, wgpMode, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, waveBreakSize, MemberTypeEnum, false);

    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, forceLoopUnrollCount, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, useSiScheduler, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, updateDescInElf, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, allowVaryWaveSize, MemberTypeBool, false);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, enableLoadScalarizer, MemberTypeBool, false);
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 35
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableLicm, MemberTypeBool, false);
#endif
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, unrollThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, scalarThreshold, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderOption, disableLoopUnroll, MemberTypeBool, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 19;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section NGG state
class SectionNggState : public Section {
public:
  typedef Vkgc::NggState SubState;

  SectionNggState() : Section(m_addrTable, MemberCount, SectionTypeUnset, "nggState") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableNgg, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableGsUse, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, forceNonPassthrough, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, alwaysUsePrimShaderTable, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, compactMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableFastLaunch, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableVertexReuse, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableBackfaceCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableFrustumCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableBoxFilterCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableSphereCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableSmallPrimFilter, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, enableCullDistanceCulling, MemberTypeBool, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, backfaceExponent, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, subgroupSizing, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, primsPerSubgroup, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionNggState, vertsPerSubgroup, MemberTypeInt, false);

    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 17;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the section graphics state
class SectionGraphicsState : public Section {
public:
  typedef GraphicsPipelineState SubState;

  SectionGraphicsState() : Section(m_addrTable, MemberCount, SectionTypeGraphicsState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, topology, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, polygonMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, cullMode, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, frontFace, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, depthBiasEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, patchControlPoints, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, deviceIndex, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, disableVertexReuse, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, depthClipEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rasterizerDiscardEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, perSampleShading, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, numSamples, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, samplePatternIdx, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, usrClipPlaneMask, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, alphaToCoverageEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, dualSourceBlendEnable, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, switchWinding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableMultiView, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_options, MemberTypePipelineOption, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionGraphicsState, m_nggState, MemberTypeNggState, true);
    INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState, m_colorBuffer, MemberTypeColorBufferItem,
                                   Vkgc::MaxColorTargets, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    for (unsigned i = 0; i < Vkgc::MaxColorTargets; ++i)
      m_colorBuffer[i].getSubState(m_state.colorBuffer[i]);
    m_options.getSubState(m_state.options);
    m_nggState.getSubState(m_state.nggState);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  SectionNggState m_nggState;
  static const unsigned MemberCount = 24;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  SectionColorBuffer m_colorBuffer[Vkgc::MaxColorTargets]; // Color buffer
  SectionPipelineOption m_options;
};

// =====================================================================================================================
// Represents the section compute state
class SectionComputeState : public Section {
public:
  typedef ComputePipelineState SubState;

  SectionComputeState() : Section(m_addrTable, MemberCount, SectionTypeComputeState, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, deviceIndex, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionComputeState, m_options, MemberTypePipelineOption, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(const std::string &docFilename, SubState &state, std::string *errorMsg) {
    m_options.getSubState(m_state.options);
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 5;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
  SectionPipelineOption m_options;
};

// =====================================================================================================================
// Represents the sub section vertex input binding
class SectionVertexInputBinding : public Section {
public:
  typedef VkVertexInputBindingDescription SubState;

  SectionVertexInputBinding() : Section(m_addrTable, MemberCount, SectionTypeUnset, "binding") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, stride, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, inputRate, MemberTypeEnum, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input attribute
class SectionVertexInputAttribute : public Section {
public:
  typedef VkVertexInputAttributeDescription SubState;

  SectionVertexInputAttribute() : Section(m_addrTable, MemberCount, SectionTypeUnset, "binding") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, location, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, format, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, offset, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 4;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input divisor
class SectionVertexInputDivisor : public Section {
public:
  typedef VkVertexInputBindingDivisorDescriptionEXT SubState;

  SectionVertexInputDivisor() : Section(m_addrTable, MemberCount, SectionTypeUnset, "divisor") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputDivisor, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputDivisor, divisor, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 2;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the section vertex input
class SectionVertexInput : public Section {
public:
  typedef VkPipelineVertexInputStateCreateInfo SubState;

  SectionVertexInput() : Section(m_addrTable, MemberCount, SectionTypeVertexInputState, nullptr) {
    memset(&m_vkDivisorState, 0, sizeof(m_vkDivisorState));
    m_vkDivisorState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, m_attribute, MemberTypeVertexInputAttributeItem, true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, m_binding, MemberTypeVertexInputBindingItem, true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, m_divisor, MemberTypeVertexInputDivisorItem, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    m_vkBindings.resize(m_binding.size());
    m_vkAttributes.resize(m_attribute.size());
    m_vkDivisors.resize(m_divisor.size());

    for (unsigned i = 0; i < m_attribute.size(); ++i)
      m_attribute[i].getSubState(m_vkAttributes[i]);

    for (unsigned i = 0; i < m_binding.size(); ++i)
      m_binding[i].getSubState(m_vkBindings[i]);

    for (unsigned i = 0; i < m_divisor.size(); ++i)
      m_divisor[i].getSubState(m_vkDivisors[i]);

    state.vertexAttributeDescriptionCount = static_cast<unsigned>(m_vkAttributes.size());
    state.vertexBindingDescriptionCount = static_cast<unsigned>(m_vkBindings.size());
    state.pVertexBindingDescriptions = state.vertexBindingDescriptionCount ? &m_vkBindings[0] : nullptr;
    state.pVertexAttributeDescriptions = state.vertexAttributeDescriptionCount ? &m_vkAttributes[0] : nullptr;
    if (m_vkDivisors.size() > 0) {
      state.pNext = &m_vkDivisorState;
      m_vkDivisorState.vertexBindingDivisorCount = static_cast<unsigned>(m_vkDivisors.size());
      m_vkDivisorState.pVertexBindingDivisors = &m_vkDivisors[0];
    }
  };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<SectionVertexInputAttribute> m_attribute;                // Vertex input atribute
  std::vector<SectionVertexInputBinding> m_binding;                    // Vertex input binding
  std::vector<SectionVertexInputDivisor> m_divisor;                    // Vertex input divisor
  std::vector<VkVertexInputBindingDescription> m_vkBindings;           // Vulkan input binding
  std::vector<VkVertexInputAttributeDescription> m_vkAttributes;       // Vulkan vertex input atribute
  std::vector<VkVertexInputBindingDivisorDescriptionEXT> m_vkDivisors; // Vulkan vertex input divisor
  VkPipelineVertexInputDivisorStateCreateInfoEXT m_vkDivisorState;     // Vulkan vertex input divisor state
};

// =====================================================================================================================
// Represents the sub section specialization constant map entry
class SectionSpecEntryItem : public Section {
public:
  typedef VkSpecializationMapEntry SubState;

  SectionSpecEntryItem() : Section(m_addrTable, MemberCount, SectionTypeUnset, "mapEntry") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, constantID, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, offset, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, size, MemberTypeInt, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 3;
  static StrToMemberAddr m_addrTable[MemberCount];

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section specialization constant info
class SectionSpecInfo : public Section {
public:
  typedef VkSpecializationInfo SubState;

  SectionSpecInfo() : Section(m_addrTable, MemberCount, SectionTypeUnset, "specConst") {
    m_intData = &m_bufMem;
    m_uintData = &m_bufMem;
    m_int64Data = &m_bufMem;
    m_uint64Data = &m_bufMem;
    m_floatData = &m_bufMem;
    m_doubleData = &m_bufMem;
    m_float16Data = &m_bufMem;
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionSpecInfo, m_mapEntry, MemberTypeSpecEntryItem, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_intData, MemberTypeIArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_int64Data, MemberTypeI64Array, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_uint64Data, MemberTypeU64Array, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_floatData, MemberTypeFArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_doubleData, MemberTypeDArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_float16Data, MemberTypeF16Array, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    if (m_mapEntry.size()) {
      state.mapEntryCount = static_cast<unsigned>(m_mapEntry.size());
      m_vkMapEntries.resize(state.mapEntryCount);
      for (unsigned i = 0; i < m_vkMapEntries.size(); ++i)
        m_mapEntry[i].getSubState(m_vkMapEntries[i]);
      state.pMapEntries = &m_vkMapEntries[0];
      state.dataSize = m_bufMem.size();
      state.pData = &m_bufMem[0];
    } else
      memset(&state, 0, sizeof(SubState));
  }

private:
  static const unsigned MemberCount = 8;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<SectionSpecEntryItem> m_mapEntry;
  std::vector<uint8_t> *m_intData;     // Contains int data of this buffer
  std::vector<uint8_t> *m_uintData;    // Contains uint data of this buffer
  std::vector<uint8_t> *m_int64Data;   // Contains int64 data of this buffer
  std::vector<uint8_t> *m_uint64Data;  // Contains uint64 long data of this buffer
  std::vector<uint8_t> *m_floatData;   // Contains float data of this buffer
  std::vector<uint8_t> *m_doubleData;  // Contains double data of this buffer
  std::vector<uint8_t> *m_float16Data; // Contains float16 data of this buffer

  std::vector<uint8_t> m_bufMem;                        // Buffer memory
  std::vector<VkSpecializationMapEntry> m_vkMapEntries; // Vulkan specialization map entry
};

// =====================================================================================================================
// Represents the sub section descriptor range value
class SectionDescriptorRangeValueItem : public Section {
public:
  typedef Vkgc::DescriptorRangeValue SubState;

  SectionDescriptorRangeValueItem() : Section(m_addrTable, MemberCount, SectionTypeUnset, "descriptorRangeValue") {
    m_intData = &m_bufMem;
    m_uintData = &m_bufMem;
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, type, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, set, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, binding, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, arraySize, MemberTypeInt, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, m_uintData, MemberTypeUArray, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, m_intData, MemberTypeIArray, false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }
  void getSubState(SubState &state) {
    state = m_state;
    state.pValue = m_bufMem.size() > 0 ? (const unsigned *)(&m_bufMem[0]) : nullptr;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static const unsigned MemberCount = 6;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<uint8_t> *m_intData;
  std::vector<uint8_t> *m_uintData;
  SubState m_state;
  std::vector<uint8_t> m_bufMem;
};

// =====================================================================================================================
// Represents the sub section resource mapping node
class SectionResourceMappingNode : public Section {
public:
  typedef Vkgc::ResourceMappingNode SubState;
  SectionResourceMappingNode() : Section(m_addrTable, MemberCount, SectionTypeUnset, "userDataNode") {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, type, MemberTypeEnum, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, sizeInDwords, MemberTypeInt, false);
    INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, offsetInDwords, MemberTypeInt, false);
    INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, set, srdRange.set,
                                           SectionResourceMappingNode::getResourceMapNodeSet, MemberTypeInt, false);
    INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, binding, srdRange.binding,
                                           SectionResourceMappingNode::getResourceMapNodeBinding, MemberTypeInt, false);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMappingNode, m_next, MemberTypeResourceMappingNode, true);
    INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode, indirectUserDataCount, userDataPtr.sizeInDwords,
                                           SectionResourceMappingNode::getResourceMapNodeUserDataCount, MemberTypeInt,
                                           false);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    if (m_state.type == Vkgc::ResourceMappingNodeType::DescriptorTableVaPtr) {
      m_nextNodeBuf.resize(m_next.size());
      for (unsigned i = 0; i < m_next.size(); ++i)
        m_next[i].getSubState(m_nextNodeBuf[i]);
      m_state.tablePtr.pNext = &m_nextNodeBuf[0];
      m_state.tablePtr.nodeCount = static_cast<unsigned>(m_nextNodeBuf.size());
    }
    state = m_state;
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static void *getResourceMapNodeSet(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.set);
  }

  static void *getResourceMapNodeBinding(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.srdRange.binding);
  }

  static void *getResourceMapNodeUserDataCount(void *obj) {
    SectionResourceMappingNode *castedObj = static_cast<SectionResourceMappingNode *>(obj);
    return static_cast<void *>(&castedObj->m_state.userDataPtr.sizeInDwords);
  }

  static const unsigned MemberCount = 7;
  static StrToMemberAddr m_addrTable[MemberCount];

  std::vector<SectionResourceMappingNode> m_next; // Next rsource mapping node
  SubState m_state;
  std::vector<SubState> m_nextNodeBuf; // Contains next nodes
};

// =====================================================================================================================
// Represents the sub section pipeline shader info
class SectionShaderInfo : public Section {
public:
  typedef Vkgc::PipelineShaderInfo SubState;
  SectionShaderInfo(SectionType sectionType) : Section(m_addrTable, MemberCount, sectionType, nullptr) {
    memset(&m_state, 0, sizeof(m_state));
  }

  static void initialAddrTable() {
    StrToMemberAddr *tableItem = m_addrTable;
    INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_entryPoint, MemberTypeString, false);
    INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_specConst, MemberTypeSpecInfo, true);
    INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, m_options, MemberTypeShaderOption, true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, m_descriptorRangeValue, MemberTypeDescriptorRangeValue, true);
    INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, m_userDataNode, MemberTypeResourceMappingNode, true);
    VFX_ASSERT(tableItem - &m_addrTable[0] <= MemberCount);
  }

  void getSubState(SubState &state) {
    memset(&state, 0, sizeof(SubState));
    state.pEntryTarget = m_entryPoint.c_str();
    memcpy(&state.options, &m_state.options, sizeof(m_state.options));

    m_specConst.getSubState(m_specializationInfo);
    state.pSpecializationInfo = &m_specializationInfo;

    m_options.getSubState(state.options);

    if (m_descriptorRangeValue.size() > 0) {
      m_descriptorRangeValues.resize(m_descriptorRangeValue.size());
      for (unsigned i = 0; i < m_descriptorRangeValue.size(); ++i)
        m_descriptorRangeValue[i].getSubState(m_descriptorRangeValues[i]);
      state.descriptorRangeValueCount = static_cast<unsigned>(m_descriptorRangeValue.size());
      state.pDescriptorRangeValues = &m_descriptorRangeValues[0];
    }

    if (m_userDataNode.size() > 0) {
      state.userDataNodeCount = static_cast<unsigned>(m_userDataNode.size());
      m_userDataNodes.resize(state.userDataNodeCount);
      for (unsigned i = 0; i < state.userDataNodeCount; ++i)
        m_userDataNode[i].getSubState(m_userDataNodes[i]);
      state.pUserDataNodes = &m_userDataNodes[0];
    }
  };
  SubState &getSubStateRef() { return m_state; };

  const char *getEntryPoint() const { return m_entryPoint.empty() ? nullptr : m_entryPoint.c_str(); }

private:
  static const unsigned MemberCount = 5;
  static StrToMemberAddr m_addrTable[MemberCount];
  SubState m_state;
  SectionSpecInfo m_specConst;                                         // Specialization constant info
  SectionShaderOption m_options;                                       // Pipeline shader options
  std::string m_entryPoint;                                            // Entry point name
  std::vector<SectionDescriptorRangeValueItem> m_descriptorRangeValue; // Contains descriptor range vuale
  std::vector<SectionResourceMappingNode> m_userDataNode;              // Contains user data node

  VkSpecializationInfo m_specializationInfo;
  std::vector<Vkgc::DescriptorRangeValue> m_descriptorRangeValues;
  std::vector<Vkgc::ResourceMappingNode> m_userDataNodes;
};

} // namespace Vfx
