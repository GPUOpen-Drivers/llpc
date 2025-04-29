/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  WorkgroupLayout.cpp
* @brief LLPC source file: Implementation of swizzle workgroup layout
***********************************************************************************************************************
*/
#include "lgc/util/WorkgroupLayout.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Do automatic workgroup size reconfiguration in a compute shader, to allow ReconfigWorkgroupLayout
// to apply optimizations.
//
// @param shaderStage : Shader stage
SwizzleWorkgroupLayout lgc::calculateWorkgroupLayout(PipelineState *pipelineState, ShaderStageEnum shaderStage) {
  unsigned workgroupSizeX = 0;
  unsigned workgroupSizeY = 0;
  SwizzleWorkgroupLayout resultLayout = {WorkgroupLayout::Unknown, WorkgroupLayout::Unknown};
  DerivativeMode derivativeMode = DerivativeMode::Linear;

  if (shaderStage != ShaderStage::Compute && shaderStage != ShaderStage::Task && shaderStage != ShaderStage::Mesh) {
    return resultLayout;
  }

  if (shaderStage == ShaderStage::Compute) {
    const ResourceUsage *resUsage = pipelineState->getShaderResourceUsage(ShaderStage::Compute);
    if (resUsage->builtInUsage.cs.foldWorkgroupXY) {
      llvm_unreachable("Should never be called!");
    }
    auto &mode = pipelineState->getShaderModes()->getComputeShaderMode();
    workgroupSizeX = mode.workgroupSizeX;
    workgroupSizeY = mode.workgroupSizeY;
    derivativeMode = mode.derivativeMode;
  }

  if (shaderStage == ShaderStage::Task) {
    const ResourceUsage *resUsage = pipelineState->getShaderResourceUsage(ShaderStage::Task);
    if (resUsage->builtInUsage.task.foldWorkgroupXY) {
      llvm_unreachable("Should never be called!");
    }
    auto &mode = pipelineState->getShaderModes()->getComputeShaderMode();
    workgroupSizeX = mode.workgroupSizeX;
    workgroupSizeY = mode.workgroupSizeY;
    derivativeMode = mode.derivativeMode;
  }

  if (shaderStage == ShaderStage::Mesh) {
    const ResourceUsage *resUsage = pipelineState->getShaderResourceUsage(ShaderStage::Mesh);
    if (resUsage->builtInUsage.mesh.foldWorkgroupXY) {
      llvm_unreachable("Should never be called!");
    }
    auto &mode = pipelineState->getShaderModes()->getMeshShaderMode();
    workgroupSizeX = mode.workgroupSizeX;
    workgroupSizeY = mode.workgroupSizeY;
    derivativeMode = mode.derivativeMode;
  }

  if (derivativeMode == DerivativeMode::Quads) {
    resultLayout.microLayout = WorkgroupLayout::Quads;
  } else if (derivativeMode == DerivativeMode::Linear) {
    resultLayout.microLayout = WorkgroupLayout::Linear;
  }

  if (pipelineState->getOptions().forceCsThreadIdSwizzling ||
      (pipelineState->getOptions().xInterleave == 3 && pipelineState->getOptions().yInterleave == 3)) {
    if ((workgroupSizeX >= 16) && (workgroupSizeX % 8 == 0) && (workgroupSizeY % 4 == 0)) {
      resultLayout.macroLayout = WorkgroupLayout::SexagintiQuads;
    }
  }

  // If no configuration has been specified, apply a reconfigure if the compute shader uses images and the
  // pipeline option was enabled.
  if (pipelineState->getOptions().reconfigWorkgroupLayout) {
    if ((workgroupSizeX % 2) == 0 && (workgroupSizeY % 2) == 0) {
      if (workgroupSizeX % 8 == 0) {
        // It can be reconfigured into 8 X N
        if (resultLayout.macroLayout == WorkgroupLayout::Unknown) {
          resultLayout.macroLayout = WorkgroupLayout::SexagintiQuads;
        }
      } else {
        // If our local size in the X & Y dimensions are multiples of 2, we can reconfigure.
        if (resultLayout.microLayout == WorkgroupLayout::Unknown) {
          resultLayout.microLayout = WorkgroupLayout::Quads;
        }
      }
    }
  }

  return resultLayout;
}

// =====================================================================================================================
// Reconfigure the workgroup for optimization purposes.
// @param localInvocationId : This is a v3i32 shader input (three VGPRs set up in hardware).
// @param pipelineState: pipeline state
// @param shaderStage : Shader stage
// @param macroLayout : Swizzle the thread id into macroLayout from macro level
// @param microLayout : Swizzle the thread id into microLayout from micro level
// @param workgroupSizeX : WorkgroupSize X for thread Id numbers
// @param workgroupSizeY : WorkgroupSize Y for thread Id numbers
// @param workgroupSizeZ : WorkgroupSize Z for thread Id numbers
// @param isHwLocalInvocationId : identify whether the localInvocationId is builtInLocalInvcocationId or
// BuiltInUnswizzledLocalInvocationId
// @param builder : the builder to use
Value *lgc::reconfigWorkgroupLayout(Value *localInvocationId, PipelineState *pipelineState, ShaderStageEnum shaderStage,
                                    WorkgroupLayout macroLayout, WorkgroupLayout microLayout, unsigned workgroupSizeX,
                                    unsigned workgroupSizeY, unsigned workgroupSizeZ, bool isHwLocalInvocationId,
                                    BuilderBase &builder) {
  Value *newLocalInvocationId = PoisonValue::get(localInvocationId->getType());
  unsigned bitsX = 0;
  unsigned bitsY = 0;

  Value *tidX = builder.CreateExtractElement(localInvocationId, builder.getInt32(0), "tidX");
  Value *tidY = builder.CreateExtractElement(localInvocationId, builder.getInt32(1), "tidY");

  Value *apiX = builder.getInt32(0);
  Value *apiY = builder.getInt32(0);
  Value *apiZ = builder.getInt32(0);
  if (workgroupSizeZ > 1) {
    apiZ = builder.CreateExtractElement(localInvocationId, builder.getInt32(2), "tidZ");
  }

  if (isHwLocalInvocationId) {
    apiX = tidX;
    apiY = tidY;
  } else {
    // Micro-tiling with quad:2x2, the thread-id will be marked as {<0,0>,<1,0>,<0,1>,<1,1>}
    // for each quad. Each 4 threads will be wrapped in the same tid.
    Value *tidXY = builder.CreateAdd(builder.CreateMul(tidY, builder.getInt32(workgroupSizeX)), tidX);
    if (microLayout == WorkgroupLayout::Quads) {
      apiX = builder.CreateAnd(tidXY, builder.getInt32(1));
      apiY = builder.CreateAnd(builder.CreateLShr(tidXY, builder.getInt32(1)), builder.getInt32(1));
      tidXY = builder.CreateLShr(tidXY, builder.getInt32(2));
      bitsX = 1;
      bitsY = 1;
    }

    // Macro-tiling with 8xN block
    if (macroLayout == WorkgroupLayout::SexagintiQuads) {
      unsigned bits = 3 - bitsX;
      Value *subTileApiX = builder.CreateAnd(tidXY, builder.getInt32((1 << bits) - 1));
      subTileApiX = builder.CreateShl(subTileApiX, builder.getInt32(bitsX));
      apiX = builder.CreateOr(apiX, subTileApiX);

      // 1. Folding 4 threads as one tid if micro-tiling with quad before.
      //    After the folding, each 4 hwThreadIdX share the same tid after tid>>=bits.
      //    For example: hwThreadId.X = 0~3, the tid will be 0; <apiX,apiY> will be {<0,0>,<1,0>,<0,1>,<1,1>}
      //                 hwThreadId.X = 4~7, the tid will be 1; <apiX,apiY> will be {<0,0>,<1,0>,<0,1>,<1,1>}
      // 2. Folding 8 threads as one tid without any micro-tiling before.
      //    After the folding, each 8 hwThreadIdX share the same tid after tid>>=bits and only apiX are calculated.
      //    For example: hwThreadId.X = 0~7, tid = hwThreadId.X/8 = 0; <apiX> will be {0,1,...,7}
      //                 hwThreadId.X = 8~15, tid = hwThreadId.X/8 = 1; <apiX> will be {0,1,...,7}
      tidXY = builder.CreateLShr(tidXY, builder.getInt32(bits));
      bitsX = 3;

      // 1. Unfolding 4 threads, it needs to set walkY = workgroupSizeY/2 as these threads are wrapped in 2X2 size.
      // 2. Unfolding 8 threads, it needs to set walkY = workgroupSizeY/2 as these threads are wrapped in 1x8 size.
      // After unfolding these threads, it needs '| apiX and | apiY' to calculated each thread's coordinate
      // in the unfolded wrap threads.
      unsigned walkY = workgroupSizeY >> bitsY;
      Value *tileApiY = builder.CreateShl(builder.CreateURem(tidXY, builder.getInt32(walkY)), builder.getInt32(bitsY));
      apiY = builder.CreateOr(apiY, tileApiY);
      Value *tileApiX = builder.CreateShl(builder.CreateUDiv(tidXY, builder.getInt32(walkY)), builder.getInt32(bitsX));
      apiX = builder.CreateOr(apiX, tileApiX);
    } else {
      // Update the coordinates for each 4 wrap-threads then unfold each thread to calculate the coordinate by '| apiX
      // and | apiY'
      unsigned walkX = workgroupSizeX >> bitsX;
      Value *tileApiX = builder.CreateShl(builder.CreateURem(tidXY, builder.getInt32(walkX)), builder.getInt32(bitsX));
      apiX = builder.CreateOr(apiX, tileApiX);
      Value *tileApiY = builder.CreateShl(builder.CreateUDiv(tidXY, builder.getInt32(walkX)), builder.getInt32(bitsY));
      apiY = builder.CreateOr(apiY, tileApiY);
    }
  }

  newLocalInvocationId = builder.CreateInsertElement(newLocalInvocationId, apiX, uint64_t(0));
  newLocalInvocationId = builder.CreateInsertElement(newLocalInvocationId, apiY, uint64_t(1));
  newLocalInvocationId = builder.CreateInsertElement(newLocalInvocationId, apiZ, uint64_t(2));
  return newLocalInvocationId;
}
