/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcGraphicsContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::GraphicsContext.
 ***********************************************************************************************************************
 */
#include "llpcGraphicsContext.h"
#include "SPIRVInternal.h"
#include "llpcCompiler.h"
#include "lgc/Builder.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "llpc-graphics-context"

using namespace llvm;
using namespace SPIRV;

namespace Llpc {

// =====================================================================================================================
//
// @param gfxIp : Graphics Ip version info
// @param pipelineInfo : Graphics pipeline build info
// @param pipelineHash : Pipeline hash code
// @param cacheHash : Cache hash code
GraphicsContext::GraphicsContext(GfxIpVersion gfxIp, const GraphicsPipelineBuildInfo *pipelineInfo,
                                 MetroHash::Hash *pipelineHash, MetroHash::Hash *cacheHash)
    : PipelineContext(gfxIp, pipelineHash, cacheHash), m_pipelineInfo(pipelineInfo), m_stageMask(0),
      m_activeStageCount(0), m_gsOnChip(false) {
  setUnlinked(pipelineInfo->unlinked);
  const PipelineShaderInfo *shaderInfo[ShaderStageGfxCount] = {
      &pipelineInfo->vs, &pipelineInfo->tcs, &pipelineInfo->tes, &pipelineInfo->gs, &pipelineInfo->fs,
  };

  for (unsigned stage = 0; stage < ShaderStageGfxCount; ++stage) {
    if (shaderInfo[stage]->pModuleData) {
      m_stageMask |= shaderStageToMask(static_cast<ShaderStage>(stage));
      ++m_activeStageCount;

      if (stage == ShaderStageGeometry) {
        m_stageMask |= shaderStageToMask(ShaderStageCopyShader);
        ++m_activeStageCount;
      }
    }
  }

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 41
  m_resourceMapping = pipelineInfo->resourceMapping;
#else
  mergeResourceMappingData();
#endif
}

// =====================================================================================================================
GraphicsContext::~GraphicsContext() {
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
//
// @param shaderStage : Shader stage
const PipelineShaderInfo *GraphicsContext::getPipelineShaderInfo(ShaderStage shaderStage) const {
  if (shaderStage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    shaderStage = ShaderStageGeometry;
  }

  assert(shaderStage < ShaderStageGfxCount);

  const PipelineShaderInfo *shaderInfo = nullptr;
  switch (shaderStage) {
  case Llpc::ShaderStageVertex:
    shaderInfo = &m_pipelineInfo->vs;
    break;
  case Llpc::ShaderStageTessControl:
    shaderInfo = &m_pipelineInfo->tcs;
    break;
  case Llpc::ShaderStageTessEval:
    shaderInfo = &m_pipelineInfo->tes;
    break;
  case Llpc::ShaderStageGeometry:
    shaderInfo = &m_pipelineInfo->gs;
    break;
  case Llpc::ShaderStageFragment:
    shaderInfo = &m_pipelineInfo->fs;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return shaderInfo;
}

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 41
// =====================================================================================================================
// Merges per-shader resource mapping structs into a single per-pipeline resource mapping struct
void GraphicsContext::mergeResourceMappingData() {
  unsigned stageMask = getShaderStageMask();
  SmallVector<ResourceMappingNode, 8> allNodes;

  // Collect user data nodes from all shader stages into one big table.
  for (unsigned stage = 0; stage < ShaderStageNativeStageCount; ++stage) {
    if ((stageMask >> stage) & 1) {
      auto shaderInfo = getPipelineShaderInfo(ShaderStage(stage));
      for (const ResourceMappingNode &node :
           ArrayRef<ResourceMappingNode>(shaderInfo->pUserDataNodes, shaderInfo->userDataNodeCount))
        allNodes.push_back(node);
    }
  }

  // Sort and merge.
  ArrayRef<ResourceMappingNode> mergedNodes = mergeUserDataNodeTable(allNodes);

  // Populate root node structs
  if (mergedNodes.size() > 0) {
    m_userDataNodeStorage = std::make_unique<SmallVector<ResourceMappingRootNode, 8>>();
    m_userDataNodeStorage->reserve(mergedNodes.size());
    for (unsigned i = 0; i < mergedNodes.size(); ++i)
      m_userDataNodeStorage->push_back({mergedNodes[i], stageMask});
    m_resourceMapping.userDataNodeCount = m_userDataNodeStorage->size();
    m_resourceMapping.pUserDataNodes = m_userDataNodeStorage->data();
  }

  // Collect descriptor range values (immutable descriptors) from all shader stages into one big table.
  SmallVector<DescriptorRangeValue, 8> allRangeValues;
  for (unsigned stage = 0; stage < ShaderStageNativeStageCount; ++stage) {
    if ((stageMask >> stage) & 1) {
      auto shaderInfo = getPipelineShaderInfo(ShaderStage(stage));
      for (const DescriptorRangeValue &rangeValue :
           ArrayRef<DescriptorRangeValue>(shaderInfo->pDescriptorRangeValues, shaderInfo->descriptorRangeValueCount))
        allRangeValues.push_back(rangeValue);
    }
  }

  // Sort them by set and binding, so we can spot duplicates.
  std::sort(allRangeValues.begin(), allRangeValues.end(),
            [](const DescriptorRangeValue &left, const DescriptorRangeValue &right) {
              if (left.set != right.set)
                return left.set < right.set;
              return left.binding < right.binding;
            });

  if (!allRangeValues.empty()) {
    // Create a new table with merged duplicates.
    m_staticDescriptorValueStorage = std::make_unique<SmallVector<StaticDescriptorValue, 8>>();
    auto &mergedRangeValues = *m_staticDescriptorValueStorage;
    ArrayRef<DescriptorRangeValue> rangeValues = allRangeValues;

    while (!rangeValues.empty()) {
      // Find the next block of duplicate rangeValues.
      unsigned duplicateCount = 1;
      for (; duplicateCount != rangeValues.size(); ++duplicateCount) {
        if (rangeValues[0].set != rangeValues[duplicateCount].set ||
            rangeValues[0].binding != rangeValues[duplicateCount].binding)
          break;
        assert(rangeValues[0].type == rangeValues[duplicateCount].type &&
               "Descriptor range value merge conflict: type");
        assert(rangeValues[0].arraySize == rangeValues[duplicateCount].arraySize &&
               "Descriptor range value merge conflict: arraySize");
        assert(memcmp(rangeValues[0].pValue, rangeValues[duplicateCount].pValue,
                      rangeValues[0].arraySize * sizeof(unsigned)) == 0 &&
               "Descriptor range value merge conflict: value");
      }

      // Keep the merged range.
      StaticDescriptorValue staticDescValue;
      memcpy(&staticDescValue, &rangeValues[0], sizeof(DescriptorRangeValue));
      staticDescValue.visibility = stageMask;
      mergedRangeValues.push_back(staticDescValue);
      rangeValues = rangeValues.slice(duplicateCount);
    }

    m_resourceMapping.staticDescriptorValueCount = m_staticDescriptorValueStorage->size();
    m_resourceMapping.pStaticDescriptorValues = m_staticDescriptorValueStorage->data();
  }
}

// =====================================================================================================================
// Merge user data nodes that have been collected into one big table
//
// @param allNodes : Table of nodes
ArrayRef<ResourceMappingNode> GraphicsContext::mergeUserDataNodeTable(SmallVectorImpl<ResourceMappingNode> &allNodes) {
  // Sort the nodes by offset, so we can spot duplicates.
  std::sort(allNodes.begin(), allNodes.end(), [](const ResourceMappingNode &left, const ResourceMappingNode &right) {
    return left.offsetInDwords < right.offsetInDwords;
  });

  // Merge duplicates.
  m_allocUserDataNodes.push_back(std::make_unique<SmallVector<ResourceMappingNode, 8>>());
  auto &mergedNodes = *m_allocUserDataNodes.back();
  ArrayRef<ResourceMappingNode> nodes = allNodes;

  while (!nodes.empty()) {
    // Find the next block of duplicate nodes.
    unsigned duplicatesCount = 1;
    for (; duplicatesCount != nodes.size(); ++duplicatesCount) {
      if (nodes[0].offsetInDwords != nodes[duplicatesCount].offsetInDwords)
        break;
      assert(nodes[0].type == nodes[duplicatesCount].type && "User data merge conflict: type");
      assert(nodes[0].sizeInDwords == nodes[duplicatesCount].sizeInDwords && "User data merge conflict: size");
      assert(nodes[0].type != ResourceMappingNodeType::IndirectUserDataVaPtr &&
             "User data merge conflict: only one shader stage expected to have vertex buffer");
      assert(nodes[0].type != ResourceMappingNodeType::StreamOutTableVaPtr &&
             "User data merge conflict: only one shader stage expected to have stream out");
      if (nodes[0].type != ResourceMappingNodeType::DescriptorTableVaPtr) {
        assert(nodes[0].srdRange.set == nodes[duplicatesCount].srdRange.set &&
               nodes[0].srdRange.binding == nodes[duplicatesCount].srdRange.binding &&
               "User data merge conflict: set or binding");
      }
    }

    if (duplicatesCount == 1 || nodes[0].type != ResourceMappingNodeType::DescriptorTableVaPtr) {
      // Keep the merged node.
      mergedNodes.push_back(nodes[0]);
    } else {
      // Merge the inner tables too. First collect nodes from all inner tables.
      SmallVector<ResourceMappingNode, 8> allInnerNodes;

      for (unsigned i = 0; i != duplicatesCount; ++i) {
        const auto &node = nodes[0];
        ArrayRef<ResourceMappingNode> innerTable(node.tablePtr.pNext, node.tablePtr.nodeCount);
        allInnerNodes.insert(allInnerNodes.end(), innerTable.begin(), innerTable.end());
      }

      // Call recursively to sort and merge.
      auto mergedInnerNodes = mergeUserDataNodeTable(allInnerNodes);

      // Finished merging the inner tables. Keep the merged DescriptorTableVaPtr node.
      ResourceMappingNode modifiedNode = nodes[0];
      modifiedNode.tablePtr.nodeCount = mergedInnerNodes.size();
      modifiedNode.tablePtr.pNext = &mergedInnerNodes[0];
      mergedNodes.push_back(modifiedNode);
    }

    nodes = nodes.slice(duplicatesCount);
  }
  return mergedNodes;
}
#endif

// =====================================================================================================================
// Check whether the pipeline uses features relevant to subgroup size
bool GraphicsContext::usesSubgroupSize() const {
  std::array<const PipelineShaderInfo *, 5> shaderInfos = {
      &m_pipelineInfo->vs, &m_pipelineInfo->tcs, &m_pipelineInfo->tes, &m_pipelineInfo->gs, &m_pipelineInfo->fs,
  };
  for (auto shaderInfo : shaderInfos) {
    if (!shaderInfo->pModuleData)
      continue;
    auto *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
    if (!moduleData->usage.useSubgroupSize)
      continue;
    return true;
  }
  return false;
}

} // namespace Llpc
