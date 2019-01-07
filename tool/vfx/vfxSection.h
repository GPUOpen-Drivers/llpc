/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include <string.h>
#include <stddef.h>
#include <vector>
#include <sstream>
#include <map>
#include "vfx.h"
#include "vfxError.h"

namespace Vfx
{
// =====================================================================================================================
// Enumerates VFX section type.
enum SectionType : uint32_t
{
    SectionTypeUnset = 0,               // Initial state, not entering any section.
    // Beginning of rule based key-value sections
    SectionTypeResult,                  // Result section
    SectionTypeBufferView,              // Buffer view section
    SectionTypeVertexState,             // Vertex state section
    SectionTypeDrawState,               // Draw state section
    SectionTypeImageView,               // Image view section
    SectionTypeSampler,                 // Sampler section
    SectionTypeVersion,                 // Version section
    SectionTypeGraphicsState,           // Graphics state section
    SectionTypeComputeState,            // Compute state section
    SectionTypeVertexInputState,        // Vertex input state section
    SectionTypeVertexShaderInfo,        // Vertex shader info section
    SectionTypeTessControlShaderInfo,   // Tess control shader info section
    SectionTypeTessEvalShaderInfo,      // Tess enval shader info section
    SectionTypeGeometryShaderInfo,      // Geometry shader info section
    SectionTypeFragmentShaderInfo,      // Fragment shader info section
    SectionTypeComputeShaderInfo,       // Compute shader info section
    SectionTypeCompileLog,              // Compile log section
    SectionTypeVertexShader,            // Vertex shader source section
    SectionTypeTessControlShader,       // Tess control shader source section
    SectionTypeTessEvalShader,          // Tess eval shader source section
    SectionTypeGeometryShader,          // Geometry shader source section
    SectionTypeFragmentShader,          // Fragment shader source section
    SectionTypeComputeShader,           // Compute shader source section
    SectionTypeNameNum,                 // Name num section
};

// =====================================================================================================================
// Enumerates VFX member type.
enum MemberType : uint32_t
{
    MemberTypeInt,                           // VFX member type: 32 bit integer
    MemberTypeFloat,                         // VFX member type: 32 bit float
    MemberTypeFloat16,                       // VFX member type: 16 bit float
    MemberTypeDouble,                        // VFX member type: 64 bit double
    MemberTypeBool,                          // VFX member type: boolean
    MemberTypeIVec4,                         // VFX member type: int vec4
    MemberTypeI64Vec2,                       // VFX member type: int64 vec2
    MemberTypeFVec4,                         // VFX member type: float vec4
    MemberTypeF16Vec4,                       // VFX member type: float16 vec4
    MemberTypeDVec2,                         // VFX member type: double vec2
    MemberTypeIArray,                        // VFX member type: int vector (dynamic array)
    MemberTypeUArray,                        // VFX member type: uint vector (dynamic array)
    MemberTypeI64Array,                      // VFX member type: int64 vector (dynamic array)
    MemberTypeU64Array,                      // VFX member type: uint64 vector (dynamic array)
    MemberTypeFArray,                        // VFX member type: float vector (dynamic array)
    MemberTypeF16Array,                      // VFX member type: float16 vector (dynamic array)
    MemberTypeDArray,                        // VFX member type: double vector (dynamic array)
    MemberTypeEnum,                          // VFX member type: Enums from Vulkan API
    MemberTypeBinding,                       // VFX member type: Binding
    MemberTypeString,                        // VFX member type: String
    MemberTypeResultItem,                    // VFX member type: SectionResultItem
    MemberTypeVertexBufferBindingItem,       // VFX member type: SectionVertexBufferBinding
    MemberTypeVertexAttributeItem,           // VFX member type: SectionVertexAttribute
    MemberTypeSpecConstItem,                 // VFX member type: SectionSpecConstItem
    MemberTypeSpecConst,                     // VFX member type: SectionSpecConst
    MemberTypePushConstRange,                // VFX member type: SectionPushConstRange
    MemberTypeVertexInputBindingItem,        // VFX member type: SectionVertexInputBinding
    MemberTypeVertexInputAttributeItem,      // VFX member type: SectionVertexInputAttribute
    MemberTypeVertexInputDivisorItem,        // VFX member type: SectionVertexInputDivisor
    MemberTypeColorBufferItem,               // VFX member type: SectionColorBuffer
    MemberTypeSpecEntryItem,                 // VFX member type: SectionSpecEntryItem
    MemberTypeResourceMappingNode,           // VFX member type: SectionResourceMappingNode
    MemberTypeSpecInfo,                      // VFX member type: SectionSpecInfo
    MemberTypeDescriptorRangeValue,          // VFX member type: SectionDescriptorRangeValueItem
};

// =====================================================================================================================
// Enumerates the property of shader source section
enum ShaderType
{
    Glsl,            // GLSL source
    SpirvAsm,        // SPIRV assemble code
    GlslFile,        // GLSL source in extenal file
    SpirvFile,       // SPIRV binary code in external file
    SpirvAsmFile,    // SPIRV assemble code in external file
};

// =====================================================================================================================
// Gets offset of the member
#ifdef _WIN32
    #define OFFSETOF(T, name) offsetof(T, name)
#else
    // Clang has a warning for using offsetof for non-POD type
    #define OFFSETOF(T, name) ((size_t)(&((T*)(0))->name))
#endif

// =====================================================================================================================
// Initiates a member to address table
#define INIT_MEMBER_NAME_TO_ADDR(T, name, type, _isObject) pTableItem->pMemberName = STRING(name); \
    pTableItem->memberOffset = (uint32_t)(OFFSETOF(T, name));       \
    pTableItem->memberType = type;         \
    pTableItem->arrayMaxSize = 1;          \
    pTableItem->isSection = _isObject;      \
    ++pTableItem;

// =====================================================================================================================
// Initiates a state's member to address table
#define INIT_STATE_MEMBER_NAME_TO_ADDR(T, name, type, _isObject) pTableItem->pMemberName = STRING(name); \
    pTableItem->memberOffset = (uint32_t)(OFFSETOF(SubState, name) + OFFSETOF(T, m_state));       \
    pTableItem->memberType = type;         \
    pTableItem->arrayMaxSize = 1;          \
    pTableItem->isSection = _isObject;      \
    ++pTableItem;

// =====================================================================================================================
// Initiates a state's member to address table with explicit name
#define INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(T, name, member, type, _isObject) \
    pTableItem->pMemberName = STRING(name); \
    pTableItem->memberOffset = (uint32_t)(OFFSETOF(SubState, member) + OFFSETOF(T, m_state));       \
    pTableItem->memberType = type;         \
    pTableItem->arrayMaxSize = 1;          \
    pTableItem->isSection = _isObject;      \
    ++pTableItem;

// =====================================================================================================================
// Initiates a array member to address table
#define INIT_MEMBER_ARRAY_NAME_TO_ADDR(T, name, type, maxSize, _isObject) pTableItem->pMemberName = STRING(name); \
    pTableItem->memberOffset = (uint32_t)(OFFSETOF(T, name));        \
    pTableItem->memberType = type;         \
    pTableItem->arrayMaxSize = maxSize;    \
    pTableItem->isSection = _isObject;      \
    ++pTableItem;

// =====================================================================================================================
// Initiates a dynamic array member to address table
#define INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(T, name, type, _isObject) pTableItem->pMemberName = STRING(name); \
    pTableItem->memberOffset = (uint32_t)(OFFSETOF(T, name));        \
    pTableItem->memberType = type;         \
    pTableItem->arrayMaxSize = VfxDynamicArrayId;  \
    pTableItem->isSection = _isObject;      \
    ++pTableItem;

// =====================================================================================================================
// Csses a section to sub section
#define CASE_SUBSECTION(ENUM, TYPE)  \
    case ENUM:    \
        {                            \
            TYPE *pSubSectionObj = nullptr;   \
            result = GetPtrOf(lineNum, pMemberName, true, arrayIndex, &pSubSectionObj, pErrorMsg);\
            *ptrOut = pSubSectionObj; \
            break; \
        }

// =====================================================================================================================
// Initiates section info
#define INIT_SECTION_INFO(NAME, type, property) { \
    SectionInfo sectionInfo = { type, property }; \
    m_sectionInfo[NAME] = sectionInfo;}

// =====================================================================================================================
// Represents the structure that maps a string to a class member offset
struct StrToMemberAddr
{
    const char*   pMemberName;    // String form name
    MemberType    memberType;     // Member value type
    uint32_t      memberOffset;   // Member offset in bytes within the object
    uint32_t      arrayMaxSize;   // If greater than 1, this member is an array
    bool          isSection;      // Is this member another Section object
};

// =====================================================================================================================
// Represents the info of section type
struct SectionInfo
{
    SectionType type;           // Section type
    uint32_t    property;       // Additional section information
};

// =====================================================================================================================
// Represents an object whose member can be set throught it's string form name.
class Section
{
public:
    Section(StrToMemberAddr* addrTable, uint32_t tableSize, SectionType type, const char* sectionName);
    virtual ~Section() {}

    static Section* CreateSection(const char* pSectionName);
    static SectionType GetSectionType(const char* pSectionName);
    static void InitSectionInfo();

    virtual bool IsShaderSourceSection() { return false;}

    // Adds a new line to section, it is only valid for non-rule based section
    virtual void AddLine(const char* pLine) { };

    // Gets section type.
    SectionType GetSectionType() const { return m_sectionType; }

    void* GetMemberAddr(uint32_t i)
    {
        return reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(this) + m_pMemberTable[i].memberOffset);
    }

    bool GetMemberType(uint32_t lineNum, const char* memberName, MemberType* pValueType, std::string* pErrorMsg);

    bool GetPtrOfSubSection(uint32_t     lineNum,
                            const char*  memberName,
                            MemberType   memberType,
                            bool         isWriteAccess,
                            uint32_t     arrayIndex,
                            Section**    ptrOut,
                            std::string* pErrorMsg);

    // Gets ptr of a member.
    template<typename TValue>
    bool GetPtrOf(uint32_t     lineNum,
                  const char*  memberName,
                  bool         isWriteAccess,
                  uint32_t     arrayIndex,
                  TValue**     ptrOut,
                  std::string* pErrorMsg);

    // Sets value to a member array
    template<typename TValue>
    bool Set(uint32_t lineNum, const char* fieldName, uint32_t arrayIndex, TValue* pValue);

    // Sets value to a member
    template<typename TValue>
    bool Set(uint32_t lineNum, const char* fieldName, TValue* pValue)
    {
        return Set(lineNum, fieldName, 0, pValue);
    };

    bool IsSection(uint32_t lineNum, const char* memberName, bool* pOutput, MemberType *pType, std::string* pErrorMsg);

    // Has this object been configured in VFX file.
    bool IsActive() { return m_isActive; }

    void SetActive(bool isActive) { m_isActive = isActive; }

    void PrintSelf(uint32_t level);

    void SetLineNum(uint32_t lineNum) { m_lineNum = lineNum; }
private:
    Section() {};

protected:
    SectionType               m_sectionType;        // Section type
    const char*               m_pSectionName;       // Section name
    uint32_t                  m_lineNum;            // Line number of this section
private:
    StrToMemberAddr*          m_pMemberTable;      // Member address table
    uint32_t                  m_tableSize;         // Address table size
    bool                      m_isActive;          // If the scestion is active
    static std::map<std::string, SectionInfo> m_sectionInfo;    //Section info
};

// =====================================================================================================================
// Gets ptr of a member. return true if operation success
template<typename TValue>
bool Section::GetPtrOf(
    uint32_t     lineNum,           // Line No.
    const char*  memberName,        // [in] Member name
    bool         isWriteAccess,     // True for write
    uint32_t     arrayIndex,        // Array index
    TValue**     ptrOut,            // [out] Pointer of section member
    std::string* pErrorMsg)         // [out] Error message
{
    bool        result      = true;
    void*      pMemberAddr  = reinterpret_cast<void*>(VfxInvalidValue);
    MemberType memberType   = MemberTypeInt;
    uint32_t   arrayMaxSize = 0;

    if (isWriteAccess == true)
    {
        SetActive(true);
    }
    // Search section member
    for (uint32_t i = 0; i < m_tableSize; ++i)
    {
        if (strcmp(memberName, m_pMemberTable[i].pMemberName) == 0)
        {
            pMemberAddr = GetMemberAddr(i);
            memberType  = m_pMemberTable[i].memberType;
            if (arrayIndex >= m_pMemberTable[i].arrayMaxSize)
            {
                PARSE_ERROR(*pErrorMsg,
                            lineNum,
                            "Array access out of bound: %u of %s[%u]",
                            arrayIndex,
                            memberName,
                            m_pMemberTable[i].arrayMaxSize);
                result = false;
            }
            arrayMaxSize = m_pMemberTable[i].arrayMaxSize;
            break;
        }
    }

    if ((result == true) && (pMemberAddr == reinterpret_cast<void*>(VfxInvalidValue)))
    {
        PARSE_WARNING(*pErrorMsg, lineNum, "Invalid member name: %s", memberName);
        result = false;
    }

    if (result == true)
    {
        VFX_ASSERT(ptrOut != nullptr);
        if (arrayMaxSize == VfxDynamicArrayId)
        {
            // Member is dynamic array, cast to std::vector
            std::vector<TValue>* pMemberVector = reinterpret_cast<std::vector<TValue>*>(pMemberAddr);
            if (pMemberVector->size() <= arrayIndex)
            {
                pMemberVector->resize(arrayIndex + 1);
            }
            *ptrOut = &((*pMemberVector)[arrayIndex]);
        }
        else
        {
            TValue* pMember = reinterpret_cast<TValue*>(pMemberAddr);
            *ptrOut = pMember + arrayIndex;
        }
    }

    return result;
};

// =====================================================================================================================
// Sets value to a member array
template<typename TValue>
bool Section::Set(
    uint32_t    lineNum,           // Line No.
    const char* pMemberName,       // [in] Name of section member
    uint32_t    arrayIndex,        // Array index
    TValue*     pValue)            // [in] Value to be set
{
    bool result = false;
    VFX_ASSERT(pValue != nullptr);
    TValue* pMemberPtr = nullptr;
    std::string dummyMsg;
    result = GetPtrOf(lineNum, pMemberName, true, arrayIndex, &pMemberPtr, &dummyMsg);
    VFX_ASSERT(result == true);
    if (result == true)
    {
        *pMemberPtr = *pValue;
    }

    return result;
};

// =====================================================================================================================
// Represents the document version.
class SectionVersion : public Section
{
public:
    SectionVersion() :
        Section(m_addrTable, MemberCount, SectionTypeVersion, nullptr)
    {
        version = 0;
    };

    // Setup member name to member offset mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_NAME_TO_ADDR(SectionVersion, version, MemberTypeInt, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(uint32_t& state)  { state = version; };

private:
    static const uint32_t  MemberCount = 1;
    static StrToMemberAddr m_addrTable[MemberCount];

    uint32_t               version;            // Document version
};
// =====================================================================================================================
// Represents the class that includes data to verify a test result.
class SectionResultItem : public Section
{
public:
    typedef ResultItem SubState;

    SectionResultItem() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "ResultItem")
    {
        memset(&m_state, 0, sizeof(m_state));
        m_state.resultSource = ResultSourceMaxEnum;
        m_state.compareMethod = ResultCompareMethodEqual;
    };

    // Setup member name to member offset mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, resultSource,   MemberTypeEnum,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, bufferBinding,  MemberTypeBinding, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, offset,         MemberTypeIVec4,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, iVec4Value,     MemberTypeIVec4,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, i64Vec2Value,   MemberTypeI64Vec2, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, fVec4Value,     MemberTypeFVec4,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, f16Vec4Value,   MemberTypeF16Vec4, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, dVec2Value,     MemberTypeDVec2,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResultItem, compareMethod,  MemberTypeEnum,    false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 9;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState               m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent the result section
class SectionResult : public Section
{
public:
    typedef TestResult SubState;

    SectionResult() :
        Section(m_addrTable, MemberCount, SectionTypeResult, nullptr)
    {
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionResult,
                                       result,
                                       MemberTypeResultItem,
                                       MaxResultCount,
                                       true);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        state.numResult = 0;
        for (uint32_t i = 0; i < MaxResultCount; ++i)
        {
            if (result[i].IsActive())
            {
                result[i].GetSubState(state.result[state.numResult++]);
            }
        }
    }

private:
    static const uint32_t  MemberCount = 1;
    static StrToMemberAddr m_addrTable[MemberCount];

    SectionResultItem      result[MaxResultCount]; // section result items
};

// =====================================================================================================================
// Represents the class that includes data to represent one spec constant
class SectionSpecConstItem : public Section
{
public:
    typedef SpecConstItem SubState;

    SectionSpecConstItem() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "specConst")
    {
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, i, MemberTypeIVec4, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, f, MemberTypeFVec4, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecConstItem, d, MemberTypeDVec2, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };
private:
    static const uint32_t     MemberCount = 3;
    static StrToMemberAddr    m_addrTable[MemberCount];

    SubState                  m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent spec constants in on shader stage
class SectionSpecConst : public Section
{
public:
    typedef SpecConst SubState;

     SectionSpecConst(const char* name = nullptr) :
        Section(m_addrTable, MemberCount, SectionTypeUnset, name)
    {
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionSpecConst,
                                       specConst,
                                       MemberTypeSpecConstItem,
                                       MaxSpecConstantCount,
                                       true);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        state.numSpecConst = 0;
        for (uint32_t i = 0; i < MaxResultCount; ++i)
        {
            if (specConst[i].IsActive())
            {
                specConst[i].GetSubState(state.specConst[state.numSpecConst++]);
            }
        }
    }

private:
    static const uint32_t  MemberCount = 3;
    static StrToMemberAddr m_addrTable[MemberCount];

    SectionSpecConstItem   specConst[MaxSpecConstantCount]; // Spec constant for one shader stage
};

// =====================================================================================================================
// Represents the class of vertex buffer binding.
class SectionVertexBufferBinding : public Section
{
public:
    typedef VertrexBufferBinding SubState;

    SectionVertexBufferBinding() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "VertexBufferBinding")
    {
        m_state.binding = VfxInvalidValue;
        m_state.strideInBytes = VfxInvalidValue;
        m_state.stepRate = VK_VERTEX_INPUT_RATE_VERTEX;
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, binding,       MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, strideInBytes, MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexBufferBinding, stepRate,      MemberTypeEnum, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t     MemberCount = 3;
    static StrToMemberAddr    m_addrTable[MemberCount];

    SubState                  m_state;
};

// =====================================================================================================================
// Represents the class of vertex attribute.
class SectionVertexAttribute : public Section
{
public:
    typedef VertexAttribute SubState;

    SectionVertexAttribute() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "VertexAttribute")
    {
        m_state.binding = VfxInvalidValue;
        m_state.format = VK_FORMAT_UNDEFINED;
        m_state.location = VfxInvalidValue;
        m_state.offsetInBytes = VfxInvalidValue;
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, binding,       MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, format,        MemberTypeEnum,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, location,      MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexAttribute, offsetInBytes, MemberTypeInt,    false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 4;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState               m_state;
};

// =====================================================================================================================
// Represents the class of vertex state
class SectionVertexState : public Section
{
public:
    typedef VertexState    SubState;

    SectionVertexState() :
        Section(m_addrTable, MemberCount, SectionTypeVertexState, nullptr)
    {
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionVertexState,
                                       vbBinding,
                                       MemberTypeVertexBufferBindingItem,
                                       MaxVertexBufferBindingCount,
                                       true);
        INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionVertexState,
                                       attribute,
                                       MemberTypeVertexAttributeItem,
                                       MaxVertexAttributeCount,
                                       true);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        state.numVbBinding = 0;
        for (uint32_t i = 0; i < MaxVertexBufferBindingCount; ++i)
        {
            if (vbBinding[i].IsActive())
            {
                vbBinding[i].GetSubState(state.vbBinding[state.numVbBinding++]);
            }
        }

        state.numAttribute = 0;
        for (uint32_t i = 0; i < MaxVertexAttributeCount; ++i)
        {
            if (attribute[i].IsActive())
            {
                attribute[i].GetSubState(state.attribute[state.numAttribute++]);
            }
        }
    }

private:
    static const uint32_t       MemberCount = 2;
    static StrToMemberAddr      m_addrTable[MemberCount];
    SectionVertexBufferBinding  vbBinding[MaxVertexBufferBindingCount];    // Binding info of all vertex buffers
    SectionVertexAttribute      attribute[MaxVertexAttributeCount];        // Attribute info of all vertex attributes
};

// =====================================================================================================================
// Represents the class that includes data needed to create buffer and buffer view object.
class SectionBufferView : public Section
{
public:
    typedef BufferView SubState;

    SectionBufferView() :
        Section(m_addrTable, MemberCount, SectionTypeBufferView, nullptr),
        intData(&m_bufMem),
        uintData(&m_bufMem),
        int64Data(&m_bufMem),
        uint64Data(&m_bufMem),
        floatData(&m_bufMem),
        doubleData(&m_bufMem),
        float16Data(&m_bufMem)
    {
        memset(&m_state, 0, sizeof(m_state));
        m_state.size     = VfxInvalidValue;
        m_state.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, binding,       MemberTypeBinding,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, descriptorType,MemberTypeEnum,       false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, size,          MemberTypeInt,        false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionBufferView, format,        MemberTypeEnum,       false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       intData,       MemberTypeIArray,     false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       uintData,      MemberTypeUArray,     false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       int64Data,     MemberTypeI64Array,   false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       uint64Data,    MemberTypeU64Array,   false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       floatData,     MemberTypeFArray,     false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       doubleData,    MemberTypeDArray,     false);
        INIT_MEMBER_NAME_TO_ADDR(SectionBufferView,       float16Data,   MemberTypeF16Array,   false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        m_state.dataSize = static_cast<uint32_t>(m_bufMem.size());
        m_state.pData = m_state.dataSize > 0 ? &m_bufMem[0] : nullptr;
        state = m_state;
    };

private:
    static const uint32_t     MemberCount = 11;
    static StrToMemberAddr    m_addrTable[MemberCount];
    SubState                  m_state;       // State of BufferView
    std::vector<uint8_t>      m_bufMem;      // Underlying buffer for all data types.
    std::vector<uint8_t>*     intData;       // Contains int data of this buffer
    std::vector<uint8_t>*     uintData;      // Contains uint data of this buffer
    std::vector<uint8_t>*     int64Data;     // Contains int64 data of this buffer
    std::vector<uint8_t>*     uint64Data;    // Contains uint64 data of this buffer
    std::vector<uint8_t>*     floatData;     // Contains float data of this buffer
    std::vector<uint8_t>*     doubleData;    // Contains double data of this buffer
    std::vector<uint8_t>*     float16Data;   // Contains float16 data of this buffer
};

// =====================================================================================================================
// Represents the class of image/image view object
class SectionImageView  : public Section
{
public:
    typedef ImageView SubState;

    SectionImageView() :
        Section(m_addrTable, MemberCount, SectionTypeImageView, nullptr)
    {
        memset(&m_state, 0, sizeof(m_state));
        m_state.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        m_state.viewType       = VK_IMAGE_VIEW_TYPE_2D;
        m_state.dataPattern    = ImageCheckBoxUnorm;
        m_state.samples        = 1;
        m_state.mipmap         = 0;
    }

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, binding,       MemberTypeBinding,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, descriptorType,MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, size,          MemberTypeBinding,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, viewType,      MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, dataPattern,   MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, samples,       MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionImageView, mipmap,        MemberTypeInt,      false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t     MemberCount = 7;
    static StrToMemberAddr    m_addrTable[MemberCount];
    SubState                  m_state;
};

// =====================================================================================================================
// Represents the class of sampler object
class SectionSampler : public Section
{
public:
    typedef Sampler SubState;

    SectionSampler() :
        Section(m_addrTable, MemberCount, SectionTypeSampler, nullptr)

    {
        memset(&m_state, 0, sizeof(m_state));
        m_state.descriptorType = static_cast<VkDescriptorType>(VfxInvalidValue);
        m_state.dataPattern = static_cast<SamplerPattern>(VfxInvalidValue);
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, binding,       MemberTypeBinding, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, descriptorType,MemberTypeEnum,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSampler, dataPattern,   MemberTypeEnum,    false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t     MemberCount = 3;
    static StrToMemberAddr    m_addrTable[MemberCount];

    SubState                  m_state;
};

// =====================================================================================================================
// Represents the class that includes data to represent one push constant range
class SectionPushConstRange : public Section
{
public:
    typedef PushConstRange SubState;

    SectionPushConstRange() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "pushConstRange"),
        intData(&m_bufMem),
        uintData(&m_bufMem),
        floatData(&m_bufMem),
        doubleData(&m_bufMem)
    {
    };

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPushConstRange, start,       MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionPushConstRange, length,      MemberTypeInt,    false);
        INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange,       intData,     MemberTypeIArray, false);
        INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange,       uintData,    MemberTypeUArray, false);
        INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange,       floatData,   MemberTypeFArray, false);
        INIT_MEMBER_NAME_TO_ADDR(SectionPushConstRange,       doubleData,  MemberTypeDArray, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
       m_state.dataSize = static_cast<uint32_t>(m_bufMem.size()) * sizeof(uint32_t);
       m_state.pData = state.dataSize > 0 ? &m_bufMem[0] : nullptr;
       state = m_state;
    };

private:
    static const uint32_t     MemberCount = 6;
    static StrToMemberAddr    m_addrTable[MemberCount];

    std::vector<uint32_t>     m_bufMem;      // Underlying buffer for all data types.
    std::vector<uint32_t>*    intData;       // Contains int data of this push constant range
    std::vector<uint32_t>*    uintData;      // Contains uint data of this push constant range
    std::vector<uint32_t>*    floatData;     // Contains float data of this push constant range
    std::vector<uint32_t>*    doubleData;    // Contains double data of this push constant range
    SubState                  m_state;
};

// =====================================================================================================================
// Represents the class that includes data needed to create global draw state
class SectionDrawState : public Section
{
public:
    typedef DrawState SubState;

    SectionDrawState() :
        Section(m_addrTable, MemberCount, SectionTypeDrawState, nullptr),
        vs("vs"),
        tcs("tcs"),
        tes("tes"),
        gs("gs"),
        fs("fs"),
        cs("cs")
    {
        InitDrawState(m_state);
    }

    // Set initial value for DrawState.
    static void InitDrawState(SubState& state)
    {
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
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, instance,       MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, vertex,         MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstInstance,  MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstVertex,    MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, index,          MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, firstIndex,     MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, vertexOffset,   MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, topology,       MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, polygonMode,    MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, cullMode,       MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, frontFace,      MemberTypeEnum,     false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, depthBiasEnable,    MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, patchControlPoints, MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, dispatch,       MemberTypeIVec4,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, width,          MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, height,         MemberTypeInt,      false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, lineWidth,      MemberTypeFloat,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDrawState, viewport,       MemberTypeIVec4,    false);
        INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, vs,   MemberTypeSpecConst, true);
        INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, tcs,  MemberTypeSpecConst, true);
        INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, tes,  MemberTypeSpecConst, true);
        INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, gs,   MemberTypeSpecConst, true);
        INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, fs,   MemberTypeSpecConst, true);
        INIT_MEMBER_NAME_TO_ADDR(SectionDrawState, cs,   MemberTypeSpecConst, true);
        INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionDrawState, pushConstRange, MemberTypePushConstRange, MaxPushConstRangCount, true);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        state = m_state;
        vs.GetSubState(state.vs);
        tcs.GetSubState(state.tcs);
        tes.GetSubState(state.tes);
        gs.GetSubState(state.gs);
        fs.GetSubState(state.fs);
        cs.GetSubState(state.cs);
        state.numPushConstRange = 0;
        for (uint32_t i = 0; i < MaxPushConstRangCount; ++i)
        {
            if (pushConstRange[i].IsActive())
            {
                pushConstRange[i].GetSubState(state.pushConstRange[state.numPushConstRange++]);
            }
        }
    };

private:
    static const uint32_t  MemberCount = 25;
    static StrToMemberAddr m_addrTable[MemberCount];
    SubState               m_state;
    SectionSpecConst       vs;                                       // Vertex shader's spec constant
    SectionSpecConst       tcs;                                      // Tessellation control shader's spec constant
    SectionSpecConst       tes;                                      // Tessellation evaluation shader's spec constant
    SectionSpecConst       gs;                                       // Geometry shader's spec constant
    SectionSpecConst       fs;                                       // Fragment shader's spec constant
    SectionSpecConst       cs;                                       // Compute shader shader's spec constant
    SectionPushConstRange  pushConstRange[MaxPushConstRangCount];    // Pipeline push constant ranges
};

// =====================================================================================================================
// Represents the class that includes all kinds of shader source
class SectionShader : public Section
{
public:
    typedef ShaderSource SubState;
    SectionShader(const SectionInfo& info)
        :
        Section(m_addrTable, MemberCount, info.type, nullptr),
        m_shaderType(static_cast<ShaderType>(info.property))
    {
    }

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_NAME_TO_ADDR(SectionShader, fileName, MemberTypeString, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    virtual bool IsShaderSourceSection();

    void GetSubState(SubState& state)
    {
        state.dataSize = static_cast<uint32_t>(m_spvBin.size());
        state.pData = state.dataSize > 0 ? &m_spvBin[0] : nullptr;
    }

    virtual void AddLine(const char* pLine) { shaderSource += pLine; };

    bool CompileShader(const std::string& docFilename, std::string* pErrorMsg);

private:
    bool ReadFile(const std::string& docFilename, bool isBinary, std::string* pErrorMsg);
    bool CompileGlsl(std::string* pErrorMsg);
    bool AssembleSpirv(std::string* pErrorMsg);

    static const uint32_t  MemberCount = 1;
    static StrToMemberAddr m_addrTable[MemberCount];

    std::string          fileName;                // External shader source file name
    std::string          shaderSource;            // Shader source code
    ShaderType           m_shaderType;            // Shader type
    std::vector<uint8_t> m_spvBin;                // SPIRV shader binary
};

// =====================================================================================================================
// Represents the class that includes all kinds of compile log, This section is ignored in Document::GetDocument
class SectionCompileLog : public Section
{
public:
    SectionCompileLog()
        :
        Section(m_addrTable, MemberCount, SectionTypeCompileLog, nullptr)
    {
    }

    // Setup member name to member address mapping.
    static void InitialAddrTable()
    {
    }

    virtual void AddLine(const char* pLine) { compileLog += pLine; };

private:
    static const uint32_t  MemberCount = 1;
    static StrToMemberAddr m_addrTable[MemberCount];
    std::string            compileLog;            // Compile Log
};

// =====================================================================================================================
// Represents the sub section color target
class SectionColorBuffer : public Section
{
public:
    typedef ColorBuffer SubState;

    SectionColorBuffer() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "colorBuffer")
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, format,               MemberTypeEnum,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, blendEnable,          MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, blendSrcAlphaToColor, MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionColorBuffer, channelWriteMask,     MemberTypeInt,    false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 4;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState               m_state;
};

// =====================================================================================================================
// Represents the section graphics state
class SectionGraphicsState : public Section
{
public:
    typedef GraphicsPipelineState SubState;

    SectionGraphicsState() :
        Section(m_addrTable, MemberCount, SectionTypeGraphicsState, nullptr)
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, topology,                MemberTypeEnum, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, polygonMode,             MemberTypeEnum, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, cullMode,                MemberTypeEnum, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, frontFace,               MemberTypeEnum, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, depthBiasEnable,         MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, patchControlPoints,      MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, deviceIndex,             MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, disableVertexReuse,      MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, depthClipEnable,         MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, rasterizerDiscardEnable, MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, perSampleShading,        MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, numSamples,              MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, samplePatternIdx,        MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, usrClipPlaneMask,        MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, alphaToCoverageEnable,   MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, dualSourceBlendEnable,   MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, switchWinding,           MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, enableMultiView,         MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, includeDisassembly,      MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, autoLayoutDesc,          MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionGraphicsState, scalarBlockLayout,       MemberTypeInt, false);

        INIT_MEMBER_ARRAY_NAME_TO_ADDR(SectionGraphicsState,
                                       colorBuffer,
                                       MemberTypeColorBufferItem,
                                       MaxColorTargets,
                                       true);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        for (uint32_t i = 0; i < MaxColorTargets; ++i)
        {
            colorBuffer[i].GetSubState(m_state.colorBuffer[i]);
        }
        state = m_state;
    };

private:
    static const uint32_t  MemberCount = 22;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState               m_state;
    SectionColorBuffer     colorBuffer[MaxColorTargets]; // Color buffer
};

// =====================================================================================================================
// Represents the section compute state
class SectionComputeState : public Section
{
public:
    typedef ComputePipelineState SubState;

    SectionComputeState() :
        Section(m_addrTable, MemberCount, SectionTypeComputeState, nullptr)
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, deviceIndex,             MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, includeDisassembly,      MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionComputeState, autoLayoutDesc,          MemberTypeInt, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        state = m_state;
    };

private:
    static const uint32_t  MemberCount = 3;

    static StrToMemberAddr m_addrTable[MemberCount];

    SubState               m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input binding
class SectionVertexInputBinding : public Section
{
public:
    typedef VkVertexInputBindingDescription SubState;

    SectionVertexInputBinding() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "binding")
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, binding,   MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, stride,    MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputBinding, inputRate, MemberTypeEnum, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 3;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input attribute
class SectionVertexInputAttribute : public Section
{
public:
    typedef VkVertexInputAttributeDescription SubState;

    SectionVertexInputAttribute() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "binding")
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, location,  MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, binding,   MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, format,    MemberTypeEnum,   false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputAttribute, offset,    MemberTypeInt,    false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 4;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState m_state;
};

// =====================================================================================================================
// Represents the sub section vertex input divisor
class SectionVertexInputDivisor : public Section
{
public:
    typedef VkVertexInputBindingDivisorDescriptionEXT SubState;

    SectionVertexInputDivisor() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "divisor")
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputDivisor, binding,   MemberTypeInt,    false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionVertexInputDivisor, divisor,    MemberTypeInt,    false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 2;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState m_state;
};

// =====================================================================================================================
// Represents the section vertex input
class SectionVertexInput : public Section
{
public:
    typedef VkPipelineVertexInputStateCreateInfo SubState;

    SectionVertexInput() :
        Section(m_addrTable, MemberCount, SectionTypeVertexInputState, nullptr)
    {
        memset(&m_vkDivisorState, 0, sizeof(m_vkDivisorState));
        m_vkDivisorState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, attribute, MemberTypeVertexInputAttributeItem, true);
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, binding,   MemberTypeVertexInputBindingItem,   true);
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionVertexInput, divisor,   MemberTypeVertexInputDivisorItem,   true);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        m_vkBindings.resize(binding.size());
        m_vkAttributes.resize(attribute.size());
        m_vkDivisors.resize(divisor.size());

        for (uint32_t i = 0; i < attribute.size(); ++i)
        {
            attribute[i].GetSubState(m_vkAttributes[i]);
        }

        for (uint32_t i = 0; i < binding.size(); ++i)
        {
            binding[i].GetSubState(m_vkBindings[i]);
        }

        for (uint32_t i = 0; i < divisor.size(); ++i)
        {
            divisor[i].GetSubState(m_vkDivisors[i]);
        }

        state.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_vkAttributes.size());
        state.vertexBindingDescriptionCount = static_cast<uint32_t>(m_vkBindings.size());
        state.pVertexBindingDescriptions = state.vertexBindingDescriptionCount ? &m_vkBindings[0] : nullptr;
        state.pVertexAttributeDescriptions = state.vertexAttributeDescriptionCount ? &m_vkAttributes[0] : nullptr;
        if (m_vkDivisors.size() > 0)
        {
            state.pNext = &m_vkDivisorState;
            m_vkDivisorState.vertexBindingDivisorCount = m_vkDivisors.size();
            m_vkDivisorState.pVertexBindingDivisors = &m_vkDivisors[0];
        }
    };
private:
    static const uint32_t  MemberCount = 3;
    static StrToMemberAddr m_addrTable[MemberCount];

    std::vector<SectionVertexInputAttribute>        attribute;       // Vertex input atribute
    std::vector<SectionVertexInputBinding>          binding;         // Vertex input binding
    std::vector<SectionVertexInputDivisor>          divisor;         // Vertex input divisor
    std::vector<VkVertexInputBindingDescription>    m_vkBindings;    // Vulkan input binding
    std::vector<VkVertexInputAttributeDescription>  m_vkAttributes;  // Vulkan vertex input atribute
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> m_vkDivisors; // Vulkan vertex input divisor
    VkPipelineVertexInputDivisorStateCreateInfoEXT  m_vkDivisorState;    // Vulkan vertex input divisor state

};

// =====================================================================================================================
// Represents the sub section specialization constant map entry
class SectionSpecEntryItem : public Section
{
public:
    typedef VkSpecializationMapEntry SubState;

    SectionSpecEntryItem() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "mapEntry")
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, constantID,  MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, offset,      MemberTypeInt, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionSpecEntryItem, size,        MemberTypeInt, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)  { state = m_state; };

private:
    static const uint32_t  MemberCount = 3;
    static StrToMemberAddr m_addrTable[MemberCount];

    SubState m_state;
};

// =====================================================================================================================
// Represents the sub section specialization constant info
class SectionSpecInfo : public Section
{
public:
    typedef VkSpecializationInfo SubState;

   SectionSpecInfo() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "specConst")
    {
         intData = &m_bufMem;
         uintData = &m_bufMem;
         int64Data = &m_bufMem;
         uint64Data = &m_bufMem;
         floatData = &m_bufMem;
         doubleData = &m_bufMem;
         float16Data = &m_bufMem;
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionSpecInfo, mapEntry, MemberTypeSpecEntryItem, true);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, intData,       MemberTypeIArray,       false);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, uintData,      MemberTypeUArray,       false);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, int64Data,     MemberTypeI64Array,     false);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, uint64Data,    MemberTypeU64Array,     false);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, floatData,     MemberTypeFArray,       false);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, doubleData,    MemberTypeDArray,       false);
        INIT_MEMBER_NAME_TO_ADDR(SectionSpecInfo, float16Data,   MemberTypeF16Array,     false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        if (mapEntry.size())
        {
            state.mapEntryCount = static_cast<uint32_t>(mapEntry.size());
            m_vkMapEntries.resize(state.mapEntryCount);
            for (uint32_t i = 0; i < m_vkMapEntries.size(); ++i)
            {
                mapEntry[i].GetSubState(m_vkMapEntries[i]);
            }
            state.pMapEntries = &m_vkMapEntries[0];
            state.dataSize = m_bufMem.size();
            state.pData = &m_bufMem[0];
        }
        else
        {
            memset(&state, 0, sizeof(SubState));
        }
    }

private:
    static const uint32_t  MemberCount = 8;
    static StrToMemberAddr m_addrTable[MemberCount];

    std::vector<SectionSpecEntryItem> mapEntry;
    std::vector<uint8_t>*       intData;       // Contains int data of this buffer
    std::vector<uint8_t>*       uintData;      // Contains uint data of this buffer
    std::vector<uint8_t>*       int64Data;     // Contains int64 data of this buffer
    std::vector<uint8_t>*       uint64Data;    // Contains uint64 long data of this buffer
    std::vector<uint8_t>*       floatData;     // Contains float data of this buffer
    std::vector<uint8_t>*       doubleData;    // Contains double data of this buffer
    std::vector<uint8_t>*       float16Data;   // Contains float16 data of this buffer

    std::vector<uint8_t>                  m_bufMem;       // Buffer memory
    std::vector<VkSpecializationMapEntry> m_vkMapEntries; // Vulkan specialization map entry
};

// =====================================================================================================================
// Represents the sub section descriptor range value
class SectionDescriptorRangeValueItem : public Section
{
public:
    typedef DescriptorRangeValue SubState;

    SectionDescriptorRangeValueItem() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "descriptorRangeValue")
    {
        intData = &m_bufMem;
        uintData = &m_bufMem;
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, type,      MemberTypeEnum, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, set,       MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, binding,   MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem, arraySize, MemberTypeInt,  false);
        INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem,       uintData,  MemberTypeUArray, false);
        INIT_MEMBER_NAME_TO_ADDR(SectionDescriptorRangeValueItem,       intData,   MemberTypeIArray, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }
    void GetSubState(SubState& state)
    {
        state = m_state;
        state.pValue = m_bufMem.size() > 0 ? (const uint32_t *)(&m_bufMem[0]) : nullptr;
    };

private:
    static const uint32_t  MemberCount = 6;
    static StrToMemberAddr m_addrTable[MemberCount];

    std::vector<uint8_t>*     intData;
    std::vector<uint8_t>*     uintData;
    SubState                  m_state;
    std::vector<uint8_t>      m_bufMem;
};

// =====================================================================================================================
// Represents the sub section resource mapping node
class SectionResourceMappingNode : public Section
{
public:
    typedef ResourceMappingNode SubState;
    SectionResourceMappingNode() :
        Section(m_addrTable, MemberCount, SectionTypeUnset, "userDataNode")
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, type,           MemberTypeEnum, false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, sizeInDwords,   MemberTypeInt,  false);
        INIT_STATE_MEMBER_NAME_TO_ADDR(SectionResourceMappingNode, offsetInDwords, MemberTypeInt,  false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode,
                                               set,
                                               srdRange.set,
                                               MemberTypeInt,
                                               false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode,
                                               binding,
                                               srdRange.binding,
                                               MemberTypeInt,
                                               false);
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionResourceMappingNode,
                                          next,
                                          MemberTypeResourceMappingNode,
                                          true);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionResourceMappingNode,
                                               indirectUserDataCount,
                                               userDataPtr.sizeInDwords,
                                               MemberTypeInt,
                                               false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        if (m_state.type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            m_nextNodeBuf.resize(next.size());
            for (uint32_t i = 0; i < next.size(); ++i)
            {
                next[i].GetSubState(m_nextNodeBuf[i]);
            }
            m_state.tablePtr.pNext = &m_nextNodeBuf[0];
            m_state.tablePtr.nodeCount = static_cast<uint32_t>(m_nextNodeBuf.size());
        }
        state = m_state;
    };

private:
    static const uint32_t                   MemberCount = 7;
    static StrToMemberAddr                  m_addrTable[MemberCount];

    std::vector<SectionResourceMappingNode> next;          // Next rsource mapping node
    SubState                                m_state;
    std::vector<SubState>                   m_nextNodeBuf; // Contains next nodes
};

// =====================================================================================================================
// Represents the sub section pipeline shader info
class SectionShaderInfo : public Section
{
public:
    typedef PipelineShaderInfo SubState;
    SectionShaderInfo(SectionType sectionType) :
        Section(m_addrTable, MemberCount, sectionType, nullptr)
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    static void InitialAddrTable()
    {
        StrToMemberAddr* pTableItem = m_addrTable;
        INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, entryPoint,  MemberTypeString, false);
        INIT_MEMBER_NAME_TO_ADDR(SectionShaderInfo, specConst,   MemberTypeSpecInfo, true);
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, descriptorRangeValue, MemberTypeDescriptorRangeValue, true);
        INIT_MEMBER_DYNARRAY_NAME_TO_ADDR(SectionShaderInfo, userDataNode, MemberTypeResourceMappingNode, true);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, trapPresent, options.trapPresent, MemberTypeBool ,false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, debugMode, options.debugMode, MemberTypeBool, false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, enablePerformanceData, options.enablePerformanceData, MemberTypeBool, false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, allowReZ, options.allowReZ, MemberTypeBool, false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, vgprLimit, options.vgprLimit, MemberTypeInt, false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, sgprLimit, options.sgprLimit, MemberTypeInt, false);
        INIT_STATE_MEMBER_EXPLICITNAME_TO_ADDR(SectionShaderInfo, maxThreadGroupsPerComputeUnit, options.maxThreadGroupsPerComputeUnit, MemberTypeInt, false);
        VFX_ASSERT(pTableItem - &m_addrTable[0] <= MemberCount);
    }

    void GetSubState(SubState& state)
    {
        memset(&state, 0, sizeof(SubState));
        state.pEntryTarget = entryPoint.c_str();
        memcpy(&state.options, &m_state.options, sizeof(m_state.options));

        specConst.GetSubState(m_specializationInfo);
        state.pSpecializationInfo = &m_specializationInfo;

        if (descriptorRangeValue.size() > 0)
        {
            m_descriptorRangeValues.resize(descriptorRangeValue.size());
            for (uint32_t i = 0; i < descriptorRangeValue.size(); ++i)
            {
                descriptorRangeValue[i].GetSubState(m_descriptorRangeValues[i]);
            }
            state.descriptorRangeValueCount = static_cast<uint32_t>(descriptorRangeValue.size());
            state.pDescriptorRangeValues = &m_descriptorRangeValues[0];
        }

        if (userDataNode.size() > 0)
        {
            state.userDataNodeCount = static_cast<uint32_t>(userDataNode.size());
            m_userDataNodes.resize(state.userDataNodeCount);
            for (uint32_t i = 0; i< state.userDataNodeCount; ++i)
            {
                userDataNode[i].GetSubState(m_userDataNodes[i]);
            }
            state.pUserDataNodes = &m_userDataNodes[0];
        }
    };

private:
    static const uint32_t  MemberCount = 11;
    static StrToMemberAddr m_addrTable[MemberCount];
    SubState        m_state;
    SectionSpecInfo specConst;                                            // Specialization constant info
    std::string entryPoint;                                               // Entry point name
    std::vector<SectionDescriptorRangeValueItem> descriptorRangeValue;    // Contains descriptor range vuale
    std::vector<SectionResourceMappingNode> userDataNode;                 // Contains user data node

    VkSpecializationInfo              m_specializationInfo;
    std::vector<DescriptorRangeValue> m_descriptorRangeValues;
    std::vector<ResourceMappingNode>  m_userDataNodes;
};

}
