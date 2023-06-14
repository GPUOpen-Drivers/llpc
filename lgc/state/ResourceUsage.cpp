/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ResourceUsage.cpp
 * @brief LLPC source file: contains implementation of ResourceUsage and InterfaceData.
 ***********************************************************************************************************************
 */
#include "lgc/state/ResourceUsage.h"

using namespace lgc;

// =====================================================================================================================
ResourceUsage::ResourceUsage(ShaderStage shaderStage) {
  // NOTE: We use memset to explicitly zero builtInUsage since it has unions inside.
  memset(&builtInUsage, 0, sizeof(builtInUsage));

  if (shaderStage == ShaderStageVertex) {
    // NOTE: For vertex shader, PAL expects base vertex and base instance in user data,
    // even if they are not used in shader.
    builtInUsage.vs.baseVertex = true;
    builtInUsage.vs.baseInstance = true;
  } else if (shaderStage == ShaderStageTessControl) {
    inOutUsage.tcs.calcFactor = {};
  } else if (shaderStage == ShaderStageGeometry) {
    inOutUsage.gs.calcFactor = {};
  } else if (shaderStage == ShaderStageFragment) {
    for (uint32_t i = 0; i < MaxColorTargets; ++i) {
      inOutUsage.fs.outputTypes[i] = BasicType::Unknown;
    }

    inOutUsage.fs.cbShaderMask = 0;
    inOutUsage.fs.isNullFs = false;
  }
}

// =====================================================================================================================
InterfaceData::InterfaceData() {
}
