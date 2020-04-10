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
 * @file  llpcVertexFetch.h
 * @brief LLPC header file: contains declaration of class lgc::VertexFetch.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/llpcBuilder.h"
#include "lgc/llpcPipeline.h"
#include "llpcInternal.h"
#include "llpcIntrinsDefs.h"

namespace lgc
{

class PipelineState;
class ShaderSystemValues;

// Represents vertex format info corresponding to vertex attribute format (VkFormat).
struct VertexFormatInfo
{
    BufNumFormat    nfmt;           // Numeric format of vertex buffer
    BufDataFormat   dfmt;           // Data format of vertex buffer
    unsigned        numChannels;    // Valid number of channels
};

// Represents vertex component info corresponding to to vertex data format (BufDataFormat).
//
// NOTE: This info is used by vertex fetch instructions. We split vertex fetch into its per-component fetches when
// the original vertex fetch does not match the hardware requirements (such as vertex attribute offset, vertex
// attribute stride, etc..)
struct VertexCompFormatInfo
{
    unsigned        vertexByteSize; // Byte size of the vertex
    unsigned        compByteSize;   // Byte size of each individual component
    unsigned        compCount;      // Component count
    BufDataFmt      compDfmt;       // Equivalent data format of each component
};

// =====================================================================================================================
// Represents the manager of vertex fetch operations.
class VertexFetch
{
public:
    VertexFetch(llvm::Function* entrypoint, ShaderSystemValues* shaderSysValues, PipelineState* pipelineState);

    static VertexFormatInfo getVertexFormatInfo(const VertexInputDescription* description);

    llvm::Value* run(llvm::Type* inputTy, unsigned location, unsigned compIdx, llvm::Instruction* insertPos);

    // Gets variable corresponding to vertex index
    llvm::Value* getVertexIndex() { return m_vertexIndex; }

    // Gets variable corresponding to instance index
    llvm::Value* getInstanceIndex() { return m_instanceIndex; }

private:
    VertexFetch() = delete;
    VertexFetch(const VertexFetch&) = delete;
    VertexFetch& operator=(const VertexFetch&) = delete;

    static const VertexCompFormatInfo* getVertexComponentFormatInfo(unsigned dfmt);

    unsigned mapVertexFormat(unsigned dfmt, unsigned nfmt) const;

    llvm::Value* loadVertexBufferDescriptor(unsigned binding, llvm::Instruction* insertPos) const;

    void addVertexFetchInst(llvm::Value*       vbDesc,
                            unsigned           numChannels,
                            bool               is16bitFetch,
                            llvm::Value*       vbIndex,
                            unsigned           offset,
                            unsigned           stride,
                            unsigned           dfmt,
                            unsigned           nfmt,
                            llvm::Instruction* insertPos,
                            llvm::Value**      ppFetch) const;

    bool needPostShuffle(const VertexInputDescription* inputDesc,
                         std::vector<llvm::Constant*>&          shuffleMask) const;

    bool needPatchA2S(const VertexInputDescription* inputDesc) const;

    bool needSecondVertexFetch(const VertexInputDescription* inputDesc) const;

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Module*       m_module;          // LLVM module
    llvm::LLVMContext*  m_context;         // LLVM context
    ShaderSystemValues* m_shaderSysValues; // ShaderSystemValues object for getting vertex buffer pointer from
    PipelineState*      m_pipelineState;   // Pipeline state

    llvm::Value*    m_vertexIndex;     // Vertex index
    llvm::Value*    m_instanceIndex;   // Instance index
    llvm::Value*    m_baseInstance;    // Base instance
    llvm::Value*    m_instanceId;      // Instance ID

    static const VertexCompFormatInfo   MVertexCompFormatInfo[];   // Info table of vertex component format
    static const BufFormat              MVertexFormatMap[];        // Info table of vertex format mapping

    // Default values for vertex fetch (<4 x i32> or <8 x i32>)
    struct
    {
        llvm::Constant*   int8;      // < 0, 0, 0, 1 >
        llvm::Constant*   int16;     // < 0, 0, 0, 1 >
        llvm::Constant*   int32;     // < 0, 0, 0, 1 >
        llvm::Constant*   int64;     // < 0, 0, 0, 0, 0, 0, 0, 1 >
        llvm::Constant*   float16;   // < 0, 0, 0, 0x3C00 >
        llvm::Constant*   float32;   // < 0, 0, 0, 0x3F800000 >
        llvm::Constant*   double64;  // < 0, 0, 0, 0, 0, 0, 0, 0x3FF00000 >
    } m_fetchDefaults;
};

} // lgc
