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
 * @file  llpcAutoLayout.h
 * @brief LLPC header file: descriptor layout utilities for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include <map>
#include <vector>

namespace Llpc {
namespace StandaloneCompiler {

struct CompileInfo; // Defined in llpcCompilationUtils.h.

struct ResourceNodeSet {
  std::vector<Llpc::ResourceMappingNode> nodes; // Vector of resource mapping nodes
  std::map<unsigned, unsigned> bindingMap;      // Map from binding to index in nodes vector
  unsigned visibility = 0;                      // Mask of shader stages which this set is visible to
};

using ResourceMappingNodeMap = std::map<unsigned, ResourceNodeSet>;

// Lay out dummy bottom-level descriptors and other information for one shader stage. This is used when running
// standalone compiler on a single SPIR-V or GLSL shader, rather than on a .pipe file. Memory allocated here may be
// leaked, but that does not matter because we are running a short-lived command-line utility.
void doAutoLayoutDesc(ShaderStage shaderStage, BinaryData spirvBin, GraphicsPipelineBuildInfo *pipelineInfo,
                      PipelineShaderInfo *shaderInfo, ResourceMappingNodeMap &resNodeSets, unsigned &pushConstSize,
                      bool autoLayoutDesc, bool reverseThreadGroup);

// Lay out dummy top-level descriptors and populate ResourceMappingData. This is used when running standalone compiler
// on a single SPIR-V or GLSL shader, rather than on a .pipe file.
void buildTopLevelMapping(unsigned shaderMask, const ResourceMappingNodeMap &resNodeSets, unsigned pushConstSize,
                          ResourceMappingData *resourceMapping, bool autoLayoutDesc);

bool checkResourceMappingCompatible(const ResourceMappingData *resourceMapping, unsigned autoLayoutUserDataNodeCount,
                                    const ResourceMappingRootNode *autoLayoutUserDataNodes);

bool checkPipelineStateCompatible(const ICompiler *compiler, GraphicsPipelineBuildInfo *pipelineInfo,
                                  GraphicsPipelineBuildInfo *autoLayoutPipelineInfo, GfxIpVersion gfxIp);

} // namespace StandaloneCompiler
} // namespace Llpc
