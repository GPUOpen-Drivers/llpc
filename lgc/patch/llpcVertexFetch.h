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
    uint32_t        numChannels;    // Valid number of channels
};

// Represents vertex component info corresponding to to vertex data format (BufDataFormat).
//
// NOTE: This info is used by vertex fetch instructions. We split vertex fetch into its per-component fetches when
// the original vertex fetch does not match the hardware requirements (such as vertex attribute offset, vertex
// attribute stride, etc..)
struct VertexCompFormatInfo
{
    uint32_t        vertexByteSize; // Byte size of the vertex
    uint32_t        compByteSize;   // Byte size of each individual component
    uint32_t        compCount;      // Component count
    BufDataFmt      compDfmt;       // Equivalent data format of each component
};

// =====================================================================================================================
// Represents the manager of vertex fetch operations.
class VertexFetch
{
public:
    VertexFetch(llvm::Function* pEntrypoint, ShaderSystemValues* pShaderSysValues, PipelineState* pPipelineState);

    static VertexFormatInfo GetVertexFormatInfo(const VertexInputDescription* pDescription);

    llvm::Value* Run(llvm::Type* pInputTy, uint32_t location, uint32_t compIdx, llvm::Instruction* pInsertPos);

    // Gets variable corresponding to vertex index
    llvm::Value* GetVertexIndex() { return m_pVertexIndex; }

    // Gets variable corresponding to instance index
    llvm::Value* GetInstanceIndex() { return m_pInstanceIndex; }

private:
    VertexFetch() = delete;
    VertexFetch(const VertexFetch&) = delete;
    VertexFetch& operator=(const VertexFetch&) = delete;

    static const VertexCompFormatInfo* GetVertexComponentFormatInfo(uint32_t dfmt);

    uint32_t MapVertexFormat(uint32_t dfmt, uint32_t nfmt) const;

    llvm::Value* LoadVertexBufferDescriptor(uint32_t binding, llvm::Instruction* pInsertPos) const;

    void AddVertexFetchInst(llvm::Value*       pVbDesc,
                            uint32_t           numChannels,
                            bool               is16bitFetch,
                            llvm::Value*       pVbIndex,
                            uint32_t           offset,
                            uint32_t           stride,
                            uint32_t           dfmt,
                            uint32_t           nfmt,
                            llvm::Instruction* pInsertPos,
                            llvm::Value**      ppFetch) const;

    bool NeedPostShuffle(const VertexInputDescription* pInputDesc,
                         std::vector<llvm::Constant*>&          shuffleMask) const;

    bool NeedPatchA2S(const VertexInputDescription* pInputDesc) const;

    bool NeedSecondVertexFetch(const VertexInputDescription* pInputDesc) const;

    // -----------------------------------------------------------------------------------------------------------------

    llvm::Module*       m_pModule;          // LLVM module
    llvm::LLVMContext*  m_pContext;         // LLVM context
    ShaderSystemValues* m_pShaderSysValues; // ShaderSystemValues object for getting vertex buffer pointer from
    PipelineState*      m_pPipelineState;   // Pipeline state

    llvm::Value*    m_pVertexIndex;     // Vertex index
    llvm::Value*    m_pInstanceIndex;   // Instance index
    llvm::Value*    m_pBaseInstance;    // Base instance
    llvm::Value*    m_pInstanceId;      // Instance ID

    static const VertexCompFormatInfo   m_vertexCompFormatInfo[];   // Info table of vertex component format
    static const BufFormat              m_vertexFormatMap[];        // Info table of vertex format mapping

    // Default values for vertex fetch (<4 x i32> or <8 x i32>)
    struct
    {
        llvm::Constant*   pInt8;      // < 0, 0, 0, 1 >
        llvm::Constant*   pInt16;     // < 0, 0, 0, 1 >
        llvm::Constant*   pInt32;     // < 0, 0, 0, 1 >
        llvm::Constant*   pInt64;     // < 0, 0, 0, 0, 0, 0, 0, 1 >
        llvm::Constant*   pFloat16;   // < 0, 0, 0, 0x3C00 >
        llvm::Constant*   pFloat32;   // < 0, 0, 0, 0x3F800000 >
        llvm::Constant*   pDouble64;  // < 0, 0, 0, 0, 0, 0, 0, 0x3FF00000 >
    } m_fetchDefaults;
};

} // lgc
