/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvLowerTranslator.cpp
 * @brief LLPC source file: contains implementation of Llpc::SpirvLowerTranslator
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerTranslator.h"
#include "LLVMSPIRVLib.h"
#include "llpcCompiler.h"
#include "llpcContext.h"
#include "lgc/Builder.h"
#include <sstream>
#include <string>

#define DEBUG_TYPE "llpc-spirv-lower-translator"

using namespace llvm;
using namespace Llpc;

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
llvm::PreservedAnalyses SpirvLowerTranslator::run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Translator\n");

  SpirvLower::init(&module);

#ifdef LLPC_ENABLE_SPIRV_OPT
  InitSpvGen();
#endif

  m_context = static_cast<Context *>(&module.getContext());

  // Translate SPIR-V binary to machine-independent LLVM module
  translateSpirvToLlvm(m_shaderInfo, &module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Translates SPIR-V binary to machine-independent LLVM module.
//
// @param shaderInfo : Specialization info
// @param [in/out] module : Module to translate into, initially empty
void SpirvLowerTranslator::translateSpirvToLlvm(const PipelineShaderInfo *shaderInfo, Module *module) {
  BinaryData optimizedSpirvBin = {};
  const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
  assert(moduleData->binType == BinaryType::Spirv);
  const BinaryData *spirvBin = &moduleData->binCode;
  if (ShaderModuleHelper::optimizeSpirv(spirvBin, &optimizedSpirvBin) == Result::Success)
    spirvBin = &optimizedSpirvBin;

  std::string spirvCode(static_cast<const char *>(spirvBin->pCode), spirvBin->codeSize);
  std::istringstream spirvStream(spirvCode);
  std::string errMsg;
  SPIRV::SPIRVSpecConstMap specConstMap;
  ShaderStage entryStage = shaderInfo->entryStage;
  // Build specialization constant map
  if (shaderInfo->pSpecializationInfo) {
    for (unsigned i = 0; i < shaderInfo->pSpecializationInfo->mapEntryCount; ++i) {
      SPIRV::SPIRVSpecConstEntry specConstEntry = {};
      auto mapEntry = &shaderInfo->pSpecializationInfo->pMapEntries[i];
      specConstEntry.DataSize = mapEntry->size;
      specConstEntry.Data = voidPtrInc(shaderInfo->pSpecializationInfo->pData, mapEntry->offset);
      specConstMap[mapEntry->constantID] = specConstEntry;
    }
  }

  Context *context = static_cast<Context *>(&module->getContext());

  // Build the converting sampler info.
  auto resourceMapping = context->getResourceMapping();
  auto descriptorRangeValues = ArrayRef<StaticDescriptorValue>(resourceMapping->pStaticDescriptorValues,
                                                               resourceMapping->staticDescriptorValueCount);
  SmallVector<SPIRV::ConvertingSampler, 4> convertingSamplers;
  for (const auto &range : descriptorRangeValues) {
    if (range.type == ResourceMappingNodeType::DescriptorYCbCrSampler) {
      uint32_t rangeSet = range.set;
      if (context->getPipelineContext()->getPipelineOptions()->getGlState().replaceSetWithResourceType &&
          range.set == 0) {
        rangeSet = PipelineContext::getGlResourceNodeSetFromType(range.type);
      }
      convertingSamplers.push_back(
          {rangeSet, range.binding,
           ArrayRef<unsigned>(range.pValue, range.arraySize * SPIRV::ConvertingSamplerDwordCount)});
    }
  }

  if (!readSpirv(context->getBuilder(), &(moduleData->usage), &(shaderInfo->options), spirvStream,
                 convertToExecModel(entryStage), shaderInfo->pEntryTarget, specConstMap, convertingSamplers,
                 m_globalVarPrefix, module, errMsg)) {
    report_fatal_error(Twine("Failed to translate SPIR-V to LLVM (") +
                           getShaderStageName(static_cast<ShaderStage>(entryStage)) + " shader): " + errMsg,
                       false);
  }

  ShaderModuleHelper::cleanOptimizedSpirv(&optimizedSpirvBin);
}
