/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderContext.h
 * @brief LLPC header file: declaration of llpc::BuilderContext class for creating and using lgc::Builder
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"

namespace llvm
{

class LLVMContext;
class ModulePass;
class raw_pwrite_stream;
class TargetMachine;
class Timer;

namespace legacy
{

class PassManager;

} // legacy

} // llvm

namespace lgc
{

using namespace llvm;

class Builder;
class PassManager;
class Pipeline;
class TargetInfo;

// =====================================================================================================================
// BuilderContext class, used to create Pipeline and Builder objects. State shared between multiple compiles
// is kept here.
class BuilderContext
{
public:
    // Initialize the middle-end. This must be called before the first BuilderContext::Create, although you are
    // allowed to call it again after that. It must also be called before LLVM command-line processing, so
    // that you can use a pass name in an option such as -print-after. If multiple concurrent compiles are
    // possible, this should be called in a thread-safe way.
    static void Initialize();

    // Create the BuilderContext. Returns nullptr on failure to recognize the AMDGPU target whose name is specified
    static BuilderContext* Create(
        LLVMContext&  context,               // [in] LLVM context to use on all compiles
        StringRef     gpuName,               // LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
        uint32_t      palAbiVersion);        // PAL pipeline ABI version to compile for

    ~BuilderContext();

    // Get LLVM context
    LLVMContext& GetContext() const { return m_context; }

    // Get the target machine.
    TargetMachine* GetTargetMachine() const { return m_pTargetMachine; }

    // Get targetinfo
    const TargetInfo& GetTargetInfo() const { return *m_pTargetInfo; }

    // Get the PAL pipeline ABI version to compile for
    uint32_t GetPalAbiVersion() const { return m_palAbiVersion; }

    // Create a Pipeline object for a pipeline compile
    Pipeline* CreatePipeline();

    // Create a Builder object. For a shader compile (pPipelineState is nullptr), useBuilderRecorder is ignored
    // because it always uses BuilderRecorder.
    Builder* CreateBuilder(
        Pipeline*  pPipeline,           // [in] Pipeline object for pipeline compile, nullptr for shader compile
        bool       useBuilderRecorder); // True to use BuilderRecorder, false to use BuilderImpl

    // Prepare a pass manager. This manually adds a target-aware TLI pass, so middle-end optimizations do not
    // think that we have library functions.
    void PreparePassManager(
        legacy::PassManager*  pPassMgr);  // [in/out] Pass manager

    // Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
    void AddTargetPasses(lgc::PassManager& passMgr, Timer* pCodeGenTimer, raw_pwrite_stream& outStream);

    void SetBuildRelocatableElf(bool buildRelocatableElf) { m_buildRelocatableElf = buildRelocatableElf; }
    bool BuildingRelocatableElf() { return m_buildRelocatableElf; }

    // Utility method to create a start/stop timer pass
    static ModulePass* CreateStartStopTimer(Timer* pTimer, bool starting);

    // Set and get a pointer to the stream used for LLPC_OUTS. This is initially nullptr,
    // signifying no output from LLPC_OUTS. Setting this to a stream means that LLPC_OUTS
    // statements in the middle-end output to that stream, giving a dump of LLVM IR at a
    // few strategic places in the pass flow, as well as information such as input/output
    // mapping.
    static void SetLlpcOuts(raw_ostream* pStream) { m_pLlpcOuts = pStream; }
    static raw_ostream* GetLgcOuts() { return m_pLlpcOuts; }

private:
    BuilderContext() = delete;
    BuilderContext(const BuilderContext&) = delete;
    BuilderContext& operator =(const BuilderContext&) = delete;

    BuilderContext(LLVMContext& context, uint32_t palAbiVersion);

    // -----------------------------------------------------------------------------------------------------------------
    static raw_ostream*        m_pLlpcOuts;                   // nullptr or stream for LLPC_OUTS
    LLVMContext&               m_context;                     // LLVM context
    TargetMachine*             m_pTargetMachine = nullptr;    // Target machine
    TargetInfo*                m_pTargetInfo = nullptr;       // Target info
    bool                       m_buildRelocatableElf = false; // Flag indicating whether we are building relocatable ELF
    uint32_t                   m_palAbiVersion = 0xFFFFFFFF;  // PAL pipeline ABI version to compile for
};

} // lgc
