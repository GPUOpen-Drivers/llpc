/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  RelocHandler.cpp
 * @brief LLPC source file: Class to handle internal relocatable values when ELF linking
 ***********************************************************************************************************************
 */
#include "RelocHandler.h"
#include "lgc/LgcContext.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"

#define DEBUG_TYPE "lgc-elf-reloc-handler"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Parse descriptor set, binding, and type letter from reloc name
//
// @param str : StringRef containing text of the form "1_2_r"
// @param [out] descSet : Value of first integer
// @param [out] binding : Value of second integer
// @param [out] type : The type of the resource node
// @returns : True if parse successful
static bool parseDescSetBinding(StringRef str, unsigned &descSet, unsigned &binding, ResourceNodeType &type) {
  type = ResourceNodeType::Unknown;
  if (str.consumeInteger(10, descSet) || str.empty() || str[0] != '_')
    return false;
  str = str.drop_front();
  if (str.consumeInteger(10, binding))
    return false;
  if (str.empty())
    return true;
  if (str.size() != 2 || str[0] != '_')
    return false;

  int typeLetter = str[1];
  switch (typeLetter) {
  case 's':
    type = ResourceNodeType::DescriptorSampler;
    break;
  case 'r':
    type = ResourceNodeType::DescriptorResource;
    break;
  case 'b':
    type = ResourceNodeType::DescriptorBuffer;
    break;
  case 't':
    type = ResourceNodeType::DescriptorTexelBuffer;
    break;
  case 'f':
    type = ResourceNodeType::DescriptorFmask;
    break;
  case 'x':
    type = ResourceNodeType::Unknown;
    break;
  default:
    llvm_unreachable("Unexpected resource type in relocation.");
    break;
  }
  return true;
}

// =====================================================================================================================
// Get value for a reloc, if it is an internal LGC one
//
// @param name : Symbol name used by relocation
// @param [out] value : Returns value of symbol if found
// @returns : True if successful, false if not handled
bool RelocHandler::getValue(StringRef name, uint64_t &value) {
  if (name.startswith(reloc::DescriptorOffset)) {
    // Descriptor offset in bytes in the descriptor table for its set, or in the spill table if in the root table.
    unsigned descSet = 0;
    unsigned binding = 0;
    ResourceNodeType type = ResourceNodeType::Unknown;
    if (parseDescSetBinding(name.drop_front(strlen(reloc::DescriptorOffset)), descSet, binding, type)) {
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) = getPipelineState()->findResourceNode(type, descSet, binding);

      if (!node)
        report_fatal_error("No resource node for " + name);

      value = node->offsetInDwords * 4;
      if (type == ResourceNodeType::DescriptorSampler &&
          node->concreteType == ResourceNodeType::DescriptorCombinedTexture)
        value += DescriptorSizeResource;
      return true;
    }
  }

  if (name.startswith(reloc::DescriptorTableOffset)) {
    unsigned descSet = 0;
    if (!name.drop_front(strlen(reloc::DescriptorTableOffset)).consumeInteger(10, descSet)) {
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) =
          getPipelineState()->findResourceNode(ResourceNodeType::DescriptorTableVaPtr, descSet, 0);

      // If all entries for the descriptor set are in the root table, then the descriptor table will not be found.
      // In that case, the value does not matter, so just return 0.
      if (node) {
        value = node->offsetInDwords * 4;
        m_pipelineState->getPalMetadata()->setUserDataSpillUsage(node->offsetInDwords);
      } else {
        value = 0;
      }
      return true;
    }
  }

  if (name.startswith(reloc::DescriptorUseSpillTable)) {
    // If the corresponding node is a root node and the type is DescriptorBuffer, use the spill table to get the
    // descriptor pointer.
    unsigned descSet = 0;
    unsigned binding = 0;
    // We don't care about typeLetter. This should always be 'b' for DescriptorBuffers.
    ResourceNodeType type = ResourceNodeType::Unknown;
    StringRef suffix = name.drop_front(strlen(reloc::DescriptorUseSpillTable));
    if (parseDescSetBinding(suffix, descSet, binding, type)) {
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) =
          getPipelineState()->findResourceNode(ResourceNodeType::DescriptorBuffer, descSet, binding);

      if (!node)
        report_fatal_error("No resource node for " + name);

      // Check if this is a top-level node.
      value = (node == outerNode) ? 1 : 0;

      // Mark access to spill table.
      if (value == 1)
        m_pipelineState->getPalMetadata()->setUserDataSpillUsage(0);

      return true;
    }
  }

  if (name.startswith(reloc::DescriptorStride)) {
    // Descriptor stride in bytes.
    unsigned descSet = 0;
    unsigned binding = 0;
    ResourceNodeType type = ResourceNodeType::Unknown;
    if (parseDescSetBinding(name.drop_front(strlen(reloc::DescriptorStride)), descSet, binding, type)) {
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) = getPipelineState()->findResourceNode(type, descSet, binding);
      if (!node)
        report_fatal_error("No resource node for " + name);
      value = node->stride * sizeof(uint32_t);
      return true;
    }
  }

  if (name.startswith(reloc::CompactBuffer)) {
    // Descriptor stride in bytes.
    unsigned descSet = 0;
    unsigned binding = 0;
    ResourceNodeType type = ResourceNodeType::Unknown;
    if (parseDescSetBinding(name.drop_front(strlen(reloc::CompactBuffer)), descSet, binding, type)) {
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) = getPipelineState()->findResourceNode(type, descSet, binding);
      if (!node)
        report_fatal_error("No resource node for " + name);
      value = (node->concreteType == ResourceNodeType::DescriptorBufferCompact ||
               node->concreteType == ResourceNodeType::DescriptorConstBufferCompact)
                  ? 1
                  : 0;
      return true;
    }
  }

  if (name == reloc::NumSamples) {
    value = m_pipelineState->getRasterizerState().numSamples;
    return true;
  }
  if (name == reloc::SamplePatternIdx) {
    value = m_pipelineState->getRasterizerState().samplePatternIdx;
    return true;
  }
  if (name == reloc::DeviceIdx) {
    value = m_pipelineState->getDeviceIndex();
    return true;
  }
  if (name == reloc::Pushconst) {
    auto *pushConstantNode = m_pipelineState->findPushConstantResourceNode();
    value = pushConstantNode->offsetInDwords * 4;
    getPipelineState()->getPalMetadata()->setUserDataSpillUsage(pushConstantNode->offsetInDwords);
    return true;
  }
  if (name == reloc::ShadowDescriptorTableEnabled) {
    value = m_pipelineState->getOptions().shadowDescriptorTable != ShadowDescriptorTableDisable;
    return true;
  }

  if (name == reloc::ShadowDescriptorTable) {
    value = m_pipelineState->getOptions().shadowDescriptorTable;
    return true;
  }

  return false;
}
