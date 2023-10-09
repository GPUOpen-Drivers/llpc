/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcUtil.cpp
 * @brief LLPC source file: contains implementation of LLPC internal types and utility functions
 * (independent of LLVM use).
 ***********************************************************************************************************************
 */
#include "llpcUtil.h"
#include "llpc.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "palPipelineAbi.h"
#include "spirvExt.h"
#include "vkgcElfReader.h"
#include <cassert>

using namespace llvm;
using namespace Vkgc;

#define DEBUG_TYPE "llpc-util"

namespace Llpc {

// =====================================================================================================================
// Gets the name string of shader stage.
//
// @param shaderStage : Shader stage
const char *getShaderStageName(ShaderStage shaderStage) {
  const char *name = nullptr;

  if (shaderStage == ShaderStageCopyShader)
    name = "copy";
  else if (shaderStage < ShaderStageCount) {
    static const char *ShaderStageNames[] = {"task",
                                             "vertex",
                                             "tessellation control",
                                             "tessellation evaluation",
                                             "geometry",
                                             "mesh",
                                             "fragment",
                                             "compute",
                                             "raygen",
                                             "intersect",
                                             "anyhit",
                                             "closesthit",
                                             "miss",
                                             "callable"};

    name = ShaderStageNames[static_cast<unsigned>(shaderStage)];
  } else
    name = "bad";

  return name;
}

// =====================================================================================================================
// Converts the SPIR-V execution model to the shader stage
//
// @param execModel : SPIR-V execution model
ShaderStage convertToShaderStage(unsigned execModel) {
  switch (execModel) {
  case spv::ExecutionModelTaskEXT:
    return ShaderStageTask;
  case spv::ExecutionModelVertex:
    return ShaderStageVertex;
  case spv::ExecutionModelTessellationControl:
    return ShaderStageTessControl;
  case spv::ExecutionModelTessellationEvaluation:
    return ShaderStageTessEval;
  case spv::ExecutionModelGeometry:
    return ShaderStageGeometry;
  case spv::ExecutionModelMeshEXT:
    return ShaderStageMesh;
  case spv::ExecutionModelFragment:
    return ShaderStageFragment;
  case spv::ExecutionModelGLCompute:
    return ShaderStageCompute;
  case spv::ExecutionModelCopyShader:
    return ShaderStageCopyShader;
  case spv::ExecutionModelRayGenerationKHR:
    return ShaderStageRayTracingRayGen;
  case spv::ExecutionModelIntersectionKHR:
    return ShaderStageRayTracingIntersect;
  case spv::ExecutionModelAnyHitKHR:
    return ShaderStageRayTracingAnyHit;
  case spv::ExecutionModelClosestHitKHR:
    return ShaderStageRayTracingClosestHit;
  case spv::ExecutionModelMissKHR:
    return ShaderStageRayTracingMiss;
  case spv::ExecutionModelCallableKHR:
    return ShaderStageRayTracingCallable;
  default:
    llvm_unreachable("Unknown execution model");
    return ShaderStageInvalid;
  }
}

// =====================================================================================================================
// Converts the shader stage to the SPIR-V execution model
//
// @param shaderStage : Shader stage
spv::ExecutionModel convertToExecModel(ShaderStage shaderStage) {
  switch (shaderStage) {
  case ShaderStageTask:
    return spv::ExecutionModelTaskEXT;
  case ShaderStageVertex:
    return spv::ExecutionModelVertex;
  case ShaderStageTessControl:
    return spv::ExecutionModelTessellationControl;
  case ShaderStageTessEval:
    return spv::ExecutionModelTessellationEvaluation;
  case ShaderStageGeometry:
    return spv::ExecutionModelGeometry;
  case ShaderStageMesh:
    return spv::ExecutionModelMeshEXT;
  case ShaderStageFragment:
    return spv::ExecutionModelFragment;
  case ShaderStageCompute:
    return spv::ExecutionModelGLCompute;
  case ShaderStageCopyShader:
    return spv::ExecutionModelCopyShader;
  case ShaderStageRayTracingRayGen:
    return spv::ExecutionModelRayGenerationKHR;
  case ShaderStageRayTracingIntersect:
    return spv::ExecutionModelIntersectionKHR;
  case ShaderStageRayTracingAnyHit:
    return spv::ExecutionModelAnyHitKHR;
  case ShaderStageRayTracingClosestHit:
    return spv::ExecutionModelClosestHitKHR;
  case ShaderStageRayTracingMiss:
    return spv::ExecutionModelMissKHR;
  case ShaderStageRayTracingCallable:
    return spv::ExecutionModelCallableKHR;
  default:
    llvm_unreachable("Unknown shader stage");
    return spv::ExecutionModelMax;
  }
}

// =====================================================================================================================
// Checks whether a specified shader stage is for ray tracing.
//
// @param stage : Shader stage to check
bool isRayTracingShaderStage(ShaderStage stage) {
  return stage >= ShaderStageRayTracingRayGen && stage <= ShaderStageRayTracingCallable;
}

// =====================================================================================================================
// Checks whether a specified shader stage mask contains ray tracing shader stages
//
// @param shageMask : Shader stage mask to check
bool hasRayTracingShaderStage(unsigned shageMask) {
  return (shageMask & ShaderStageAllRayTracingBit) != 0;
}

// =====================================================================================================================
// Returns true if shaderInfo has the information required to compile an unlinked shader of the given type.
//
// @param type : The unlinked shader type.
// @param shaderInfo : The shader information to check.
bool hasDataForUnlinkedShaderType(Vkgc::UnlinkedShaderStage type, ArrayRef<const PipelineShaderInfo *> shaderInfo) {
  switch (type) {
  case UnlinkedStageVertexProcess:
    return doesShaderStageExist(shaderInfo, ShaderStageVertex) || doesShaderStageExist(shaderInfo, ShaderStageMesh);
  case UnlinkedStageFragment:
    return doesShaderStageExist(shaderInfo, ShaderStageFragment);
  case UnlinkedStageCompute:
    return doesShaderStageExist(shaderInfo, ShaderStageCompute);
  default:
    return false;
  }
}

// =====================================================================================================================
// Returns the shader stage mask for all shader stages that can be part of the given unlinked shader type.
//
// @param type : The unlinked shader type.
unsigned getShaderStageMaskForType(Vkgc::UnlinkedShaderStage type) {
  switch (type) {
  case UnlinkedStageVertexProcess:
    return ShaderStageBit::ShaderStageTaskBit | ShaderStageBit::ShaderStageVertexBit |
           ShaderStageBit::ShaderStageTessControlBit | ShaderStageBit::ShaderStageTessEvalBit |
           ShaderStageBit::ShaderStageGeometryBit | ShaderStageBit::ShaderStageMeshBit;
  case UnlinkedStageFragment:
    return ShaderStageBit::ShaderStageFragmentBit;
  case UnlinkedStageCompute:
    return ShaderStageBit::ShaderStageComputeBit;
  default:
    return 0;
  }
}

// =====================================================================================================================
// Returns the name of the given unlinked shader type.
//
// @param type : The unlinked shader type.
const char *getUnlinkedShaderStageName(Vkgc::UnlinkedShaderStage type) {
  static const char *Names[] = {"vertex", "fragment", "compute", "unknown"};
  return Names[type];
}

// =====================================================================================================================
// Returns the name of the given part-pipeline stage.
//
// @param type : The part-pipeline stage type.
const char *getPartPipelineStageName(Vkgc::PartPipelineStage type) {
  switch (type) {
  case PartPipelineStageFragment:
    return "fragment";
  case PartPipelineStagePreRasterization:
    return "pre-rasterization";
  default:
    llvm_unreachable("Unknown part-pipeline stage.");
    return "unknown";
  }
}

// =====================================================================================================================
// Find userDataNode with specified set and binding. And return Node index.
//
// @param userDataNodes : ResourceMappingNode pointer
// @param nodeCount : User data node count
// @param set : Find same set in node array
// @param binding : Find same binding in node array
// @param [out] index : Return node position in node array
const ResourceMappingNode *findResourceNode(const ResourceMappingNode *userDataNodes, unsigned nodeCount, unsigned set,
                                            unsigned binding, unsigned *index) {
  const ResourceMappingNode *resourceNode = nullptr;

  for (unsigned j = 0; j < nodeCount; ++j) {
    const ResourceMappingNode *next = &userDataNodes[j];

    if (set == next->srdRange.set && binding == next->srdRange.binding) {
      resourceNode = next;
      *index = j;
      break;
    }
  }

  return resourceNode;
}

// =====================================================================================================================
// Find userDataNode with specified set and binding. And return Node index.
//
// @param userDataNodes : ResourceMappingRootNode pointer
// @param nodeCount : User data node count
// @param set : Find same set in node array
// @param binding : Find same binding in node array
// @param [out] index : Return node position in node array
// @returns : The Node index
const ResourceMappingNode *findResourceNode(const ResourceMappingRootNode *userDataNodes, unsigned nodeCount,
                                            unsigned set, unsigned binding, unsigned *index) {
  const ResourceMappingNode *resourceNode = nullptr;
  for (unsigned j = 0; j < nodeCount; ++j) {
    const ResourceMappingRootNode *next = &userDataNodes[j];
    if (next->node.type == ResourceMappingNodeType::DescriptorTableVaPtr) {
      resourceNode = findResourceNode(next->node.tablePtr.pNext, next->node.tablePtr.nodeCount, set, binding, index);
      if (resourceNode) {
        *index = j;
        break;
      }
    } else if (set == next->node.srdRange.set && binding == next->node.srdRange.binding) {
      resourceNode = &next->node;
      *index = j;
      break;
    }
  }

  return resourceNode;
}

// Returns the uniform constant map entry of the given location.
//
// @param context : The LLPC context
// @param stage   : The map entry stage
// @param loc     : The location of the finding uniform constant
Vkgc::UniformConstantMapEntry *getUniformConstantEntryByLocation(const Llpc::Context *context, Vkgc::ShaderStage stage,
                                                                 unsigned loc) {
  Vkgc::UniformConstantMap *accessedUniformMap = nullptr;
  if (context->getPipelineType() == PipelineType::Graphics) {
    auto *buildInfo = static_cast<const Vkgc::GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    // Find the uniform constant map to use.
    for (unsigned s = 0; s < buildInfo->numUniformConstantMaps; s++) {
      if (buildInfo->ppUniformMaps[s] != nullptr &&
          isShaderStageInMask(stage, buildInfo->ppUniformMaps[s]->visibility)) {
        accessedUniformMap = buildInfo->ppUniformMaps[s];
        break;
      }
    }
  } else {
    assert(context->getPipelineType() == PipelineType::Compute);
    auto *buildInfo = static_cast<const Vkgc::ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    accessedUniformMap = buildInfo->pUniformMap;
  }

  if (accessedUniformMap != nullptr) {
    auto *uniforms = accessedUniformMap->pUniforms;
    unsigned numUniform = accessedUniformMap->numUniformConstants;

    UniformConstantMapEntry *locationFound =
        std::find_if(uniforms, uniforms + numUniform,
                     [&](const Vkgc::UniformConstantMapEntry &item) { return item.location == loc; });

    return locationFound != uniforms + numUniform ? locationFound : nullptr;
  }
  return nullptr;
}

} // namespace Llpc
