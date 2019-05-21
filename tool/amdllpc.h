/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  amdllpc.h
 * @brief LLPC header file: common header for amdllpc command-line utility
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"

// Lay out dummy descriptors and other information for one shader stage. This is used when running amdllpc on a single
// SPIR-V or GLSL shader, rather than on a .pipe file. Memory allocated here may be leaked, but that does not
// matter because we are running a short-lived command-line utility.
void DoAutoLayoutDesc(Llpc::ShaderStage                 shaderStage,
                      Llpc::BinaryData                  spirvBin,
                      Llpc::GraphicsPipelineBuildInfo*  pPipelineInfo,
                      Llpc::PipelineShaderInfo*         pShaderInfo,
                      uint32_t&                         topLevelOffset);

