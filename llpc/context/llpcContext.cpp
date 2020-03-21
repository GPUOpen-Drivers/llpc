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
 * @file  llpcContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Context.
 ***********************************************************************************************************************
 */
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "SPIRVInternal.h"

#include "llpcBuilder.h"
#include "llpcCompiler.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcMetroHash.h"
#include "llpcPipelineContext.h"
#include "llpcShaderCache.h"
#include "llpcShaderCacheManager.h"

#define DEBUG_TYPE "llpc-context"

using namespace lgc;
using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
Context::Context(
    GfxIpVersion gfxIp)                     // Graphics IP version info
    :
    LLVMContext(),
    m_gfxIp(gfxIp),
    m_glslEmuLib(this)
{
    Reset();
}

// =====================================================================================================================
Context::~Context()
{
}

// =====================================================================================================================
void Context::Reset()
{
    m_pPipelineContext = nullptr;
    delete m_pBuilder;
    m_pBuilder = nullptr;
}

// =====================================================================================================================
// Get (create if necessary) BuilderContext
BuilderContext* Context::GetBuilderContext()
{
    if (!m_builderContext)
    {
        // First time: Create the BuilderContext.
        std::string gpuName;
        PipelineContext::GetGpuNameString(m_gfxIp, gpuName);
        m_builderContext.reset(BuilderContext::Create(*this, gpuName));
        if (!m_builderContext)
        {
            report_fatal_error(Twine("Unknown target '") + Twine(gpuName) + Twine("'"));
        }
    }
    return &*m_builderContext;
}

// =====================================================================================================================
// Loads library from external LLVM library.
std::unique_ptr<Module> Context::LoadLibary(
    const BinaryData* pLib)     // [in] Bitcodes of external LLVM library
{
    auto pMemBuffer = MemoryBuffer::getMemBuffer(
        StringRef(static_cast<const char*>(pLib->pCode), pLib->codeSize), "", false);

    Expected<std::unique_ptr<Module>> moduleOrErr =
        getLazyBitcodeModule(pMemBuffer->getMemBufferRef(), *this);

    std::unique_ptr<Module> pLibModule = nullptr;
    if (!moduleOrErr)
    {
        Error error = moduleOrErr.takeError();
        LLPC_ERRS("Fails to load LLVM bitcode \n");
    }
    else
    {
        pLibModule = std::move(*moduleOrErr);
        if (llvm::Error errCode = pLibModule->materializeAll())
        {
            LLPC_ERRS("Fails to materialize \n");
            pLibModule = nullptr;
        }
    }

    return pLibModule;
}

// =====================================================================================================================
// Sets triple and data layout in specified module from the context's target machine.
void Context::SetModuleTargetMachine(
    Module* pModule)  // [in/out] Module to modify
{
    TargetMachine* pTargetMachine = GetBuilderContext()->GetTargetMachine();
    pModule->setTargetTriple(pTargetMachine->getTargetTriple().getTriple());
    pModule->setDataLayout(pTargetMachine->createDataLayout());
}

} // Llpc
