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
 * @file  llpcConfigBuilderBase.h
 * @brief LLPC header file: contains declaration of class Llpc::ConfigBuilderBase.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcContext.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"

namespace Llpc
{

class PipelineState;

// =====================================================================================================================
// Register configuration builder base class.
class ConfigBuilderBase
{
public:
    ConfigBuilderBase(PipelineState* pPipelineState);
    ~ConfigBuilderBase();

    void WritePalMetadata();

protected:
    // Builds metadata API_HW_SHADER_MAPPING_HI/LO.
    void BuildApiHwShaderMapping(uint32_t           vsHwShader,
                                 uint32_t           tcsHwShader,
                                 uint32_t           tesHwShader,
                                 uint32_t           gsHwShader,
                                 uint32_t           fsHwShader,
                                 uint32_t           csHwShader);

    void SetShaderHash(ShaderStage apiStage, ShaderHash hash);
    void SetNumAvailSgprs(Util::Abi::HardwareStage hwStage, uint32_t value);
    void SetNumAvailVgprs(Util::Abi::HardwareStage hwStage, uint32_t value);
    void SetUsesViewportArrayIndex(bool useViewportIndex);
    void SetPsUsesUavs(bool value);
    void SetPsWritesUavs(bool value);
    void SetPsWritesDepth(bool value);
    void SetEsGsLdsByteSize(uint32_t value);
#if LLPC_BUILD_GFX10
    void SetCalcWaveBreakSizeAtDrawTime(bool value);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
    void SetWaveFrontSize(Util::Abi::HardwareStage hwStage, uint32_t value);
#endif
#endif
    void SetApiName(const char* pValue);
    void SetPipelineType(Util::Abi::PipelineType value);
    void SetLdsSizeByteSize(Util::Abi::HardwareStage hwStage, uint32_t value);
    void SetEsGsLdsSize(uint32_t value);

    // -----------------------------------------------------------------------------------------------------------------

    PipelineState*                  m_pPipelineState;     // Pipeline state
    llvm::Module*                   m_pModule;            // LLVM module being processed
    Context*                        m_pContext;           // LLPC context
    uint8_t*                        m_pConfig = nullptr;  // Register/metadata configuration
    size_t                          m_configSize = 0;     // Size of register/metadata configuration
    GfxIpVersion                    m_gfxIp;              // Graphics IP version info

    bool                            m_hasVs;              // Whether the pipeline has vertex shader
    bool                            m_hasTcs;             // Whether the pipeline has tessellation control shader
    bool                            m_hasTes;             // Whether the pipeline has tessellation evaluation shader
    bool                            m_hasGs;              // Whether the pipeline has geometry shader

    uint32_t                        m_userDataLimit;      // User data limit for shaders seen so far
    uint32_t                        m_spillThreshold;     // Spill threshold for shaders seen so far

private:
    // Get the MsgPack map node for the specified API shader in the ".shaders" map
    llvm::msgpack::MapDocNode GetApiShaderNode(uint32_t apiStage);
    // Get the MsgPack map node for the specified HW shader in the ".hardware_stages" map
    llvm::msgpack::MapDocNode GetHwShaderNode(Util::Abi::HardwareStage hwStage);
    // Set USER_DATA_LIMIT (called once for the whole pipeline)
    void SetUserDataLimit();
    // Set SPILL_THRESHOLD (called once for the whole pipeline)
    void SetSpillThreshold();
    // Set PIPELINE_HASH (called once for the whole pipeline)
    void SetPipelineHash();

    // -----------------------------------------------------------------------------------------------------------------
    std::unique_ptr<llvm::msgpack::Document>  m_document;       // The MsgPack document
    llvm::msgpack::MapDocNode                 m_pipelineNode;   // MsgPack map node for amdpal.pipelines[0]
    llvm::msgpack::MapDocNode                 m_apiShaderNodes[ShaderStageNativeStageCount];
                                                                // MsgPack map node for each API shader's node in
                                                                //  ".shaders"
    llvm::msgpack::MapDocNode                 m_hwShaderNodes[uint32_t(Util::Abi::HardwareStage::Count)];
                                                                // MsgPack map node for each HW shader's node in
                                                                //  ".hardware_stages"
};

} // Llpc
