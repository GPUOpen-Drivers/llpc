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
* @file  vfxSection.cpp
* @brief Contains implementation of class Section and derived classes
***********************************************************************************************************************
*/

#include "vfxSection.h"
#include "vfxEnumsConverter.h"
#include "vfxParser.h"
#include <inttypes.h>

#ifndef VFX_DISABLE_SPVGEN
#include "spvgen.h"
#endif

namespace Vfx {

// =====================================================================================================================
// Static variables in class Section and derived class
std::map<std::string, SectionInfo> Section::m_sectionInfo;

#ifndef VFX_DISABLE_SPVGEN
// =====================================================================================================================
// A helper method to convert ShaderStage enumerant to corresponding SpvGenStage enumerant.
//
// @param shaderStage : Input ShaderStage enumerant
static SpvGenStage shaderStageToSpvGenStage(ShaderStage shaderStage) {
  switch (shaderStage) {
  case ShaderStage::ShaderStageTask:
    return SpvGenStageTask;
  case ShaderStage::ShaderStageVertex:
    return SpvGenStageVertex;
  case ShaderStage::ShaderStageTessControl:
    return SpvGenStageTessControl;
  case ShaderStage::ShaderStageTessEval:
    return SpvGenStageTessEvaluation;
  case ShaderStage::ShaderStageGeometry:
    return SpvGenStageGeometry;
  case ShaderStage::ShaderStageMesh:
    return SpvGenStageMesh;
  case ShaderStage::ShaderStageFragment:
    return SpvGenStageFragment;
  case ShaderStage::ShaderStageCompute:
    return SpvGenStageCompute;
#if VKI_RAY_TRACING
  case ShaderStage::ShaderStageRayTracingRayGen:
    return SpvGenStageRayTracingRayGen;
  case ShaderStage::ShaderStageRayTracingIntersect:
    return SpvGenStageRayTracingIntersect;
  case ShaderStage::ShaderStageRayTracingAnyHit:
    return SpvGenStageRayTracingAnyHit;
  case ShaderStage::ShaderStageRayTracingClosestHit:
    return SpvGenStageRayTracingClosestHit;
  case ShaderStage::ShaderStageRayTracingMiss:
    return SpvGenStageRayTracingMiss;
  case ShaderStage::ShaderStageRayTracingCallable:
    return SpvGenStageRayTracingCallable;
#endif
  default:
    VFX_NEVER_CALLED();
    return SpvGenStageInvalid;
  }
}
#endif

// =====================================================================================================================
// Dummy class used to initialize all static variables
class ParserInit {
public:
  ParserInit() {
    initEnumMap();

    Section::initSectionInfo();
  };
};

static ParserInit Init;

// =====================================================================================================================
// Constructs an Section object.
//
// @param addrTable : Table to map member name to member address
// @param tableSize : Size of above table
// @param sectionType : Section type
// @param sectionName : Name of this section.
Section::Section(StrToMemberAddrArrayRef addrTable, SectionType sectionType, const char *sectionName)
    : m_sectionType(sectionType), m_sectionName(sectionName), m_lineNum(0), m_memberTable(addrTable.data),
      m_tableSize(addrTable.size), m_isActive(false){

                                   };

// =====================================================================================================================
// Initializes static variable m_sectionInfo
void Section::initSectionInfo() {
  // Shader source sections
  INIT_SECTION_INFO("TaskShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VertexShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TessControlShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TessEvalShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GeometryShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FragmentShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("ComputeShaderGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageCompute)

  INIT_SECTION_INFO("TaskShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VertexShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TessControlShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TessEvalShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GeometryShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FragmentShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("ComputeShaderSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageCompute)

  INIT_SECTION_INFO("TaskGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callGlsl", SectionTypeShader, Glsl, ShaderStage::ShaderStageRayTracingCallable)
#endif

  INIT_SECTION_INFO("TaskSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callSpirv", SectionTypeShader, SpirvAsm, ShaderStage::ShaderStageRayTracingCallable)
#endif

  // Shader source file section
  INIT_SECTION_INFO("TaskGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callGlslFile", SectionTypeShader, GlslFile, ShaderStage::ShaderStageRayTracingCallable)
#endif

  INIT_SECTION_INFO("TaskSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callSpvFile", SectionTypeShader, SpirvFile, ShaderStage::ShaderStageRayTracingCallable)
#endif

  INIT_SECTION_INFO("TaskSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callSpvasmFile", SectionTypeShader, SpirvAsmFile, ShaderStage::ShaderStageRayTracingCallable)
#endif

  INIT_SECTION_INFO("TaskHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callHlsl", SectionTypeShader, Hlsl, ShaderStage::ShaderStageRayTracingCallable)
#endif

  INIT_SECTION_INFO("TaskHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageTask)
  INIT_SECTION_INFO("VsHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageVertex)
  INIT_SECTION_INFO("TcsHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageTessControl)
  INIT_SECTION_INFO("TesHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageTessEval)
  INIT_SECTION_INFO("GsHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageGeometry)
  INIT_SECTION_INFO("MeshHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageMesh)
  INIT_SECTION_INFO("FsHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageFragment)
  INIT_SECTION_INFO("CsHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageCompute)
#if VKI_RAY_TRACING
  INIT_SECTION_INFO("rgenHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageRayTracingRayGen)
  INIT_SECTION_INFO("sectHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageRayTracingIntersect)
  INIT_SECTION_INFO("ahitHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageRayTracingAnyHit)
  INIT_SECTION_INFO("chitHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageRayTracingClosestHit)
  INIT_SECTION_INFO("missHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageRayTracingMiss)
  INIT_SECTION_INFO("callHlslFile", SectionTypeShader, HlslFile, ShaderStage::ShaderStageRayTracingCallable)
#endif
  INIT_SECTION_INFO("Version", SectionTypeVersion, 0)
  INIT_SECTION_INFO("CompileLog", SectionTypeCompileLog, 0)
}

// =====================================================================================================================
// Gets data type of a member.
//
// @param lineNum : Line No.
// @param memberName : Member string name
// @param [out] valueType : Member data type.
// @param [out] errorMsg : Error message
bool Section::getMemberType(unsigned lineNum, const char *memberName, MemberType *valueType, std::string *errorMsg) {
  bool result = false;
  for (unsigned i = 0; i < m_tableSize; ++i) {
    if (m_memberTable[i].memberName && strcmp(memberName, m_memberTable[i].memberName) == 0) {
      result = true;

      if (valueType)
        *valueType = m_memberTable[i].memberType;

      break;
    }
  }

  if (!result) {
    PARSE_WARNING(*errorMsg, lineNum, "Invalid member name: %s", memberName);
  }

  return result;
}

// =====================================================================================================================
// Is this member a section object.
//
// @param lineNum : Line number
// @param memberName : Member name
// @param [out] output : Is this member a section object
// @param [out] type : Object type
// @param [out] errorMsg : Error message
bool Section::isSection(unsigned lineNum, const char *memberName, bool *output, MemberType *type,
                        std::string *errorMsg) {
  bool result = false;

  for (unsigned i = 0; i < m_tableSize; ++i) {
    if (m_memberTable[i].memberName && strcmp(memberName, m_memberTable[i].memberName) == 0) {
      result = true;
      if (output)
        *output = m_memberTable[i].isSection;

      if (type)
        *type = m_memberTable[i].memberType;
      break;
    }
  }

  if (!result) {
    PARSE_WARNING(*errorMsg, lineNum, "Invalid member name: %s", memberName);
  }

  return result;
}

// =====================================================================================================================
// Prints all data in this object, for debug purpose.
//
// @param level : Nest level from the base object
void Section::printSelf(Document *pDoc, unsigned level) {
  if (m_isActive) {
    for (unsigned l = 0; l < level; ++l) {
      printf("\t");
    }
    printf("[%s]\n", m_sectionName);
    for (unsigned i = 0; i < m_tableSize; ++i) {
      if (m_memberTable[i].memberName)
        continue;
      for (unsigned arrayIndex = 0; arrayIndex < m_memberTable[i].arrayMaxSize; ++arrayIndex) {
        if (m_memberTable[i].isSection) {
          Section *subObj;
          std::string dummyMsg;
          if (pDoc->getPtrOfSubSection(this, 0, m_memberTable[i].memberName, m_memberTable[i].memberType, false,
                                       arrayIndex, &subObj, &dummyMsg)) {
            if (subObj->m_isActive)
              subObj->printSelf(pDoc, level + 1);
          }
        } else {
          for (unsigned l = 0; l < level; ++l) {
            printf("\t");
          }
          int tempValue = *(((int *)(getMemberAddr(i))) + arrayIndex);
          if (static_cast<unsigned>(tempValue) != VfxInvalidValue) {
            switch (m_memberTable[i].memberType) {
            case MemberTypeEnum:
            case MemberTypeInt: {
              printf("%s = %d\n", m_memberTable[i].memberName, *(((int *)(getMemberAddr(i))) + arrayIndex));
              break;
            }
            case MemberTypeBool: {
              printf("%s = %d\n", m_memberTable[i].memberName, *(((bool *)(getMemberAddr(i))) + arrayIndex));
              break;
            }
            case MemberTypeFloat: {
              printf("%s = %.3f\n", m_memberTable[i].memberName, *(((float *)(getMemberAddr(i))) + arrayIndex));
              break;
            }
            case MemberTypeFloat16: {
              float v = (((Float16 *)(getMemberAddr(i))) + arrayIndex)->GetValue();
              printf("%s = %.3fhf\n", m_memberTable[i].memberName, v);
              break;
            }
            case MemberTypeDouble: {
              printf("%s = %.3f\n", m_memberTable[i].memberName, *(((double *)(getMemberAddr(i))) + arrayIndex));
              break;
            }
            case MemberTypeIVec4: {
              IUFValue *iufValue = static_cast<IUFValue *>(getMemberAddr(i));
              iufValue += arrayIndex;

              if (!iufValue->props.isDouble && !iufValue->props.isFloat) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned j = 0; j < iufValue->props.length; ++j) {
                  if (iufValue->props.isHex)
                    printf(" 0x%x", iufValue->iVec4[j]);
                  else
                    printf(" %d", iufValue->iVec4[j]);
                }
                printf("\n");
              }
              break;
            }
            case MemberTypeI64Vec2: {
              IUFValue *iufValue = static_cast<IUFValue *>(getMemberAddr(i));
              iufValue += arrayIndex;

              if (!iufValue->props.isDouble && !iufValue->props.isFloat) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned j = 0; j < iufValue->props.length; ++j) {
                  if (iufValue->props.isHex)
                    printf(" 0x%" PRIx64, iufValue->i64Vec2[j]);
                  else
                    printf(" %" PRId64, iufValue->i64Vec2[j]);
                }
                printf("\n");
              }
              break;
            }
            case MemberTypeFVec4: {
              IUFValue *iufValue = static_cast<IUFValue *>(getMemberAddr(i));
              iufValue += arrayIndex;

              if (!iufValue->props.isDouble && iufValue->props.isFloat) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned j = 0; j < iufValue->props.length; ++j)
                  printf(" %.3f", iufValue->fVec4[j]);
                printf("\n");
              }
              break;
            }
            case MemberTypeF16Vec4: {
              IUFValue *iufValue = static_cast<IUFValue *>(getMemberAddr(i));
              iufValue += arrayIndex;

              if (!iufValue->props.isDouble && iufValue->props.isFloat16) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned j = 0; j < iufValue->props.length; ++j)
                  printf(" %.3fhf", iufValue->f16Vec4[j].GetValue());
                printf("\n");
              }
              break;
            }
            case MemberTypeDVec2: {
              IUFValue *iufValue = static_cast<IUFValue *>(getMemberAddr(i));
              iufValue += arrayIndex;

              if (iufValue->props.isDouble && !iufValue->props.isFloat) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned j = 0; j < iufValue->props.length; ++j)
                  printf(" %.3f", iufValue->dVec2[j]);
                printf("\n");
              }
              break;
            }
            case MemberTypeIArray:
            case MemberTypeUArray: {
              std::vector<unsigned> *intBufData = *static_cast<std::vector<unsigned> **>(getMemberAddr(i));

              if (intBufData->size() > 0) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned i = 0; i < intBufData->size(); ++i)
                  printf(" 0x%x", (*intBufData)[i]);
                printf("\n");
              }

              break;
            }
            case MemberTypeI64Array:
            case MemberTypeU64Array: {
              std::vector<unsigned> *int64BufData = *static_cast<std::vector<unsigned> **>(getMemberAddr(i));
              union {
                uint64_t u64Val;
                unsigned uVal[2];
              };

              if (int64BufData->size() > 0) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned i = 0; i < int64BufData->size(); i += 2) {
                  uVal[0] = (*int64BufData)[i];
                  uVal[1] = (*int64BufData)[i + 1];
                  printf(" 0x%" PRIx64, u64Val);
                }
                printf("\n");
              }

              break;
            }
            case MemberTypeFArray: {
              std::vector<unsigned> *floatBufData = *static_cast<std::vector<unsigned> **>(getMemberAddr(i));
              union {
                float fVal;
                unsigned uVal;
              };
              if (floatBufData->size() > 0) {
                printf("%s =", m_memberTable[i].memberName);

                for (unsigned i = 0; i < floatBufData->size(); ++i) {
                  uVal = (*floatBufData)[i];
                  printf(" %.3f", fVal);
                }
                printf("\n");
              }
              break;
            }
            case MemberTypeF16Array: {
              std::vector<uint16_t> *float16BufData = *static_cast<std::vector<uint16_t> **>(getMemberAddr(i));
              union {
                Float16 f16Val;
                uint16_t uVal;
              };
              if (float16BufData->size() > 0) {
                printf("%s =", m_memberTable[i].memberName);

                for (unsigned i = 0; i < float16BufData->size(); ++i) {
                  uVal = (*float16BufData)[i];
                  printf(" %.3f", f16Val.GetValue());
                }
                printf("\n");
              }
              break;
            }
            case MemberTypeDArray: {
              std::vector<unsigned> *doubleBufData = *static_cast<std::vector<unsigned> **>(getMemberAddr(i));
              union {
                double dVal;
                unsigned uVal[2];
              };

              if (doubleBufData->size() > 1) {
                printf("%s =", m_memberTable[i].memberName);
                for (unsigned i = 0; i < doubleBufData->size() - 1; i += 2) {
                  uVal[0] = (*doubleBufData)[i];
                  uVal[1] = (*doubleBufData)[i + 1];
                  printf(" %.3f", dVal);
                }
                printf("\n");
              }
              break;
            }
            case MemberTypeString: {
              std::string *str = static_cast<std::string *>(getMemberAddr(i));
              printf("%s = %s\n", m_memberTable[i].memberName, str->c_str());
              break;
            }
            default:;
            }
          }
        }
      }
    }
    putchar('\n');
  }
}

// =====================================================================================================================
// Gets section type according to section name
//
// @param sectionName : Section name
SectionType Section::getSectionType(const char *sectionName) {
  SectionType type = SectionTypeUnset;
  auto it = m_sectionInfo.find(sectionName);
  if (it != m_sectionInfo.end())
    type = it->second.type;
  return type;
}

// =====================================================================================================================
// Reads whole file content
// NOTE: The input file name is  from class member "fileName" and read result is stored in shaderSource or m_spvBin
// according to file type
//
// @param docFilename : File name of parent document
// @param fileName : File name
// @param isBinary : Whether file is SPIRV binary file
// @param [out] binaryData : Binary data
// @param [out] textData : Text data
// @param [out] errorMsg : Error message
bool Section::readFile(const std::string &docFilename, const std::string &fileName, bool isBinary,
                       std::vector<uint8_t> *binaryData, std::string *textData, std::string *errorMsg) {
  bool result = true;

  // Prepend directory from "docFilename" to the given filename.
  std::string path;
  auto separatorIndex = docFilename.find_last_of("/\\");
  if (separatorIndex != std::string::npos)
    path = docFilename.substr(0, separatorIndex + 1);
  path += fileName;

  // Open file
  FILE *inFile = fopen(path.c_str(), isBinary ? "rb" : "r");
  if (!inFile) {
    PARSE_ERROR(*errorMsg, 0, "Fails to open input file: %s\n", path.c_str());
    return false;
  }

  // Get file size
  fseek(inFile, 0, SEEK_END);
  size_t fileSize = ftell(inFile);
  fseek(inFile, 0, SEEK_SET);

  // Allocate temp buffer and read file
  char *data = new char[fileSize + 1];
  VFX_ASSERT(data);
  memset(data, 0, fileSize + 1);
  size_t readSize = fread(data, 1, fileSize, inFile);

  // Copy to destination
  if (isBinary) {
    (*binaryData).resize(readSize);
    memcpy(&(*binaryData)[0], data, readSize);
  } else
    *textData = data;

  // Clean up
  delete[] data;
  fclose(inFile);

  return result;
}

// =====================================================================================================================
// Compiles GLSL source text file (input) to SPIR-V binary file (output).
//
// @param shaderInfo : Shader info section
// @param [out] errorMsg : Error message
bool SectionShader::compileGlsl(const char *entryPoint, std::string *errorMsg) {
  bool result = true;
#ifndef VFX_DISABLE_SPVGEN
  int sourceStringCount = 1;
  const char *const *sourceList[1] = {};
  const char *const *fileList[1] = {};

  const char *glslText = m_shaderSource.c_str();
  const char *fileName = m_fileName.c_str();
  SpvGenStage stage = shaderStageToSpvGenStage(m_shaderStage);
  void *program = nullptr;
  const char *log = nullptr;

  if (!InitSpvGen()) {
    PARSE_ERROR(*errorMsg, m_lineNum, "Failed to load SPVGEN: cannot compile GLSL\n");
    return false;
  }

  sourceList[0] = &glslText;
  fileList[0] = &fileName;
  int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules | SpvGenOptionDebug;
  if (m_shaderType == Hlsl || m_shaderType == HlslFile)
    compileOption |= SpvGenOptionReadHlsl;
  bool compileResult = spvCompileAndLinkProgramEx(1, &stage, &sourceStringCount, sourceList, fileList, &entryPoint,
                                                  &program, &log, compileOption);

  if (compileResult) {
    const unsigned *spvBin = nullptr;
    unsigned binSize = spvGetSpirvBinaryFromProgram(program, 0, &spvBin);
    m_spvBin.resize(binSize);
    memcpy(&m_spvBin[0], spvBin, binSize);
  } else {
    PARSE_ERROR(*errorMsg, m_lineNum, "Fail to compile GLSL\n%s\n", log);
    result = false;
  }

  if (program)
    spvDestroyProgram(program);
#else
  m_spvBin.resize(m_shaderSource.length() + 1);
  memcpy(m_spvBin.data(), m_shaderSource.c_str(), m_shaderSource.length() + 1);
#endif
  return result;
}

// =====================================================================================================================
// Assemble Spirv assemble code
//
// @param [out] errorMsg : Error message
bool SectionShader::assembleSpirv(std::string *errorMsg) {
  bool result = true;
#ifndef VFX_DISABLE_SPVGEN
  const char *text = m_shaderSource.c_str();

  if (!InitSpvGen()) {
    PARSE_ERROR(*errorMsg, m_lineNum, "Failed to load SPVGEN: cannot assemble SPIR-V assembler source\n");
    return false;
  }

  const char *log = nullptr;
  unsigned bufSize = static_cast<unsigned>(m_shaderSource.size()) * 4 + 1024;
  unsigned *buffer = new unsigned[bufSize / 4];

  int binSize = spvAssembleSpirv(text, bufSize, buffer, &log);

  if (binSize > 0) {
    m_spvBin.resize(binSize);
    memcpy(&m_spvBin[0], buffer, binSize);
  } else {
    PARSE_ERROR(*errorMsg, m_lineNum, "Fail to Assemble SPIRV\n%s\n", log);
    result = false;
  }

  delete[] buffer;
#else
  m_spvBin.resize(m_shaderSource.length() + 1);
  memcpy(m_spvBin.data(), m_shaderSource.c_str(), m_shaderSource.length() + 1);
#endif
  return result;
}

// =====================================================================================================================
// Returns true if this section contains shader source
bool SectionShader::isShaderSourceSection() {
  bool ret = false;
  switch (m_shaderType) {
  case Glsl:
  case Hlsl:
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
//
// @param docFilename : File name of parent document
// @param shaderInfo : Shader info sections
// @param [out] errorMsg : Error message
bool SectionShader::compileShader(const std::string &docFilename, const char *entryPoint, std::string *errorMsg) {
  bool result = false;
  switch (m_shaderType) {
  case Glsl:
  case Hlsl: {
    result = compileGlsl(entryPoint, errorMsg);
    break;
  }
  case GlslFile:
  case HlslFile: {
    result = readFile(docFilename, m_fileName, false, &m_spvBin, &m_shaderSource, errorMsg);
    if (result)
      compileGlsl(entryPoint, errorMsg);
    break;
  }
  case SpirvAsm: {
    result = assembleSpirv(errorMsg);
    break;
  }
  case SpirvAsmFile: {
    result = readFile(docFilename, m_fileName, false, &m_spvBin, &m_shaderSource, errorMsg);
    if (result)
      assembleSpirv(errorMsg);
    break;
  }
  case SpirvFile: {
    result = readFile(docFilename, m_fileName, true, &m_spvBin, &m_shaderSource, errorMsg);
    break;
  }
  default: {
    VFX_NEVER_CALLED();
    break;
  }
  }
  return result;
}

void SectionShader::getSubState(SectionShader::SubState &state) {
  state.dataSize = static_cast<unsigned>(m_spvBin.size());
  state.pData = state.dataSize > 0 ? &m_spvBin[0] : nullptr;
  state.stage = m_shaderStage;
}

} // namespace Vfx
