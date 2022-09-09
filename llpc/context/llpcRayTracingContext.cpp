/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcRayTracingContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::RayTracingContext.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-ray-tracing-context"

#include "llpcRayTracingContext.h"
#include "SPIRVInternal.h"
#include "lgc/Builder.h"

using namespace llvm;
namespace Llpc {
// =====================================================================================================================
//
// @param gfxIP : Graphics Ip version info
// @param pipelineInfo : Ray tracing pipeline build info
// @param traceRayShaderInfo : Trace ray shader info
// @param pipelineHash : Pipeline hash code
// @param cacheHash : Cache hash code
RayTracingContext::RayTracingContext(GfxIpVersion gfxIP, const RayTracingPipelineBuildInfo *pipelineInfo,
                                     const PipelineShaderInfo *traceRayShaderInfo, MetroHash::Hash *pipelineHash,
                                     MetroHash::Hash *cacheHash, unsigned indirectStageMask)
    : PipelineContext(gfxIP, pipelineHash, cacheHash, &pipelineInfo->rtState), m_pipelineInfo(pipelineInfo),
      m_traceRayShaderInfo(traceRayShaderInfo), m_linked(false), m_indirectStageMask(indirectStageMask),
      m_entryName(""), m_payloadMaxSize(pipelineInfo->payloadSizeMaxInLib), m_callableDataMaxSize(0),
      m_attributeDataMaxSize(pipelineInfo->attributeSizeMaxInLib) {
  m_resourceMapping = pipelineInfo->resourceMapping;
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
//
// @param shaderStage : Shader stage
const PipelineShaderInfo *RayTracingContext::getPipelineShaderInfo(ShaderStage shaderStage) const {
  return m_traceRayShaderInfo;
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
unsigned RayTracingContext::getShaderStageMask() const {
  if (m_linked)
    return ShaderStageComputeBit;
  else {
    unsigned shaderMask = 0;
    for (unsigned i = 0; i < m_pipelineInfo->shaderCount; ++i) {
      shaderMask |= shaderStageToMask(m_pipelineInfo->pShaders[i].entryStage);
    }

    shaderMask |= ShaderStageComputeBit;
    return shaderMask;
  }
}

// =====================================================================================================================
// Gets the count of active shader stages
unsigned RayTracingContext::getActiveShaderStageCount() const {
  return m_pipelineInfo->shaderCount;
}

// =====================================================================================================================
// Collect built-in
//
// @param builtIn : Built-in ID
// @param hitAttribute : whether to collect hitAttribute
void RayTracingContext::collectBuiltIn(unsigned builtIn) {
  if (isRayTracingBuiltIn(builtIn))
    m_builtIns.insert(builtIn);
}

// =====================================================================================================================
// Collect payload information
//
// @param type : Payload type
// @param dataLayout : Payload module data layout
void RayTracingContext::collectPayloadSize(llvm::Type *type, const DataLayout &dataLayout) {
  unsigned payloadTypeSize = alignTo(dataLayout.getTypeAllocSize(type), 4);
  m_payloadMaxSize = std::max(m_payloadMaxSize, payloadTypeSize);
}

// =====================================================================================================================
// Collect callable data information
//
// @param type : Callable data type
// @param dataLayout : module data layout
void RayTracingContext::collectCallableDataSize(llvm::Type *type, const DataLayout &dataLayout) {
  unsigned dataTypeSize = alignTo(dataLayout.getTypeAllocSize(type), 4);
  m_callableDataMaxSize = std::max(m_callableDataMaxSize, dataTypeSize);
}

// =====================================================================================================================
// Collect callable data information
//
// @param type : Callable data type
// @param dataLayout : module data layout
void RayTracingContext::collectAttributeDataSize(llvm::Type *type, const DataLayout &dataLayout) {
  unsigned dataTypeSize = alignTo(dataLayout.getTypeAllocSize(type), 4);
  m_attributeDataMaxSize = std::max(m_attributeDataMaxSize, dataTypeSize);
}
// =====================================================================================================================
// Get payload information
//
// @param builder : LGC builder
llvm::Type *RayTracingContext::getPayloadType(lgc::Builder *builder) {
  return ArrayType::get(builder->getInt32Ty(), m_payloadMaxSize / 4);
}

// =====================================================================================================================
// Get callable information
//
// @param builder : LGC builder
llvm::Type *RayTracingContext::getCallableDataType(lgc::Builder *builder) {
  return ArrayType::get(builder->getInt32Ty(), m_callableDataMaxSize / 4);
}

// =====================================================================================================================
// Get attribute information
//
// @param builder : LGC builder
unsigned RayTracingContext::getAttributeDataSize() {
  return m_attributeDataMaxSize / 4;
}

// =====================================================================================================================
// If the builtIn is builtIn used in raytracing
//
// @param builtIn : Built-in ID
bool RayTracingContext::isRayTracingBuiltIn(unsigned builtIn) {
  switch (builtIn) {
  case BuiltInPrimitiveId:
  case BuiltInHitKindKHR:
  case BuiltInIncomingRayFlagsKHR:
  case BuiltInInstanceCustomIndexKHR:
  case BuiltInInstanceId:
  case BuiltInRayTminKHR:
  case BuiltInWorldRayOriginKHR:
  case BuiltInWorldRayDirectionKHR:
  case BuiltInRayGeometryIndexKHR:
  case BuiltInHitTNV:
  case BuiltInRayTmaxKHR:
  case BuiltInObjectToWorldKHR:
  case BuiltInWorldToObjectKHR:
  case BuiltInObjectRayOriginKHR:
  case BuiltInObjectRayDirectionKHR:
  case BuiltInCullMaskKHR:
    return true;
  default:
    return false;
  }
}

// =====================================================================================================================
// Get the shader stage module IDs
//
// @param stage : Shader stage
// @param intersectId : Module ID of intersect shader
// @param [out] moduleIds : Module IDs for the shader stage
void RayTracingContext::getStageModuleIds(ShaderStage stage, unsigned intersectId, std::vector<unsigned> &moduleIds) {
  moduleIds.clear();
  for (unsigned i = 0; i < m_pipelineInfo->shaderCount; ++i) {
    if (m_pipelineInfo->pShaders[i].entryStage != stage)
      continue;

    if (intersectId == InvalidShaderId)
      moduleIds.push_back(getModuleIdByIndex(i));
    else if (stage == ShaderStageRayTracingAnyHit) {
      for (unsigned j = 0; j < m_pipelineInfo->shaderGroupCount; ++j) {
        auto shaderGroup = &(m_pipelineInfo->pShaderGroups[j]);
        if (shaderGroup->anyHitShader != i)
          continue;

        if (shaderGroup->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR &&
            intersectId == TriangleHitGroup) {
          moduleIds.push_back(getModuleIdByIndex(i));
          break;
        } else if (shaderGroup->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR &&
                   getModuleIdByIndex(shaderGroup->intersectionShader) == intersectId) {
          moduleIds.push_back(getModuleIdByIndex(i));
          break;
        }
      }
    }
  }
}

// =====================================================================================================================
// Gets subgroup size usage
unsigned RayTracingContext::getSubgroupSizeUsage() const {
  for (uint32_t i = 0; i < m_pipelineInfo->shaderCount; ++i) {
    const auto &shaderInfo = m_pipelineInfo->pShaders[i];
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo.pModuleData);
    if (!moduleData->usage.useSubgroupSize)
      continue;
    return unsigned(-1);
  }
  return 0;
}

} // namespace Llpc
