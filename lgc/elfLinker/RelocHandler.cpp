/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

#define DEBUG_TYPE "lgc-elf-reloc-handler"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Parse descriptor set, binding, and type letter from reloc name
//
// @param str : StringRef containing text of the form "1_2_r"
// @param [out] descSet : Value of first integer
// @param [out] binding : Value of second integer
// @param [out] typeLetter : Value of optional type letter, 0 if none
// @return : True if parse successful
static bool parseDescSetBinding(StringRef str, unsigned &descSet, unsigned &binding, int &typeLetter) {
  typeLetter = 0;
  if (str.consumeInteger(10, descSet) || str.empty() || str[0] != '_')
    return false;
  str = str.drop_front();
  if (str.consumeInteger(10, binding))
    return false;
  if (str.empty())
    return true;
  if (str.size() != 2 || str[0] != '_')
    return false;
  typeLetter = str[1];
  return true;
}

// =====================================================================================================================
// Get value for a reloc, if it is an internal LGC one
//
// @param name : Symbol name used by relocation
// @param [out] value : Returns value of symbol if found
// @return : True if successful, false if not handled
bool RelocHandler::getValue(StringRef name, uint64_t &value) {

  if (name.startswith(reloc::DescriptorOffset)) {
    // Descriptor offset in bytes in the descriptor table for its set, or in the spill table if in the root table.
    unsigned descSet = 0;
    unsigned binding = 0;
    int typeLetter = 0;
    if (parseDescSetBinding(name.drop_front(strlen(reloc::DescriptorOffset)), descSet, binding, typeLetter)) {
      ResourceNodeType type = ResourceNodeType::Unknown;
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
      }
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) = getPipelineState()->findResourceNode(type, descSet, binding);
      if (!node)
        report_fatal_error("No resource node for " + name);
      if (node->type == ResourceNodeType::DescriptorBufferCompact)
        getPipelineState()->setError("Cannot relocate to compact buffer descriptor");

      value = node->offsetInDwords * 4;
      if (type == ResourceNodeType::DescriptorSampler && node->type == ResourceNodeType::DescriptorCombinedTexture)
        value += getPipelineState()->getTargetInfo().getGpuProperty().descriptorSizeResource;
      return true;
    }
  }

  if (name.startswith(reloc::DescriptorStride)) {
    // Descriptor stride in bytes.
    unsigned descSet = 0;
    unsigned binding = 0;
    int typeLetter = 0;
    if (parseDescSetBinding(name.drop_front(strlen(reloc::DescriptorStride)), descSet, binding, typeLetter)) {
      const ResourceNode *outerNode = nullptr;
      const ResourceNode *node = nullptr;
      std::tie(outerNode, node) = getPipelineState()->findResourceNode(ResourceNodeType::Unknown, descSet, binding);
      if (!node)
        report_fatal_error("No resource node for " + name);
      const GpuProperty &gpuProperty = m_pipelineState->getTargetInfo().getGpuProperty();
      switch (node->type) {
      case ResourceNodeType::DescriptorResource:
        value = gpuProperty.descriptorSizeResource;
        return true;
      case ResourceNodeType::DescriptorSampler:
        value = gpuProperty.descriptorSizeSampler;
        return true;
      case ResourceNodeType::DescriptorCombinedTexture:
        value = gpuProperty.descriptorSizeResource + gpuProperty.descriptorSizeSampler;
        return true;
      default:
        break;
      }
      report_fatal_error("Wrong resource node type for " + name);
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

  return false;
}
