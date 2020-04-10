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
 * @file  vkgcUtil.cpp
 * @brief VKGC source file: contains implementation of VKGC internal types and utility functions
 ***********************************************************************************************************************
 */
#include "vkgcUtil.h"
#include "vkgcElfReader.h"

#include <sys/types.h>
#include <sys/stat.h>

#define DEBUG_TYPE "vkgc-util"

namespace Vkgc
{

// =====================================================================================================================
// Gets name string of the abbreviation for the specified shader stage
const char* getShaderStageAbbreviation(
    ShaderStage shaderStage,  // Shader stage
    bool        upper)        // Whether to use uppercase for the abbreviation (default is lowercase)
{
    const char* abbr = nullptr;

    if (shaderStage == ShaderStageCopyShader)
    {
        abbr = upper ? "COPY" : "Copy";
    }
    else if (shaderStage < ShaderStageCount)
    {
        if (upper)
        {
            static const char* ShaderStageAbbrs[] =
            {
                "VS",
                "TCS",
                "TES",
                "GS",
                "FS",
                "CS",
            };

            abbr = ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
        }
        else
        {
            static const char* ShaderStageAbbrs[] =
            {
                "Vs",
                "Tcs",
                "Tes",
                "Gs",
                "Fs",
                "Cs",
            };

            abbr = ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
        }
    }
    else
    {
        abbr = "Bad";
    }

    return abbr;
}

// =====================================================================================================================
// Create directory.
bool createDirectory(
    const char* dir)  // [in] the path of directory
{
    int result = mkdir(dir, S_IRWXU);
    return (result == 0);
}

// =====================================================================================================================
// Helper macro
#define CASE_CLASSENUM_TO_STRING(TYPE, ENUM) \
    case TYPE::ENUM: string = #ENUM; break;

// =====================================================================================================================
// Translate enum "ResourceMappingNodeType" to string
const char* getResourceMappingNodeTypeName(
    ResourceMappingNodeType type)  // Resource map node type
{
    const char* string = nullptr;
    switch (type)
    {
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, Unknown)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorResource)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorSampler)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorYCbCrSampler)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorCombinedTexture)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorTexelBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorFmask)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorTableVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, IndirectUserDataVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, PushConst)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorBufferCompact)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, StreamOutTableVaPtr)
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }
    return string;
}

} // Vkgc
