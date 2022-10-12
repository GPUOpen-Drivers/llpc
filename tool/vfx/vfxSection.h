/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

class Document;
// =====================================================================================================================
// Enumerates VFX section type.
enum SectionType : unsigned {
  // Common sections
  SectionTypeUnset = 0,  // Initial state, not entering any section.
  SectionTypeVersion,    // Version section
  SectionTypeCompileLog, // Compile log section
  SectionTypeShader,     // Shader source section
  // Render document sections
  SectionTypeResult,      // Result section
  SectionTypeBufferView,  // Buffer view section
  SectionTypeVertexState, // Vertex state section
  SectionTypeDrawState,   // Draw state section
  SectionTypeImageView,   // Image view section
  SectionTypeSampler,     // Sampler section
  // VKGC pipeline
  SectionTypeGraphicsState, // Graphics state section
  SectionTypeComputeState,  // Compute state section
#if VKI_RAY_TRACING
  SectionTypeRayTracingState, // Ray tracing state section
  SectionTypeRtState,         // Ray tracing rtState section
#endif
  SectionTypeVertexInputState, // Vertex input state section
  SectionTypeShaderInfo,       // Shader info section
  SectionTypeResourceMapping,  // Resource mapping section
  // GL pipeline
  SectionTypeGlProgramParameter, // GL program parameter section
  SectionTypeGlGraphicsState,    // GL graphic pipeline state section
  SectionTypeGlComputeState,     // GL compute pipeline state section
  SectionTypeGlTransformState,   // GL transform pipeline state section
  SectionTypeGlFfxPs,            // GL FFX PS state section
  SectionTypeGlFfxVs,            // GL FFX VS state section
  SectionTypeNameNum,            // Name num section
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
#if VKI_RAY_TRACING
  MemberTypeShaderGroup,                  // VFX member type: SectionShaderGroup
  MemberTypeRtState,                      // VFX member type: SectionRtState
  MemberTypeRayTracingShaderExportConfig, // VFX member type: SectionRayTracingShaderExportConfig
  MemberTypeIndirectCalleeSavedRegs,      // VFX member type: SectionIndirectCalleeSavedRegs
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION >= 15
  MemberTypeGpurtFuncTable, // VFX member type: SectionGpurtFuncTable
#endif
#endif
  MemberTypeExtendedRobustness,      // VFX member type: SectionExtendedRobustness
  MemberTypeGlAttribLocation,        // GL vertex attribute location
  MemberTypeGlShaderInfo,            // GL SPIRV parameters
  MemberTypeGlVertexAttrib,          // GL vertex input attribute
  MemberTypeGlVertexBinding,         // GL vertex input binding
  MemberTypeGlVertexFormat,          // GL vertex attribute format
  MemberTypeGlSpirvPipelineLayout,   // GL SPIRV explicit pipeline layout
  MemberTypeGlPatchParameter,        // GL program patch parameter
  MemberTypeGlSpeicalizeUniformDesc, // GL program specialized uniform
  MemberTypeGlFfxTexturekey,         // GL FFX texture key
};

// =====================================================================================================================
// Enumerates the property of shader source section
enum ShaderType {
  Glsl,         // GLSL source
  Hlsl,         // HLSL source
  SpirvAsm,     // SPIRV assemble code
  GlslFile,     // GLSL source in external file
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
// Represents the info of section type
struct SectionInfo {
  SectionType type; // Section type
  union {
    unsigned property; // Additional section information
    struct {
      unsigned short propertyLo;
      unsigned short propertyHi;
    };
  };
};

// =====================================================================================================================
inline SectionInfo initSectionItemInfo(SectionType type, unsigned property) {
  SectionInfo sectionInfo = {};
  sectionInfo.type = type;
  sectionInfo.property = property;
  return sectionInfo;
}

// =====================================================================================================================
inline SectionInfo initSectionItemInfo(SectionType type, uint16_t propertyLo, uint16_t propertyHi) {
  SectionInfo sectionInfo = {};
  sectionInfo.type = type;
  sectionInfo.propertyLo = propertyLo;
  sectionInfo.propertyHi = propertyHi;
  return sectionInfo;
}

// =====================================================================================================================
// Initiates a member to address table
#define INIT_MEMBER_NAME_TO_ADDR(T, name, type, _isObject)                                                             \
  do {                                                                                                                 \
    addrTableInitializer.push_back(StrToMemberAddr());                                                                 \
    StrToMemberAddr &tableItem = addrTableInitializer.back();                                                          \
    tableItem.memberName = STRING(name);                                                                               \
    if (!strncmp(tableItem.memberName, "m_", 2))                                                                       \
      tableItem.memberName += 2;                                                                                       \
    tableItem.getMember = GetMemberHelper<decltype(&T::name), &T::name>::getMemberPtr;                                 \
    tableItem.memberType = type;                                                                                       \
    tableItem.arrayMaxSize = 1;                                                                                        \
    tableItem.isSection = _isObject;                                                                                   \
  } while (false)

// =====================================================================================================================
// Initiates a state's member to address table
#define INIT_STATE_MEMBER_NAME_TO_ADDR(T, name, type, _isObject)                                                       \
  do {                                                                                                                 \
    addrTableInitializer.push_back(StrToMemberAddr());                                                                 \
    StrToMemberAddr &tableItem = addrTableInitializer.back();                                                          \
    tableItem.memberName = STRING(name);                                                                               \
    if (!strncmp(tableItem.memberName, "m_", 2))                                                                       \
      tableItem.memberName += 2;                                                                                       \
    tableItem.getMember = GetSubStateMemberHelper<T, decltype(&SubState::name), &SubState::name>::getMemberPtr;        \
    tableItem.memberType = type;                                                                                       \
    tableItem.arrayMaxSize = 1;                                                                                        \
    tableItem.isSection = _isObject;                                                                                   \
  } while (false)

// =====================================================================================================================
// Initiates a state's member to address table with explicit name
#define INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(T, name, member, getter, type, _isObject)                               \
  do {                                                                                                                 \
    addrTableInitializer.push_back(StrToMemberAddr());                                                                 \
    StrToMemberAddr &tableItem = addrTableInitializer.back();                                                          \
    tableItem.memberName = STRING(name);                                                                               \
    if (!strncmp(tableItem.memberName, "m_", 2))                                                                       \
      tableItem.memberName += 2;                                                                                       \
    tableItem.getMember = getter;                                                                                      \
    tableItem.memberType = type;                                                                                       \
    tableItem.arrayMaxSize = 1;                                                                                        \
    tableItem.isSection = _isObject;                                                                                   \
  } while (false)

// =====================================================================================================================
// Initiates a array member to address table
#define INIT_MEMBER_ARRAY_NAME_TO_ADDR(T, name, type, maxSize, _isObject)                                              \
  do {                                                                                                                 \
    addrTableInitializer.push_back(StrToMemberAddr());                                                                 \
    StrToMemberAddr &tableItem = addrTableInitializer.back();                                                          \
    tableItem.memberName = STRING(name);                                                                               \
    if (!strncmp(tableItem.memberName, "m_", 2))                                                                       \
      tableItem.memberName += 2;                                                                                       \
    tableItem.getMember = GetMemberHelper<decltype(&T::name), &T::name>::getMemberPtr;                                 \
    tableItem.memberType = type;                                                                                       \
    tableItem.arrayMaxSize = maxSize;                                                                                  \
    tableItem.isSection = _isObject;                                                                                   \
  } while (false)

// =====================================================================================================================
// Initiates a dynamic array member to address table
#define INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(T, name, type, _isObject)                                                    \
  do {                                                                                                                 \
    addrTableInitializer.push_back(StrToMemberAddr());                                                                 \
    StrToMemberAddr &tableItem = addrTableInitializer.back();                                                          \
    tableItem.memberName = STRING(name);                                                                               \
    if (!strncmp(tableItem.memberName, "m_", 2))                                                                       \
      tableItem.memberName += 2;                                                                                       \
    tableItem.getMember = GetMemberHelper<decltype(&T::name), &T::name>::getMemberPtr;                                 \
    tableItem.memberType = type;                                                                                       \
    tableItem.arrayMaxSize = VfxDynamicArrayId;                                                                        \
    tableItem.isSection = _isObject;                                                                                   \
  } while (false)

// =====================================================================================================================
// Cases a section to sub section
#define CASE_SUBSECTION(ENUM, TYPE)                                                                                    \
  case ENUM: {                                                                                                         \
    TYPE *subSectionObj = nullptr;                                                                                     \
    result = section->getPtrOf(lineNum, memberName, true, arrayIndex, &subSectionObj, errorMsg);                       \
    *ptrOut = subSectionObj;                                                                                           \
    break;                                                                                                             \
  }

// =====================================================================================================================
// Initiates section info
#define INIT_SECTION_INFO(NAME, ...)                                                                                   \
  { Section::m_sectionInfo[NAME] = initSectionItemInfo(__VA_ARGS__); }

// =====================================================================================================================
// Represents the structure that maps a string to a class member
struct StrToMemberAddr {
  const char *memberName;        // String form name
  MemberType memberType;         // Member value type
  void *(*getMember)(void *obj); // Get the member from an object
  unsigned arrayMaxSize;         // If greater than 1, this member is an array
  bool isSection;                // Is this member another Section object
};

// =====================================================================================================================
// Represents a reference to a non-owned array of StrToMemberAddr, similar to llvm::ArrayRef.
struct StrToMemberAddrArrayRef {
  StrToMemberAddr *data;
  uint64_t size;
};

// =====================================================================================================================
// Represents an object whose member can be set through it's string form name.
class Section {
public:
  Section(StrToMemberAddrArrayRef addrTable, SectionType type, const char *sectionName);
  virtual ~Section() {}

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

  void printSelf(Document *pDoc, unsigned level);

  void setLineNum(unsigned lineNum) { m_lineNum = lineNum; }

  unsigned getLineNum() const { return m_lineNum; }

private:
  Section(){};

public:
  static std::map<std::string, SectionInfo> m_sectionInfo; // Section info

protected:
  SectionType m_sectionType; // Section type
  const char *m_sectionName; // Section name
  unsigned m_lineNum;        // Line number of this section

private:
  StrToMemberAddr *m_memberTable; // Member address table
  unsigned m_tableSize;           // Address table size
  bool m_isActive;                // If the section is active
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
  SectionVersion() : Section(getAddrTable(), SectionTypeVersion, nullptr) { m_version = 0; };

  void getSubState(unsigned &state) { state = m_version; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_NAME_TO_ADDR(SectionVersion, m_version, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  unsigned m_version; // Document version
};

// =====================================================================================================================
// Represents the class that includes data to represent one spec constant
class SectionSpecConstItem : public Section {
public:
  typedef SpecConstItem SubState;

  SectionSpecConstItem() : Section(getAddrTable(), SectionTypeUnset, "specConst"){};

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, i, MemberTypeIVec4, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, f, MemberTypeFVec4, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, d, MemberTypeDVec2, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent spec constants in on shader stage
class SectionSpecConst : public Section {
public:
  typedef SpecConst SubState;

  SectionSpecConst(const char *name = nullptr) : Section(getAddrTable(), SectionTypeUnset, name){};

  void getSubState(SubState &state) {
    state.numSpecConst = 0;
    for (unsigned i = 0; i < MaxResultCount; ++i) {
      if (m_specConst[i].isActive())
        m_specConst[i].getSubState(state.specConst[state.numSpecConst++]);
    }
  }

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionSpecConst, m_specConst, MemberTypeSpecConstItem, MaxSpecConstantCount,
                                     true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SectionSpecConstItem m_specConst[MaxSpecConstantCount]; // Spec constant for one shader stage
};

// =====================================================================================================================
// Represents the class that includes all kinds of shader source
class SectionShader : public Section {
public:
  typedef ShaderSource SubState;
  SectionShader(const SectionInfo &info)
      : Section(getAddrTable(), info.type, nullptr), m_shaderType(static_cast<ShaderType>(info.propertyLo)),
        m_shaderStage(static_cast<ShaderStage>(info.propertyHi)) {}

  virtual bool isShaderSourceSection();

  virtual void addLine(const char *line) { m_shaderSource += line; };

  bool compileShader(const std::string &docFilename, const char *entryPoint, std::string *errorMsg);

  void getSubState(SubState &state);
  ShaderType getShaderType() { return m_shaderType; }
  ShaderStage getShaderStage() { return m_shaderStage; }

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_NAME_TO_ADDR(SectionShader, m_fileName, MemberTypeString, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  bool compileGlsl(const char *entryPoint, std::string *errorMsg);
  bool assembleSpirv(std::string *errorMsg);

  std::string m_fileName;        // External shader source file name
  std::string m_shaderSource;    // Shader source code
  ShaderType m_shaderType;       // Shader type
  ShaderStage m_shaderStage;     // Shader stage;
  std::vector<uint8_t> m_spvBin; // SPIRV shader binary
};

// =====================================================================================================================
// Represents the class that includes all kinds of compile log, This section is ignored in Document::GetDocument
class SectionCompileLog : public Section {
public:
  SectionCompileLog() : Section(getAddrTable(), SectionTypeCompileLog, nullptr) {}

  virtual void addLine(const char *line) { m_compileLog += line; };

private:
  // Returns an empty table currently
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  std::string m_compileLog; // Compile Log
};

// =====================================================================================================================
// Represents the sub section color target
class SectionColorBuffer : public Section {
public:
  typedef ColorBuffer SubState;

  SectionColorBuffer() : Section(getAddrTable(), SectionTypeUnset, "colorBuffer") {
    memset(&m_state, 0, sizeof(m_state));
    m_state.channelWriteMask = 0xF;
  }

  void getSubState(SubState &state) {
    state = m_state;
    state.palFormat = m_palFormat.empty() ? nullptr : m_palFormat.c_str();
  };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, format, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, blendEnable, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, blendSrcAlphaToColor, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, channelWriteMask, MemberTypeInt, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionColorBuffer, m_palFormat, MemberTypeString, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
  std::string m_palFormat;
};

#if VKI_RAY_TRACING
// =====================================================================================================================
// Represents the sub section shader group
class SectionShaderGroup : public Section {
public:
  typedef VkRayTracingShaderGroupCreateInfoKHR SubState;

  SectionShaderGroup() : Section(getAddrTable(), SectionTypeUnset, "groups") { memset(&m_state, 0, sizeof(m_state)); }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderGroup, type, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderGroup, generalShader, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderGroup, closestHitShader, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderGroup, anyHitShader, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionShaderGroup, intersectionShader, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};
#endif

// =====================================================================================================================
// Represents the sub section vertex input binding
class SectionVertexInputBinding : public Section {
public:
  typedef VkVertexInputBindingDescription SubState;

  SectionVertexInputBinding() : Section(getAddrTable(), SectionTypeUnset, "binding") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, binding, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, stride, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, inputRate, MemberTypeEnum, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input attribute
class SectionVertexInputAttribute : public Section {
public:
  typedef VkVertexInputAttributeDescription SubState;

  SectionVertexInputAttribute() : Section(getAddrTable(), SectionTypeUnset, "binding") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, location, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, binding, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, format, MemberTypeEnum, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, offset, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input divisor
class SectionVertexInputDivisor : public Section {
public:
  typedef VkVertexInputBindingDivisorDescriptionEXT SubState;

  SectionVertexInputDivisor() : Section(getAddrTable(), SectionTypeUnset, "divisor") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputDivisor, binding, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputDivisor, divisor, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the section vertex input
class SectionVertexInput : public Section {
public:
  typedef VkPipelineVertexInputStateCreateInfo SubState;

  SectionVertexInput() : Section(getAddrTable(), SectionTypeVertexInputState, nullptr) {
    memset(&m_vkDivisorState, 0, sizeof(m_vkDivisorState));
    m_vkDivisorState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
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
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, m_attribute, MemberTypeVertexInputAttributeItem, true);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, m_binding, MemberTypeVertexInputBindingItem, true);
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, m_divisor, MemberTypeVertexInputDivisorItem, true);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  std::vector<SectionVertexInputAttribute> m_attribute;                // Vertex input attribute
  std::vector<SectionVertexInputBinding> m_binding;                    // Vertex input binding
  std::vector<SectionVertexInputDivisor> m_divisor;                    // Vertex input divisor
  std::vector<VkVertexInputBindingDescription> m_vkBindings;           // Vulkan input binding
  std::vector<VkVertexInputAttributeDescription> m_vkAttributes;       // Vulkan vertex input attribute
  std::vector<VkVertexInputBindingDivisorDescriptionEXT> m_vkDivisors; // Vulkan vertex input divisor
  VkPipelineVertexInputDivisorStateCreateInfoEXT m_vkDivisorState;     // Vulkan vertex input divisor state
};

// =====================================================================================================================
// Represents the sub section specialization constant map entry
class SectionSpecEntryItem : public Section {
public:
  typedef VkSpecializationMapEntry SubState;

  SectionSpecEntryItem() : Section(getAddrTable(), SectionTypeUnset, "mapEntry") {
    memset(&m_state, 0, sizeof(m_state));
  }

  void getSubState(SubState &state) { state = m_state; };
  SubState &getSubStateRef() { return m_state; };

private:
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, constantID, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, offset, MemberTypeInt, false);
      INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, size, MemberTypeInt, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

  SubState m_state;
};

// =====================================================================================================================
// Represents the sub section specialization constant info
class SectionSpecInfo : public Section {
public:
  typedef VkSpecializationInfo SubState;

  SectionSpecInfo() : Section(getAddrTable(), SectionTypeUnset, "specConst") {
    m_intData = &m_bufMem;
    m_uintData = &m_bufMem;
    m_int64Data = &m_bufMem;
    m_uint64Data = &m_bufMem;
    m_floatData = &m_bufMem;
    m_doubleData = &m_bufMem;
    m_float16Data = &m_bufMem;
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
  static StrToMemberAddrArrayRef getAddrTable() {
    static std::vector<StrToMemberAddr> addrTable = []() {
      std::vector<StrToMemberAddr> addrTableInitializer;
      INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionSpecInfo, m_mapEntry, MemberTypeSpecEntryItem, true);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_intData, MemberTypeIArray, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_uintData, MemberTypeUArray, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_int64Data, MemberTypeI64Array, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_uint64Data, MemberTypeU64Array, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_floatData, MemberTypeFArray, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_doubleData, MemberTypeDArray, false);
      INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, m_float16Data, MemberTypeF16Array, false);
      return addrTableInitializer;
    }();
    return {addrTable.data(), addrTable.size()};
  }

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

} // namespace Vfx
