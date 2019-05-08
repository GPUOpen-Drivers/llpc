/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#define DEBUG_TYPE "llpc-context"

#include <metrohash.h>

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitstreamReader.h"
#include "llvm/Bitcode/BitstreamWriter.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llpcCompiler.h"
#include "llpcContext.h"
#include "llpcShaderCache.h"
#include "llpcShaderCacheManager.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
const uint8_t Context::GlslEmuLib[]=
{
    #include "./generate/g_llpcGlslEmuLib.h"
};

const uint8_t Context::GlslEmuLibGfx8[] =
{
#include "./generate/gfx8/g_llpcGlslEmuLibGfx8.h"
};

const uint8_t Context::GlslEmuLibGfx9[]=
{
    #include "./generate/gfx9/g_llpcGlslEmuLibGfx9.h"
};

const uint8_t Context::GlslEmuLibWaTreat1dImagesAs2d[] =
{
    #include "./generate/wa/g_llpcGlslEmuLibTreat1dImagesAs2d.h"
};

// =====================================================================================================================
Context::Context(
    GfxIpVersion gfxIp,                     // Graphics IP version info
    const WorkaroundFlags* pGpuWorkarounds) // GPU workarounds
    :
    LLVMContext(),
    m_gfxIp(gfxIp),
    m_glslEmuLib(this)
{
    std::vector<Metadata*> emptyMeta;
    m_pEmptyMetaNode = MDNode::get(*this, emptyMeta);

    // Initialize pre-constructed LLVM derived types
    m_tys.pBoolTy     = Type::getInt1Ty(*this);
    m_tys.pInt8Ty     = Type::getInt8Ty(*this);
    m_tys.pInt16Ty    = Type::getInt16Ty(*this);
    m_tys.pInt32Ty    = Type::getInt32Ty(*this);
    m_tys.pInt64Ty    = Type::getInt64Ty(*this);
    m_tys.pFloat16Ty  = Type::getHalfTy(*this);
    m_tys.pFloatTy    = Type::getFloatTy(*this);
    m_tys.pDoubleTy   = Type::getDoubleTy(*this);
    m_tys.pVoidTy     = Type::getVoidTy(*this);

    m_tys.pInt16x2Ty    = VectorType::get(m_tys.pInt16Ty, 2);
    m_tys.pInt32x2Ty    = VectorType::get(m_tys.pInt32Ty, 2);
    m_tys.pInt32x3Ty    = VectorType::get(m_tys.pInt32Ty, 3);
    m_tys.pInt32x4Ty    = VectorType::get(m_tys.pInt32Ty, 4);
    m_tys.pInt32x6Ty    = VectorType::get(m_tys.pInt32Ty, 6);
    m_tys.pInt32x8Ty    = VectorType::get(m_tys.pInt32Ty, 8);
    m_tys.pFloat16x2Ty  = VectorType::get(m_tys.pFloat16Ty, 2);
    m_tys.pFloat16x4Ty  = VectorType::get(m_tys.pFloat16Ty, 4);
    m_tys.pFloatx2Ty    = VectorType::get(m_tys.pFloatTy, 2);
    m_tys.pFloatx3Ty    = VectorType::get(m_tys.pFloatTy, 3);
    m_tys.pFloatx4Ty    = VectorType::get(m_tys.pFloatTy, 4);

    // Initialize IDs of pre-declared LLVM metadata
    m_metaIds.invariantLoad = getMDKindID("invariant.load");
    m_metaIds.range         = getMDKindID("range");
    m_metaIds.uniform       = getMDKindID("amdgpu.uniform");

    // Load external LLVM libraries, in search order.
    if (pGpuWorkarounds->gfx9.treat1dImagesAs2d)
    {
        // Add library for "treat 1d image as 2d gpu workaround".
        m_glslEmuLib.AddArchive(MemoryBufferRef(StringRef(reinterpret_cast<const char*>(GlslEmuLibWaTreat1dImagesAs2d),
                sizeof(GlslEmuLibWaTreat1dImagesAs2d)), "GlslEmuLibWaTreat1dImagesAs2d"));
    }

    if (gfxIp.major >= 9)
    {
        m_glslEmuLib.AddArchive(MemoryBufferRef(StringRef(reinterpret_cast<const char*>(GlslEmuLibGfx9),
                sizeof(GlslEmuLibGfx9)), "GlslEmuLibGfx9"));
    }
    if (gfxIp.major >= 8)
    {
        m_glslEmuLib.AddArchive(MemoryBufferRef(StringRef(reinterpret_cast<const char*>(GlslEmuLibGfx8),
                sizeof(GlslEmuLibGfx8)), "GlslEmuLibGfx8"));
    }
    m_glslEmuLib.AddArchive(MemoryBufferRef(StringRef(reinterpret_cast<const char*>(GlslEmuLib),
            sizeof(GlslEmuLib)), "GlslEmuLib"));
}

// =====================================================================================================================
Context::~Context()
{
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
    pModule->setTargetTriple(GetTargetMachine()->getTargetTriple().getTriple());
    pModule->setDataLayout(GetTargetMachine()->createDataLayout());
}

} // Llpc
