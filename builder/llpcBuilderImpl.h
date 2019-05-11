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
 * @file  llpcBuilderImpl.h
 * @brief LLPC header file: declaration of Llpc::Builder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"

namespace Llpc
{

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : public Builder
{
public:
    BuilderImplBase(llvm::LLVMContext& context) : Builder(context) {}

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplBase)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplBase)
};

// =====================================================================================================================
// Builder implementation subclass for descriptors
class BuilderImplDesc : virtual public BuilderImplBase
{
public:
    BuilderImplDesc(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a waterfall loop containing the specified instruction.
    llvm::Instruction* CreateWaterfallLoop(llvm::Instruction*       pNonUniformInst,
                                           llvm::ArrayRef<uint32_t> operandIdxs,
                                           const llvm::Twine&       instName) override final;

    // Create a load of a buffer descriptor.
    llvm::Value* CreateLoadBufferDesc(uint32_t            descSet,
                                      uint32_t            binding,
                                      llvm::Value*        pDescIndex,
                                      bool                isNonUniform,
                                      llvm::Type*         pPointeeTy,
                                      const llvm::Twine&  instName) override final;

    // Create a load of a sampler descriptor. Returns a <4 x i32> descriptor.
    llvm::Value* CreateLoadSamplerDesc(uint32_t            descSet,
                                       uint32_t            binding,
                                       llvm::Value*        pDescIndex,
                                       bool                isNonUniform,
                                       const llvm::Twine&  instName) override final;

    // Create a load of a resource descriptor. Returns a <8 x i32> descriptor.
    llvm::Value* CreateLoadResourceDesc(uint32_t            descSet,
                                        uint32_t            binding,
                                        llvm::Value*        pDescIndex,
                                        bool                isNonUniform,
                                        const llvm::Twine&  instName) override final;

    // Create a load of a texel buffer descriptor. Returns a <4 x i32> descriptor.
    llvm::Value* CreateLoadTexelBufferDesc(uint32_t            descSet,
                                           uint32_t            binding,
                                           llvm::Value*        pDescIndex,
                                           bool                isNonUniform,
                                           const llvm::Twine&  instName) override final;

    // Create a load of a F-mask descriptor. Returns a <8 x i32> descriptor.
    llvm::Value* CreateLoadFmaskDesc(uint32_t            descSet,
                                     uint32_t            binding,
                                     llvm::Value*        pDescIndex,
                                     bool                isNonUniform,
                                     const llvm::Twine&  instName) override final;

    // Create a load of the spill table pointer.
    llvm::Value* CreateLoadSpillTablePtr(llvm::Type*         pSpillTableTy,
                                         const llvm::Twine&  instName) override final;

    // Create a buffer length query based on the specified descriptor.
    llvm::Value* CreateBufferLength(llvm::Value* const pBufferDesc,
                                    const llvm::Twine& instName = "") override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplDesc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplDesc)

    llvm::Value* ScalarizeIfUniform(llvm::Value* pValue, bool isNonUniform);
};

// =====================================================================================================================
// Builder implementation subclass for matrix operations
class BuilderImplMatrix : virtual public BuilderImplBase
{
public:
    BuilderImplMatrix(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a matrix transpose.
    llvm::Value* CreateMatrixTranspose(llvm::Value* const pMatrix,
                                       const llvm::Twine& instName = "") override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMatrix)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMatrix)
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class BuilderImplMisc : virtual public BuilderImplBase
{
public:
    BuilderImplMisc(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a "kill". Only allowed in a fragment shader.
    llvm::Instruction* CreateKill(const llvm::Twine& instName = "") override final;

    // Create a "readclock".
    llvm::Instruction* CreateReadClock(bool realtime, const llvm::Twine& instName = "") override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMisc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMisc)
};

// =====================================================================================================================
// The Builder implementation, encompassing all the individual builder implementation subclasses
class BuilderImpl final : public BuilderImplDesc, public BuilderImplMatrix, public BuilderImplMisc
{
public:
    BuilderImpl(llvm::LLVMContext& context) : BuilderImplBase(context),
                                              BuilderImplDesc(context),
                                              BuilderImplMatrix(context),
                                              BuilderImplMisc(context)
    {}
    ~BuilderImpl() {}

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImpl)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImpl)
};

} // Llpc
