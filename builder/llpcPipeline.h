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
 * @file  llpcPipeline.h
 * @brief LLPC header file: contains declaration of class Llpc::Pipeline
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm
{

class Timer;

} // llvm

namespace Llpc
{

using namespace llvm;

class BuilderContext;

// =====================================================================================================================
// The public API of the middle-end pipeline state exposed to the front-end for setting state and linking and
// generating the pipeline
class Pipeline
{
public:
    Pipeline(BuilderContext* pBuilderContext)
        : m_pBuilderContext(pBuilderContext)
    {}

    virtual ~Pipeline() {}

    // Get BuilderContext
    BuilderContext* GetBuilderContext() const { return m_pBuilderContext; }

    // Get LLVMContext
    LLVMContext& GetContext() const;

    // -----------------------------------------------------------------------------------------------------------------
    // State setting methods

    // Set the resource mapping nodes for the pipeline. "nodes" describes the user data
    // supplied to the shader as a hierarchical table (max two levels) of descriptors.
    // "immutableDescs" contains descriptors (currently limited to samplers), whose values are hard
    // coded by the application. Each one is a duplicate of one in "nodes". A use of one of these immutable
    // descriptors in the applicable Create* method is converted directly to the constant value.
    //
    // If using a BuilderImpl, this method must be called before any Create* methods.
    // If using a BuilderRecorder, it can be delayed until after linking.
    virtual void SetUserDataNodes(
        ArrayRef<ResourceMappingNode>   nodes,            // The resource mapping nodes
        ArrayRef<DescriptorRangeValue>  rangeValues) = 0; // The descriptor range values

    // -----------------------------------------------------------------------------------------------------------------
    // Link and generate pipeline methods

    // Link the individual shader modules into a single pipeline module. The front-end must have
    // finished calling Builder::Create* methods and finished building the IR. In the case that
    // there are multiple shader modules, they are all freed by this call, and the linked pipeline
    // module is returned. If there is a single shader module, this might instead just return that.
    // Before calling this, each shader module needs to have one global function for the shader
    // entrypoint, then all other functions with internal linkage.
    // Returns the pipeline module, or nullptr on link failure.
    virtual Module* Link(
        ArrayRef<Module*> modules) = 0; // Array of modules indexed by shader stage, with nullptr entry
                                        //  for any stage not present in the pipeline

    // Typedef of function passed in to Generate to check the shader cache.
    // Returns the updated shader stage mask, allowing the client to decide not to compile shader stages
    // that got a hit in the cache.
    typedef std::function<uint32_t(
        const Module*               pModule,      // [in] Module
        uint32_t                    stageMask,    // Shader stage mask
        ArrayRef<ArrayRef<uint8_t>> stageHashes   // Per-stage hash of in/out usage
    )> CheckShaderCacheFunc;

    // Generate pipeline module by running patch, middle-end optimization and backend codegen passes.
    // The output is normally ELF, but IR disassembly if an option is used to stop compilation early.
    // Output is written to outStream.
    // Like other Builder methods, on error, this calls report_fatal_error, which you can catch by setting
    // a diagnostic handler with LLVMContext::setDiagnosticHandler.
    virtual void Generate(
        std::unique_ptr<Module>   pipelineModule,       // IR pipeline module
        raw_pwrite_stream&        outStream,            // [in/out] Stream to write ELF or IR disassembly output
        CheckShaderCacheFunc      checkShaderCacheFunc, // Function to check shader cache in graphics pipeline
        ArrayRef<Timer*>          timers) = 0;          // Timers for: patch passes, llvm optimizations, codegen

private:
    BuilderContext*                 m_pBuilderContext;                  // Builder context
};

} // Llpc
