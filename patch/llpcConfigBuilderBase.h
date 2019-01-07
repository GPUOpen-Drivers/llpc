/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcConfigBuilderBase.h
 * @brief LLPC header file: contains declaration of class Llpc::ConfigBuilderBase.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcContext.h"

namespace Llpc
{

// =====================================================================================================================
// Register configuration builder base class.
class ConfigBuilderBase
{
public:
    ConfigBuilderBase(llvm::Module* pModule);
    ~ConfigBuilderBase();

    void WritePalMetadata();

    // -----------------------------------------------------------------------------------------------------------------
protected:
    llvm::Module*                   m_pModule;            // LLVM module being processed
    Context*                        m_pContext;           // LLPC context
    uint8_t*                        m_pConfig = nullptr;  // Register/metadata configuration
    size_t                          m_configSize = 0;     // Size of register/metadata configuration
    GfxIpVersion                    m_gfxIp;              // Graphics IP version info

    bool                            m_hasVs;              // Whether the pipeline has vertex shader
    bool                            m_hasTcs;             // Whether the pipeline has tessellation control shader
    bool                            m_hasTes;             // Whether the pipeline has tessellation evaluation shader
    bool                            m_hasGs;              // Whether the pipeline has geometry shader
};

} // Llpc
