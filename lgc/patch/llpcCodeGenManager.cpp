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
 * @file  llpcCodeGenManager.cpp
 * @brief LLPC source file: contains implementation of class lgc::CodeGenManager.
 ***********************************************************************************************************************
 */
#include "llpcCodeGenManager.h"
#include "llpcInternal.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"
#include "lgc/llpcPassManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#define DEBUG_TYPE "llpc-code-gen-manager"

namespace llvm {

namespace cl {

// -disable-fp32-denormals: disable target option fp32-denormals
static opt<bool> DisableFp32Denormals("disable-fp32-denormals", desc("Disable target option fp32-denormals"),
                                      init(false));

} // namespace cl

} // namespace llvm

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Setup LLVM target features, target features are set per entry point function.
//
// @param pipelineState : Pipeline state
// @param [in, out] module : LLVM module
void CodeGenManager::setupTargetFeatures(PipelineState *pipelineState, Module *module) {
  std::string globalFeatures = "";

  if (pipelineState->getOptions().includeDisassembly)
    globalFeatures += ",+DumpCode";

  if (cl::DisableFp32Denormals)
    globalFeatures += ",-fp32-denormals";

  for (auto func = module->begin(), end = module->end(); func != end; ++func) {
    if (!func->empty() && func->getLinkage() == GlobalValue::ExternalLinkage) {
      std::string targetFeatures(globalFeatures);
      AttrBuilder builder;

      ShaderStage shaderStage =
          getShaderStageFromCallingConv(pipelineState->getShaderStageMask(), func->getCallingConv());

      bool useSiScheduler = pipelineState->getShaderOptions(shaderStage).useSiScheduler;
      if (useSiScheduler) {
        // It was found that enabling both SIScheduler and SIFormClauses was bad on one particular
        // game. So we disable the latter here. That only affects XNACK targets.
        targetFeatures += ",+si-scheduler";
        builder.addAttribute("amdgpu-max-memory-clause", "1");
      }

      if (func->getCallingConv() == CallingConv::AMDGPU_GS) {
        // NOTE: For NGG primitive shader, enable 128-bit LDS load/store operations to optimize gvec4 data
        // read/write. This usage must enable the feature of using CI+ additional instructions.
        const auto nggControl = pipelineState->getNggControl();
        if (nggControl->enableNgg && !nggControl->passthroughMode)
          targetFeatures += ",+ci-insts,+enable-ds128";
      }

      if (func->getCallingConv() == CallingConv::AMDGPU_HS) {
        // Force s_barrier to be present (ignore optimization)
        builder.addAttribute("amdgpu-flat-work-group-size", "128,128");
      }
      if (func->getCallingConv() == CallingConv::AMDGPU_CS) {
        // Set the work group size
        const auto &csBuiltInUsage = pipelineState->getShaderModes()->getComputeShaderMode();
        unsigned flatWorkGroupSize =
            csBuiltInUsage.workgroupSizeX * csBuiltInUsage.workgroupSizeY * csBuiltInUsage.workgroupSizeZ;
        auto flatWorkGroupSizeString = std::to_string(flatWorkGroupSize);
        builder.addAttribute("amdgpu-flat-work-group-size", flatWorkGroupSizeString + "," + flatWorkGroupSizeString);
      }

      auto gfxIp = pipelineState->getTargetInfo().getGfxIpVersion();
      if (gfxIp.major >= 9)
        targetFeatures += ",+enable-scratch-bounds-checks";

      if (gfxIp.major >= 10) {
        // Setup wavefront size per shader stage
        unsigned waveSize = pipelineState->getShaderWaveSize(shaderStage);

        targetFeatures += ",+wavefrontsize" + std::to_string(waveSize);

        // Allow driver setting for WGP by forcing backend to set 0
        // which is then OR'ed with the driver set value
        targetFeatures += ",+cumode";
      }

      if (shaderStage != ShaderStageCopyShader) {
        const auto &shaderMode = pipelineState->getShaderModes()->getCommonShaderMode(shaderStage);
        if (shaderMode.fp16DenormMode == FpDenormMode::FlushNone ||
            shaderMode.fp16DenormMode == FpDenormMode::FlushIn ||
            shaderMode.fp64DenormMode == FpDenormMode::FlushNone || shaderMode.fp64DenormMode == FpDenormMode::FlushIn)
          targetFeatures += ",+fp64-fp16-denormals";
        else if (shaderMode.fp16DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp16DenormMode == FpDenormMode::FlushInOut ||
                 shaderMode.fp64DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp64DenormMode == FpDenormMode::FlushInOut)
          targetFeatures += ",-fp64-fp16-denormals";
        if (shaderMode.fp32DenormMode == FpDenormMode::FlushNone || shaderMode.fp32DenormMode == FpDenormMode::FlushIn)
          targetFeatures += ",+fp32-denormals";
        else if (shaderMode.fp32DenormMode == FpDenormMode::FlushOut ||
                 shaderMode.fp32DenormMode == FpDenormMode::FlushInOut)
          targetFeatures += ",-fp32-denormals";
      }

      builder.addAttribute("target-features", targetFeatures);
      AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
      func->addAttributes(attribIdx, builder);
    }
  }
}

} // namespace lgc
