/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcContext.h
 * @brief LLPC header file: contains declaration of class Llpc::Context.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Target/TargetMachine.h"

#include <unordered_map>
#include <unordered_set>
#include "spirvExt.h"

#include "lgc/llpcBuilderContext.h"
#include "llpcEmuLib.h"
#include "llpcPipelineContext.h"

namespace Llpc
{

// =====================================================================================================================
// Represents LLPC context for pipeline compilation. Derived from the base class llvm::LLVMContext.
class Context : public llvm::LLVMContext
{
public:
    Context(GfxIpVersion gfxIp);
    ~Context();

    void Reset();

    // Checks whether this context is in use.
    bool IsInUse() const { return m_isInUse; }

    // Set context in-use flag.
    void SetInUse(bool inUse) { m_isInUse = inUse; }

    // Attaches pipeline context to LLPC context.
    void AttachPipelineContext(PipelineContext* pPipelineContext)
    {
        m_pPipelineContext = pPipelineContext;
    }

    // Gets pipeline context.
    PipelineContext* GetPipelineContext() const
    {
        return m_pPipelineContext;
    }

    // Set LLPC builder
    void SetBuilder(lgc::Builder* pBuilder) { m_pBuilder = pBuilder; }

    // Get LLPC builder
    lgc::Builder* GetBuilder() const { return m_pBuilder; }

    // Get (create if necessary) BuilderContext
    lgc::BuilderContext* GetBuilderContext();

    // Set value of scalarBlockLayout option. This gets called with the value from PipelineOptions when
    // starting a pipeline compile.
    void SetScalarBlockLayout(bool scalarBlockLayout) { m_scalarBlockLayout = scalarBlockLayout; }

    // Get value of scalarBlockLayout for front-end use. If there have been any pipeline compiles in this context,
    // then it returns the value from the most recent one. If there have not been any pipeline compiles in this
    // context yet, then it returns false.
    // TODO: This is not correct behavior. The front-end should not be using pipeline options. Possibly
    // scalarBlockLayout is a whole-device option that should be passed into LLPC in a different way.
    bool GetScalarBlockLayout() const { return m_scalarBlockLayout; }

    // Set value of robustBufferAccess option. This gets called with the value from PipelineOptions when
    // starting a pipeline compile.
    void SetRobustBufferAccess(bool robustBufferAccess) { m_robustBufferAccess = robustBufferAccess; }

    // Get value of robustBufferAccess for front-end use. If there have been any pipeline compiles in this context,
    // then it returns the value from the most recent one. If there have not been any pipeline compiles in this
    // context yet, then it returns false.
    // TODO: This is not correct behavior. The front-end should not be using pipeline options.
    bool GetRobustBufferAccess() const { return m_robustBufferAccess; }

    std::unique_ptr<llvm::Module> LoadLibary(const BinaryData* pLib);

    // Wrappers of interfaces of pipeline context
    bool IsGraphics() const
    {
        return m_pPipelineContext->IsGraphics();
    }
    const PipelineShaderInfo* GetPipelineShaderInfo(ShaderStage shaderStage) const
    {
        return m_pPipelineContext->GetPipelineShaderInfo(shaderStage);
    }

    const void* GetPipelineBuildInfo() const
    {
        return m_pPipelineContext->GetPipelineBuildInfo();
    }

    unsigned GetShaderStageMask() const
    {
        return m_pPipelineContext->GetShaderStageMask();
    }

    unsigned GetActiveShaderStageCount() const
    {
        return m_pPipelineContext->GetActiveShaderStageCount();
    }

    const char* GetGpuNameAbbreviation() const
    {
        return PipelineContext::GetGpuNameAbbreviation(m_gfxIp);
    }

    GfxIpVersion GetGfxIpVersion() const
    {
        return m_gfxIp;
    }

    void DoUserDataNodeMerge()
    {
        m_pPipelineContext->DoUserDataNodeMerge();
    }

    uint64_t GetPiplineHashCode() const
    {
        return m_pPipelineContext->GetPiplineHashCode();
    }

    uint64_t GetCacheHashCode() const
    {
        return m_pPipelineContext->GetCacheHashCode();
    }

    ShaderHash GetShaderHashCode(ShaderStage shaderStage) const
    {
        return m_pPipelineContext->GetShaderHashCode(shaderStage);
    }

    // Sets triple and data layout in specified module from the context's target machine.
    void SetModuleTargetMachine(llvm::Module* pModule);

private:
    Context() = delete;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // -----------------------------------------------------------------------------------------------------------------

    GfxIpVersion                  m_gfxIp;             // Graphics IP version info
    PipelineContext*              m_pPipelineContext;  // Pipeline-specific context
    EmuLib                        m_glslEmuLib;        // LLVM library for GLSL emulation
    volatile  bool                m_isInUse;           // Whether this context is in use
    lgc::Builder*                 m_pBuilder = nullptr; // LLPC builder object
    std::unique_ptr<lgc::BuilderContext> m_builderContext;  // Builder context

    std::unique_ptr<llvm::TargetMachine> m_pTargetMachine; // Target machine
    bool                          m_scalarBlockLayout = false;  // scalarBlockLayout option from last pipeline compile
    bool                          m_robustBufferAccess = false; // robustBufferAccess option from last pipeline compile
};

} // Llpc
